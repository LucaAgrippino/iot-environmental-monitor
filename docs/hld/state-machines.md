# HLD Artefact #4 — State Machine Specification

This document specifies the runtime state machines of both nodes of the IoT
Environmental Monitoring Gateway system. It is the textual companion to the
state machine diagrams produced in Visual Paradigm under
`02-architecture/state-machines/` and exported to `docs/diagrams/`.

The document is organised as follows:

1. **Conventions** — notation, owner-component mapping, and diagram-reading
   rules used across all machines.
2. **Six machine sections** — one per state machine, each with a purpose
   paragraph, the diagram, the state list with entry / do / exit semantics,
   the transition table, and a traceability summary.
3. **Cross-machine relationships** — how the six machines couple at runtime.
4. **Decisions and open questions** — design choices made during the
   derivation, together with TBDs deferred to LLD.

## Inventory

| #   | Machine                       | Owner component                            | Board         |
|-----|-------------------------------|--------------------------------------------|---------------|
| 1   | Gateway Lifecycle             | `LifecycleController` (Application)        | Gateway       |
| 2   | Cloud Connectivity            | `CloudPublisher` (Application)             | Gateway       |
| 3   | Firmware Update               | `UpdateService` (Application)              | Gateway       |
| 4   | Modbus Master                 | `ModbusPoller` (Application; protocol-level frame handling delegated to `ModbusMaster` Middleware) | Gateway       |
| 5   | Field Device Lifecycle        | `LifecycleController` (Application)        | Field Device  |
| 6   | Modbus Slave                  | `ModbusSlave` (Middleware)                 | Field Device  |

A seventh candidate — a per-channel alarm state machine (Clear ↔ Active with
hysteresis) — is deliberately deferred to LLD. Reasoning given in
[§ Decisions and open questions](#decisions-and-open-questions).

## Conventions

### UML notation

- **Transition labels** follow the form `event [guard] / action`. Any of the
  three may be empty.
- **Multiple triggers** that share a single transition are listed
  comma-separated: `event_a, event_b / action`.
- **Pseudo-states** used: initial (filled black circle), final (concentric
  circles), choice (diamond), entry-point (hollow circle on the boundary).
- **Composite states** are drawn as outer boxes containing inner sub-state
  diagrams. A transition attached to the outer boundary applies to every
  sub-state inside.
- **Sub-machine references** — when a state on a parent diagram delegates to
  a separate machine, it carries the `«submachine»` stereotype.
- **MCU reset** — final pseudo-states annotated `MCU reset (→ Init)` mark
  transitions whose action triggers `NVIC_SystemReset()`. They are not
  terminal in the lifecycle sense — the next boot re-enters via the
  initial pseudo-state of the relevant top-level lifecycle.

### Behavioural compartments

State machine diagrams in this project show **structural transitions only**.
Internal transitions, entry / do / exit actions, and other behavioural
compartments are listed in this document — not on the diagrams. The
diagrams answer *"what states exist and how do they connect?"*; this
document answers *"what does each state actually do?"*. Separating the two
keeps the diagrams readable and the behavioural specification authoritative
in one place.

The single exception is the **`«submachine»` stereotype** label, which
appears on the diagram itself for navigation.

### Subsystem ownership and colour

Per `docs/diagram-colour-palette.md` §3:

- Gateway machines (1–4) use the green family.
- Field Device machines (5–6) use the blue family.
- Faulted states carry a red border on top of the subsystem fill.
- Composite outer containers use a lighter shade than their sub-states.

---

# Machine 1 — Gateway Lifecycle

**Owned by:** `LifecycleController` (gateway, Application layer).

![Gateway Lifecycle](../diagrams/state-machine-gateway-lifecycle.png)

## Purpose

Models the gateway's overall mode: booting through the sequenced Init phase,
running normally, accepting CLI provisioning, executing a remote restart,
delegating to the Firmware Update sub-machine, or stopping in Faulted on
unrecoverable conditions. The richest top-level machine in the system,
because the gateway has the most distinct operational modes.

A key architectural decision is encoded here: the gateway lifecycle has no
"Degraded" state, even though cloud connectivity may be lost at runtime.
Per REQ-NF-200, cloud loss does not change the gateway's top-level mode —
it only changes the Cloud Connectivity sub-machine's state. Surfacing the
distinction here would duplicate logic the sub-machine already owns.

## State list

### Init — composite

Bring all subsystems up, validate post-reset integrity, gate entry to
Operational on a self-check.

- **Entry action:** record reset cause from RCC; start Init time-budget
  timer.
- **Do activity:** progress through sub-states below.
- **Exit action:** stop Init time-budget timer.

| # | Sub-state           | Entry action | Do activity                                                             | Exit action | Failure handling                                            |
|---|---------------------|--------------|-------------------------------------------------------------------------|-------------|-------------------------------------------------------------|
| 1 | CheckingIntegrity   | none         | Verify config / buffer / firmware partitions are not partially written  | none        | Unrecoverable corruption → Faulted (REQ-NF-214)             |
| 2 | LoadingConfig       | none         | `ConfigStore.read()`; fall back to defaults on failure                  | none        | Partial fallback OK, never fatal (SA-010, -020, -050)       |
| 3 | BringingUpSensors   | none         | Initialise on-board sensors with manufacturer defaults                  | none        | ≥1 sensor OK → continue; all fail → Faulted                 |
| 4 | StartingMiddleware  | none         | Trigger Cloud Connectivity, NTP, Modbus Master sub-machines             | none        | Sub-machines own their own failure recovery                 |
| 5 | SelfChecking        | none         | Verify sensors readable and Modbus link present (REQ-DM-040)            | none        | Pass → Operational; Fail → Faulted                          |

**Time budgets:**
- Cold boot ≤ [TBD]s (REQ-NF-213) — includes WiFi association.
- Watchdog reset ≤ 5s (REQ-NF-202).
- Normal/remote reset ≤ 5s (REQ-NF-203).

**Note on NTP:** completion is not a precondition for SelfChecking. The
system reaches Operational regardless of NTP success — TimeProvider provides
uptime-based timestamps until sync arrives (REQ-TS-040).

**Traceability:** SA-000, -010, -020, -031, -040, -050, -060; TS-000, -020;
DM-030, -040; NF-202, -203, -213, -214; CC-050.

### Operational — simple

The gateway does its job: sensors, alarms, Modbus polling, publishing,
remote commands, CLI diagnostics.

- **Entry action:** if `restart_flag` is set in the NV flags region,
  publish "restart success" to cloud and clear the flag (REQ-DM-030);
  start all periodic timers.
- **Do activity:** run periodic activities listed in the internal-transition
  table below.
- **Exit action:** stop periodic timers; flush in-flight Modbus transaction.

**Traceability:** SA-071, -080, -090, -100, -110..160, -170;
AM-000, -010, -011, -020, -030; CC-000, -010, -020, -030, -040;
TS-000 (periodic), -030, -040; DM-000, -001, -002, -090;
LI-000..020, -130..160; NF-200, -205, -210, -211, -212.

**Note (REQ-NF-205):** Sensor reinitialisation on failure is handled
within `SensorService`'s internal recovery loop; the lifecycle machine
stays in Operational. `SensorService` emits `unrecoverable_error` only
if reinitialisation fails permanently → T15 (→ Faulted).

### EditingConfig — simple

Field Technician is provisioning the gateway via CLI. Sensor acquisition,
alarm evaluation, and Modbus polling continue (handled by their own tasks
and sub-machines below the lifecycle); but configuration parameters take
effect only on confirmation, with rollback on failure.

- **Entry action:** start confirmation-timeout timer.
- **Do activity:** receive parameter inputs; validate each (LI-090);
  display feedback; await confirmation.
- **Exit action:** close provisioning menu; clear timeout timer.

*Snapshot, apply, and restore are owned exclusively by the incoming and
outgoing transitions (T10, T11, T12) to avoid double invocation.*

**Traceability:** LI-030, -040, -050, -060, -080, -090, -100, -110, -120,
-0E2, -0E3; DM-090.

### Restarting — simple

Persist state and trigger a controlled MCU reset on remote command.

- **Entry action:** persist pending writes (`ConfigStore.flush()`, buffer
  flush); log restart cause; signal LED pattern; set `restart_flag` in NV
  flags region (read by Operational entry on the next boot for DM-030).
- **Do activity:** wait briefly for in-flight MQTT publish (bounded
  timeout); trigger MCU reset.
- **Exit action:** none — the MCU resets; "exit" is the reset itself.

The restart-confirmation request (REQ-DM-020) lives in Operational, not
Restarting. We only enter Restarting *after* confirmation. If declined
(REQ-DM-021), no transition fires.

**Traceability:** UC-17; DM-010, -020, -021, -030. *(DM-040 traces to Init.SelfChecking on the subsequent boot.)*

### UpdatingFirmware — simple at top, composite in the Firmware Update machine

Delegates the update lifecycle to Machine 3 (Firmware Update). Marked
`«submachine»` on the diagram.

- **Entry action:** acquire firmware-update mutex; suspend periodic
  telemetry publishing; log entry.
- **Do activity:** delegated entirely to the Firmware Update sub-machine.
- **Exit action:** release firmware-update mutex.

*Cloud outcome reporting (REQ-DM-055) is owned exclusively by T18,
T19, and T20 to avoid multiple sends on the same event.*

**Traceability at this level:** UC-18; DM-054, DM-055.

### Faulted — simple, terminal-ish

Unrecoverable condition detected. Stay in a safe, observable state until
external/watchdog reset.

- **Entry action:** `log_fault(<cause>)`; stop all periodic timers; capture
  fault context to flash log; set fault-indicator LED pattern; if cloud
  still up, attempt single best-effort fault report (no retry — the system
  will reset before retrying).
- **Do activity:** maintain LED pattern; refuse to start any normal
  operation.
- **Exit action:** none — exit only via hardware/watchdog reset.

**Traceability:** Cross-cutting in §3.2 Reliability.

## Transition table

### State transitions

| #   | From                       | To                          | Event                                          | Guard                                          | Action                                                     |
|-----|----------------------------|-----------------------------|------------------------------------------------|------------------------------------------------|------------------------------------------------------------|
| T1  | `[*]`                      | Init.CheckingIntegrity      | —                                              | —                                              | —                                                          |
| T2  | Init.CheckingIntegrity     | Init.LoadingConfig          | `integrity_check_done`                         | `[partitions_ok]`                              | —                                                          |
| T3  | Init.CheckingIntegrity     | Faulted                     | `integrity_check_done`                         | `[!partitions_ok]`                             | —                                                          |
| T4  | Init.LoadingConfig         | Init.BringingUpSensors      | `config_load_done`                             | —                                              | —                                                          |
| T5  | Init.BringingUpSensors     | Init.StartingMiddleware     | `sensor_init_done`                             | `[≥1 sensor available]`                        | —                                                          |
| T6  | Init.BringingUpSensors     | Faulted                     | `sensor_init_done`                             | `[no sensor available]`                        | —                                                          |
| T7  | Init.StartingMiddleware    | Init.SelfChecking           | `middleware_started`                           | —                                              | —                                                          |
| T8  | Init.SelfChecking          | Operational                 | `self_check_done`                              | `[self_check_ok]`                              | —                                                          |
| T9  | Init.SelfChecking          | Faulted                     | `self_check_done`                              | `[!self_check_ok]`                             | —                                                          |
| T10 | Operational                | EditingConfig               | `cli_provision_entered`                        | —                                              | `snapshot_config(); show_provision_menu()`                 |
| T11 | EditingConfig              | Operational                 | `confirmation_received`                        | `[validation_ok]`                              | `apply_and_persist(); emit internet_params_changed`        |
| T12 | EditingConfig              | Operational                 | `cancel_received, confirmation_timeout`        | —                                              | `restore_snapshot()`                                       |
| T13 | Operational                | Restarting                  | `restart_cmd_received`                         | `[user_confirmed]`                             | `persist_state(); log(restart_requested)`                  |
| T14 | Operational                | UpdatingFirmware            | `ota_cmd_received`                             | `[authenticated && !update_in_progress]`       | `acquire_update_mutex(); emit update_started`              |
| T15 | Operational                | Faulted                     | `watchdog_imminent, unrecoverable_error`       | —                                              | —                                                          |
| T16 | EditingConfig              | Faulted                     | `watchdog_imminent, unrecoverable_error`       | —                                              | `restore_snapshot()`                                       |
| T17 | Restarting                 | (MCU reset)                 | `reset_triggered`                              | —                                              | `NVIC_SystemReset()`                                       |
| T18 | UpdatingFirmware           | Restarting                  | `update_done`                                  | `[update_success]`                             | `report_to_cloud(success)`                                 |
| T19 | UpdatingFirmware           | Operational                 | `update_done`                                  | `[update_failed && rollback_ok]`               | `report_to_cloud(failed_rolled_back)`                      |
| T20 | UpdatingFirmware           | Faulted                     | `update_done`                                  | `[update_failed && !rollback_ok]`              | `report_to_cloud(failed_unrecoverable)`                    |
| T21 | Faulted                    | (MCU reset)                 | `watchdog_expiry, external_reset`              | —                                              | —                                                          |

*Mutex release for T18/T19/T20 is handled by the UpdatingFirmware exit action (see §UpdatingFirmware state).*

### Internal transitions in UpdatingFirmware

| #   | Event             | Guard | Action               |
|-----|-------------------|-------|----------------------|
| I18 | `reset_requested` | —     | `NVIC_SystemReset()` |

*Machine 3 emits `reset_requested` instead of calling `NVIC_SystemReset()` directly. The Gateway Lifecycle owns the reset decision; this internal transition is the single canonical reset site during the OTA lifecycle. The UpdatingFirmware exit action does not fire for I18 (internal transitions do not exit the state), which is correct — the MCU restarts and re-enters Init, which detects the persisted flag and resumes Machine 3 at the appropriate entry-point.*

### Internal transitions in Operational

| #   | Event                              | Guard                  | Action                                                      |
|-----|------------------------------------|------------------------|-------------------------------------------------------------|
| I1  | `polling_timer_tick`               | —                      | `SensorService.read_cycle()`                                |
| I2  | `new_processed_reading`            | —                      | `AlarmService.evaluate()` (AM-000, -010, -011)              |
| I3  | `alarm_event`                      | —                      | `CloudPublisher.publish_alarm()` (AM-020, -030, CC-020)     |
| I4  | `telemetry_timer`                  | —                      | `CloudPublisher.publish_telemetry()` (CC-000, -030)         |
| I5  | `health_timer`                     | —                      | `CloudPublisher.publish_health()` (CC-010, -040)            |
| I6  | `ntp_resync_timer`                 | —                      | `TimeProvider.resync()` (NF-210)                            |
| I7  | `time_push_timer`                  | —                      | `ModbusMaster.write_time_to_field_device()` (TS-030, NF-211)|
| I8  | `remote_read_received`             | —                      | `SensorService.read_now()` (SA-170)                         |
| I9  | `remote_config_received`           | `[validation_ok]`      | `apply_config(); persist(); ack_cloud(ok)` (DM-000..002, -090)|
| I10 | `remote_config_received`           | `[!validation_ok]`     | `ack_cloud(rejected)`                                       |
| I11 | `restart_cmd_received`             | `[!user_confirmed]`    | `discard_cmd()` (DM-021)                                    |
| I12 | `ota_cmd_received`                 | `[!authenticated]`     | `reject_cmd()`                                              |
| I13 | `ota_cmd_received`                 | `[update_in_progress]` | `reject_cmd()` (DM-054)                                     |
| I14 | `cli_diagnostic_received`          | `[cmd_recognised]`     | `dispatch_and_respond()` (LI-000..020, -130..160)           |
| I15 | `cli_diagnostic_received`          | `[!cmd_recognised]`    | `display_error()` (LI-0E1)                                  |

### Internal transitions in EditingConfig

| #   | Event             | Guard               | Action                                              |
|-----|-------------------|---------------------|-----------------------------------------------------|
| I16 | `cli_param_input` | `[validation_ok]`   | `accept_input(); show_on_cli()`                     |
| I17 | `cli_param_input` | `[!validation_ok]`  | `reject(); show_error()` (LI-0E2)                   |

---

# Machine 2 — Cloud Connectivity

**Owned by:** `CloudPublisher` (gateway, Application layer).

![Cloud Connectivity](../diagrams/state-machine-cloud-connectivity.png)

## Purpose

Models the gateway's relationship with AWS IoT Core: connect, publish, lose
connection, reconnect, drain buffer. Connectivity loss is owned here, not
in the gateway top-level lifecycle (per REQ-NF-200) — the gateway stays in
Operational while this sub-machine flips to Disconnected.

## State list

### Connecting — simple, initial

Establish the MQTT/TLS session — at boot, or recovering from Disconnected.

- **Entry action:** `mqtt_client.connect()` (TLS handshake, X.509 cert
  authentication, REQ-CC-050).
- **Do activity:** await connect outcome.
- **Exit action:** none.

**Traceability:** CC-050; CC-060 (recovery path).

### Connected — composite

MQTT session is up; gateway publishes outbound messages.

- **Entry action:** reset retry counter; emit `internet_restored` event
  (REQ-TS-0E1); choice point picks initial sub-state based on buffer
  occupancy.
- **Do activity:** delegated to sub-state.
- **Exit action:** none.

#### Connected.Draining — simple

Reconnecting after a disconnection. Drain the store-and-forward buffer
in chronological order before resuming live publishing.

- **Entry action:** `start_drain()` — dequeue oldest from
  CircularFlashLog.
- **Do activity:** publish each buffered message in order; on each ack,
  remove from buffer; continue until buffer empty.
- **Exit action:** none — transitions to Publishing on `buffer_empty`.

**Traceability:** REQ-BF-010.

#### Connected.Publishing — simple

Steady state. Live messages from CloudPublisher are forwarded to MQTT
directly.

- **Entry action:** none.
- **Do activity:** publish telemetry / health / alarm messages as they
  arrive (CC-000, -010, -020, -030, -040).
- **Exit action:** none.

**Traceability:** CC-000, CC-010, CC-020, CC-030, CC-040.

### Disconnected — simple

MQTT session is down. Buffer outbound messages and retry connect at 1 Hz.

- **Entry action:** start 1 Hz `reconnect_timer` (REQ-NF-209).
- **Do activity:** enqueue outbound messages to CircularFlashLog
  (REQ-BF-000); if buffer full, drop oldest and enqueue (REQ-BF-020).
- **Exit action:** stop `reconnect_timer`.

**Traceability:** BF-000, BF-020, NF-209.

## Transition table

### State transitions

| #   | From                  | To                                | Event                       | Guard                  | Action                                |
|-----|-----------------------|-----------------------------------|-----------------------------|------------------------|---------------------------------------|
| T1  | `[*]`                 | Connecting                        | —                           | —                      | —                                     |
| T2  | Connecting            | (choice inside Connected)         | `connect_success`           | —                      | —                                     |
| T3  | Connecting            | Disconnected                      | `connect_failure`           | —                      | —                                     |
| T4  | choice                | Connected.Draining                | —                           | `[!buffer_empty]`      | —                                     |
| T5  | choice                | Connected.Publishing              | —                           | `[buffer_empty]`       | —                                     |
| T6  | Connected (boundary)  | Disconnected                      | `connection_lost`           | —                      | —                                     |
| T7  | Connected (boundary)  | Disconnected                      | `internet_params_changed`   | —                      | `force_close_session()`               |
| T8  | Disconnected          | Connecting                        | `reconnect_timer_tick`      | —                      | —                                     |
| T9  | Connected.Draining    | Connected.Publishing              | `buffer_empty`              | —                      | —                                     |

### Internal transitions

| #   | In state              | Event             | Guard                | Action                                  |
|-----|-----------------------|-------------------|----------------------|-----------------------------------------|
| I1  | Connecting            | `new_message`     | —                    | `enqueue(msg)`                          |
| I2  | Disconnected          | `new_message`     | `[!buffer_full]`     | `enqueue(msg)` (BF-000)                 |
| I3  | Disconnected          | `new_message`     | `[buffer_full]`      | `drop_oldest(); enqueue(msg)` (BF-020)  |
| I4  | Connected.Publishing  | `new_message`     | —                    | `mqtt_publish(msg)`                     |
| I5  | Connected.Draining    | `new_message`     | —                    | `enqueue(msg)` (preserve order)         |

I5 enqueues rather than publishing because, while draining the buffer, new
live messages must wait their turn in chronological order. Publishing them
immediately would violate REQ-BF-010 ordering.

---

# Machine 3 — Firmware Update

**Owned by:** `UpdateService` (gateway, Application layer).

![Firmware Update](../diagrams/state-machine-firmware-update.png)

## Purpose

The most state-rich machine in the system. Models the OTA update lifecycle:
download → validate → apply → reboot → self-check → commit-or-rollback.
Corresponds 1:1 with the `UpdatingFirmware` composite state on the gateway
top-level lifecycle (Machine 1).

This machine spans up to two MCU reboots in the worst case. Reboots are
**transition actions**, not states. The machine resumes at a specific state
determined by a flag persisted to non-volatile flags region just before the
reboot, detected by the gateway-lifecycle Init on the next boot.

## Reboot semantics

Two reboots are involved in the worst case:

1. **End of Applying** — sets `pending_self_check`, then triggers reboot.
   Gateway-lifecycle Init detects the flag on the next boot and resumes
   this machine in **SelfChecking**.
2. **End of RollingBack** (if reached) — sets `pending_rollback`, reverts
   the boot pointer, then triggers reboot. Gateway-lifecycle Init detects
   the flag on the next boot and resumes this machine in **Failed**.

On the diagram, this resumption is rendered with an entry-point pseudo-state
labelled "after reboot, from gateway_lifecycle.Init" and a choice diamond
that branches on which flag is set. The yellow note next to the diamond
makes explicit that this entry path fires only when a flag is set;
otherwise the machine remains in Idle across the reboot.

## State list

### Idle — simple, initial

No update in progress. Waits for the gateway top-level to enter
UpdatingFirmware (which only happens after the cloud command has passed
authentication and overlap checks).

- **Entry action:** clear update flags from non-volatile flags region.
- **Do activity:** none — passive.
- **Exit action:** none.

### Downloading — simple

Receive firmware image and write it to the inactive flash bank.

- **Entry action:** lock inactive bank; reset retry counter; persist
  offset = 0.
- **Do activity:** receive next chunk; write to inactive bank; persist
  offset (REQ-DM-051 resumability); on chunk failure, increment retry
  counter.
- **Exit action:** finalise inactive bank contents.

**Traceability:** DM-050, -051, -052, -053.

### Validating — simple

Verify cryptographic signature and image integrity of the downloaded
image *before* changing the boot pointer.

- **Entry action:** none.
- **Do activity:** verify signature (REQ-DM-080); verify image checksum /
  format (REQ-DM-060).
- **Exit action:** none.

**Traceability:** DM-060, -080.

### Applying — simple

Atomically commit the new image as the boot partition; current firmware
retained (REQ-DM-073).

- **Entry action:** none.
- **Do activity:** update boot pointer (REQ-DM-070); await `apply_done`
  from boot-pointer write.
- **Exit action:** none — T7's action sets the flag, persists, and emits
  `reset_requested`; the exit action is reachable and fires before T7's
  action (UML order: exit action → transition action).

**Traceability:** DM-070, -073, -074.

### SelfChecking — simple, **resumed after reboot in new firmware**

Verify the freshly-booted firmware is functional.

- **Entry action:** detect `pending_self_check` flag; run self-check
  sequence (sensors readable, communication links up — same checks as
  gateway-lifecycle SelfChecking step).
- **Do activity:** await self-check outcome.
- **Exit action:** clear `pending_self_check` flag.

**Traceability:** DM-071.

### RollingBack — simple

Revert to the previous firmware after self-check failed.

- **Entry action:** none.
- **Do activity:** await `rollback_ready` from boot-pointer revert.
- **Exit action:** none — T11's action sets the flag, reverts the boot
  pointer, persists, and emits `reset_requested`; exit action fires
  before T11's action and is reachable.

**Time budget:** ≤ 10 s end-to-end including the reboot (REQ-NF-204).

**Traceability:** DM-072, NF-204.

### Committed — final

New firmware accepted. Update lifecycle complete.

- **Entry action:** clear update flags; invalidate the now-old bank to free it for the next update.
- **Do activity:** none — terminal.
- **Exit action:** none.

*Cloud success report (REQ-DM-055) is sent by Machine 1 T18 action, not here, to avoid double-send.*

### Failed — final

Update did not complete successfully (download, validation, or self-check
+ rollback). Gateway is running on the previous firmware.

- **Entry action:** `log_fault()`; clear update flags; ensure inactive bank is invalidated.
- **Do activity:** none — terminal.
- **Exit action:** none.

*Cloud failure report (REQ-DM-055) is sent by Machine 1 T19 or T20 action, not here, to avoid double-send.*

## Transition table

### State transitions

| #   | From          | To             | Event                       | Guard                                | Action                                                                  |
|-----|---------------|----------------|-----------------------------|--------------------------------------|-------------------------------------------------------------------------|
| T1  | `[*]`         | Idle           | —                           | —                                    | —                                                                       |
| T2  | Idle          | Downloading    | `update_started`            | —                                    | `start_downloader(); reset_offset(); persist()`                         |
| T3  | Downloading   | Validating     | `download_complete`         | —                                    | `finalise_image()`                                                      |
| T4  | Downloading   | Failed         | `download_chunk_failure`    | `[retries == 3]`                     | `delete_partial_image()` (DM-052, -053)                                 |
| T5  | Validating    | Applying       | `validation_done`           | `[signature_ok && image_ok]`         | —                                                                       |
| T6  | Validating    | Failed         | `validation_done`           | `[!signature_ok || !image_ok]`       | `discard_image()` (DM-061, -062)                                        |
| T7  | Applying      | (MCU reset)    | `apply_done`                | —                                    | `set_pending_self_check(); persist(); emit reset_requested`             |
| T8  | (entry-point) | SelfChecking   | `boot_with_pending_flag`    | `[pending_self_check]`               | resumed by gateway Init detecting flag                                  |
| T9  | SelfChecking  | Committed      | `self_check_done`           | `[self_check_ok]`                    | `clear_flag()`                                                          |
| T10 | SelfChecking  | RollingBack    | `self_check_done`           | `[!self_check_ok]`                   | —                                                                       |
| T11 | RollingBack   | (MCU reset)    | `rollback_ready`            | —                                    | `set_pending_rollback(); revert_boot_ptr(); persist(); emit reset_requested` |
| T12 | (entry-point) | Failed         | `boot_with_rollback_flag`   | `[pending_rollback]`                 | `clear_flag()`                                                          |
| T13 | Committed     | Idle           | `lifecycle_exits_updating`  | —                                    | —                                                                       |
| T14 | Failed        | Idle           | `lifecycle_exits_updating`  | —                                    | —                                                                       |

### Internal transitions

| #   | In state    | Event                       | Guard               | Action                                              |
|-----|-------------|-----------------------------|---------------------|-----------------------------------------------------|
| I1  | Downloading | `download_chunk_success`    | —                   | `write_to_inactive_bank(chunk); persist_offset()`   |
| I2  | Downloading | `download_chunk_failure`    | `[retries < 3]`     | `retries++`; retry chunk (DM-051)                   |

---

# Machine 4 — Modbus Master

**Owned by:** `ModbusPoller` (gateway, Application layer; protocol-level
frame handling delegated to `ModbusMaster` Middleware).

![Modbus Master](../diagrams/state-machine-modbus-master.png)

## Purpose

Models the polling cycle for a single Modbus transaction against the field
device. Active machine — drives transitions via internal timers.

The Online/Offline link state (REQ-NF-103, NF-104, NF-215) is hysteresis on
top of the polling cycle, not a separate set of polling states. It is
tracked as a model variable (`link_state ∈ {Online, Offline}`) plus
consecutive-counter variables. Transitions to/from Offline emit
`node_offline` / `node_online` events consumed by HealthMonitor and
CloudPublisher; they are not state-machine transitions of this machine.
The yellow note on the diagram surfaces this hysteresis logic.

## State list

### Idle — simple, initial

Between polls. Polling timer drives the next transaction.

- **Entry action:** none.
- **Do activity:** await `poll_timer_tick` or `command_to_send`.
- **Exit action:** select next request based on caller.

### Transmitting — simple

Drive the request frame onto the RS-485 bus.

- **Entry action:** enable TX driver; start UART transmit (frame already
  built by the incoming transition action — T2 or T3).

*`build_request_frame()` must NOT be called here: T3's command frame
would be overwritten with a poll frame before transmission.*
- **Do activity:** wait for `tx_complete`.
- **Exit action:** disable TX driver (release the bus).

**Traceability:** MB-010, MB-020, MB-040, MB-080, MB-100.

### AwaitingResponse — simple

Wait for the slave's response within the 200 ms timeout.

- **Entry action:** `start_response_timer_ms(200)`; enable RX.
- **Do activity:** wait for `rx_complete` or `response_timer_expired`.
- **Exit action:** stop response timer.

**Traceability:** MB-050, NF-105.

### ProcessingResponse — simple

Validate the received frame and dispatch the result.

- **Entry action:** none.
- **Do activity:** check CRC; verify slave address and function code match
  the request; parse payload; dispatch to caller.
- **Exit action:** update counters; emit `node_offline` / `node_online`
  event if hysteresis threshold crossed.

**Traceability:** MB-040 (function code dispatch), MB-090 (return result),
MB-0E1 (reject malformed), NF-103, NF-104, NF-215.

## Transition table

### State transitions

| #   | From               | To                  | Event                       | Guard                                                | Action                                                          |
|-----|--------------------|---------------------|-----------------------------|------------------------------------------------------|-----------------------------------------------------------------|
| T1  | `[*]`              | Idle                | —                           | —                                                    | —                                                               |
| T2  | Idle               | Transmitting        | `poll_timer_tick`           | `[poll_pending]`                                     | `build_request_frame()`                                         |
| T3  | Idle               | Transmitting        | `command_to_send`           | —                                                    | `build_request_frame_for_incoming_command()`                    |
| T4  | Transmitting       | AwaitingResponse    | `tx_complete`               | —                                                    | —                                                               |
| T5  | AwaitingResponse   | ProcessingResponse  | `rx_complete`               | —                                                    | —                                                               |
| T6  | AwaitingResponse   | Transmitting        | `response_timer_expired`    | `[per_request_retries < 3]`                          | `per_request_retries++` (MB-060)                                |
| T7  | AwaitingResponse   | Idle                | `response_timer_expired`    | `[per_request_retries == 3]`                         | `record_poll_failure()`                                         |
| T8  | ProcessingResponse | Idle                | `processing_done`           | `[crc_ok && format_ok]`                              | `record_poll_success()`                                         |
| T9  | ProcessingResponse | Idle                | `processing_done`           | `[!crc_ok || !format_ok]`                            | `record_poll_failure()`                                         |

### Action sidebar — link state hysteresis

The actions `record_poll_failure()` and `record_poll_success()` encapsulate
the hysteresis logic from REQ-NF-103, REQ-NF-104, REQ-NF-215:

```
record_poll_failure():
  per_request_retries = 0
  consecutive_successes = 0
  consecutive_failures += 1
  if (link_state == Online && consecutive_failures >= 3):
    link_state = Offline
    emit node_offline   // → HealthMonitor / CloudPublisher (NF-215)

record_poll_success():
  per_request_retries = 0
  consecutive_failures = 0
  consecutive_successes += 1
  if (link_state == Offline && consecutive_successes >= 3):
    link_state = Online
    emit node_online    // → HealthMonitor (NF-104)
```

These sit in the transition action column; they are not additional states.

### Why the slave is structured differently

The Modbus Slave (Machine 6) deliberately has no timeout, no retry, and no
link-state hysteresis. Those behaviours are master-side responsibilities.
The slave simply receives or doesn't, and waits in Idle either way. See
the "Reactive machine" note on Machine 6's diagram.

---

# Machine 5 — Field Device Lifecycle

**Owned by:** `LifecycleController` (field device, Application layer).

![Field Device Lifecycle](../diagrams/state-machine-field-device-lifecycle.png)

## Purpose

Simpler than the gateway: no cloud, no firmware update, no remote restart.
Models the field device's Init phase (with five sequential sub-states),
its single Operational mode, and the EditingConfig path for CLI
provisioning.

Field-device-specific shape comes from CON-003 (no on-board sensors → the
sensor simulation module replaces them in BringingUpSensors), the absence
of UC-17 (no remote restart → no Restarting state), the absence of UC-18
(no firmware update at this stage), and the presence of REQ-LD-000 (LCD
essential → adds BringingUpLCD as a distinct Init sub-step).

## State list

### Init — composite

- **Entry action:** record reset cause; start Init time-budget timer.
- **Do activity:** progress through sub-states below.
- **Exit action:** stop Init time-budget timer.

| # | Sub-state           | Entry action | Do activity                                                      | Exit action | Failure handling                                            |
|---|---------------------|--------------|------------------------------------------------------------------|-------------|-------------------------------------------------------------|
| 1 | CheckingIntegrity   | none         | Verify config / firmware partitions are not partially written    | none        | Unrecoverable corruption → Faulted (REQ-NF-214)             |
| 2 | LoadingConfig       | none         | `ConfigStore.read()`; fall back to defaults on failure           | none        | Partial fallback OK, never fatal (SA-010, -020, -050)       |
| 3 | BringingUpSensors   | none         | Initialise sensor simulation module                              | none        | ≥1 sensor OK → continue; all fail → Faulted                 |
| 4 | BringingUpLCD       | none         | Initialise LCD driver and display layer                          | none        | Failure → Faulted (LCD essential per REQ-LD-000)            |
| 5 | StartingMiddleware  | none         | Start Modbus Slave sub-machine; start AlarmService; start CLI    | none        | Sub-machines own their own failure recovery                 |

Unlike the gateway, the field-device Init has **no SelfChecking sub-step**
— REQ-DM-040 is gateway-only (UC-17). Sensor and LCD verification happens
implicitly in BringingUpSensors / BringingUpLCD.

**Time budgets:** Cold boot ≤ [TBD]s (REQ-NF-213); Watchdog reset ≤ 5s
(REQ-NF-202).

**Traceability:** SA-000, -010, -020, -030, -040, -050, -060;
LD-000; NF-202, -213, -214.

### Operational — simple

Acquire and process simulated sensor data, update LCD and Modbus
registers, evaluate alarms, respond to gateway polls and CLI commands.

- **Entry action:** start periodic timers (sensor polling, LCD refresh).
- **Do activity:** see internal-transition table below.
- **Exit action:** stop periodic timers.

**Traceability:** SA-070, -080, -090, -100, -110..160, -170;
AM-000, -010, -011, -020; LD-010..090;
LI-000..020, -130..160; MB-000; NF-208, -212.

**Note (REQ-NF-205):** Sensor reinitialisation on failure is handled
entirely within `SensorService`'s internal recovery loop; the lifecycle
machine remains in Operational throughout. If `SensorService` cannot
reinitialise a sensor, it emits `unrecoverable_error` → T13 (→ Faulted).
NF-205 traces to `SensorService`, not to this state machine.

### EditingConfig — simple

Field Technician is provisioning the field device via CLI. Sensor
acquisition and alarm evaluation continue (handled by their own tasks);
but Modbus address and serial-port settings take effect only on
confirmation, with rollback on failure.

- **Entry action:** start confirmation-timeout timer.
- **Do activity:** receive parameter inputs; validate each (LI-090);
  display feedback on LCD; await confirmation.
- **Exit action:** close provisioning menu; clear timeout timer.

*Snapshot, apply, and restore are owned exclusively by T10, T11, T12
to avoid double invocation.*

**Traceability:** LI-030, -070, -080, -090, -100, -110, -120, -0E2, -0E3;
LD-100..150; DM-090.

### Faulted — simple, terminal-ish

Same shape as gateway Faulted. Unrecoverable condition detected; system
stays in a safe, observable state until external/watchdog reset.

- **Entry action:** `log_fault()`; stop all periodic timers; capture fault
  context to flash log; set fault-indicator LED pattern; show fault
  message on LCD.
- **Do activity:** maintain LED pattern and LCD fault display; refuse to
  start any normal operation.
- **Exit action:** none.

## Transition table

### State transitions

| #   | From                       | To                          | Event                                          | Guard                            | Action                                                                |
|-----|----------------------------|-----------------------------|------------------------------------------------|----------------------------------|-----------------------------------------------------------------------|
| T1  | `[*]`                      | Init.CheckingIntegrity      | —                                              | —                                | —                                                                     |
| T2  | Init.CheckingIntegrity     | Init.LoadingConfig          | `integrity_check_done`                         | `[partitions_ok]`                | —                                                                     |
| T3  | Init.CheckingIntegrity     | Faulted                     | `integrity_check_done`                         | `[!partitions_ok]`               | —                                                                     |
| T4  | Init.LoadingConfig         | Init.BringingUpSensors      | `config_load_done`                             | —                                | —                                                                     |
| T5  | Init.BringingUpSensors     | Init.BringingUpLCD          | `sensor_init_done`                             | `[≥1 sensor available]`          | —                                                                     |
| T6  | Init.BringingUpSensors     | Faulted                     | `sensor_init_done`                             | `[no sensor available]`          | —                                                                     |
| T7  | Init.BringingUpLCD         | Init.StartingMiddleware     | `lcd_init_done`                                | `[lcd_ok]`                       | —                                                                     |
| T8  | Init.BringingUpLCD         | Faulted                     | `lcd_init_done`                                | `[!lcd_ok]`                      | —                                                                     |
| T9  | Init.StartingMiddleware    | Operational                 | `middleware_started`                           | —                                | —                                                                     |
| T10 | Operational                | EditingConfig               | `cli_provision_entered`                        | —                                | `snapshot_config(); show_provision_menu()`                            |
| T11 | EditingConfig              | Operational                 | `confirmation_received`                        | `[validation_ok]`                | `apply_and_persist(); emit modbus_address_changed (if applicable)`    |
| T12 | EditingConfig              | Operational                 | `cancel_received, confirmation_timeout`        | —                                | `restore_snapshot()`                                                  |
| T13 | Operational                | Faulted                     | `watchdog_imminent, unrecoverable_error`       | —                                | —                                                                     |
| T14 | EditingConfig              | Faulted                     | `watchdog_imminent, unrecoverable_error`       | —                                | `restore_snapshot()`                                                  |
| T15 | Faulted                    | (MCU reset)                 | `watchdog_expiry, external_reset`              | —                                | —                                                                     |

### Internal transitions in Operational

| #   | Event                              | Guard               | Action                                                |
|-----|------------------------------------|---------------------|-------------------------------------------------------|
| I1  | `polling_timer_tick`               | —                   | `SensorService.read_cycle()`                          |
| I2  | `lcd_refresh_timer` (5 Hz)         | —                   | `GraphicsLibrary.refresh()` (NF-108)                  |
| I3  | `new_processed_reading`            | —                   | `update_modbus_registers(); update_lcd_buffer()` (SA-150, MB-000); `AlarmService.evaluate()` (AM-000..-020) — register/LCD update always precedes alarm evaluation so alarm logic sees the latest values |
| I5  | `modbus_time_push_received`        | —                   | `RtcDriver.set_time(); TimeProvider.mark_synced()`    |
| I6  | `remote_read_received_via_modbus`  | —                   | `SensorService.read_now()` (SA-170)                   |
| I7  | `cli_diagnostic_received`          | `[cmd_recognised]`  | `dispatch_and_respond()` (LI-000..020, -130..160)     |
| I8  | `cli_diagnostic_received`          | `[!cmd_recognised]` | `display_error()` (LI-0E1)                            |

### Internal transitions in EditingConfig

| #   | Event                | Guard               | Action                                              |
|-----|----------------------|---------------------|-----------------------------------------------------|
| I9  | `cli_param_input`    | `[validation_ok]`   | `accept_input(); show_on_lcd()`                     |
| I10 | `cli_param_input`    | `[!validation_ok]`  | `reject(); show_error()` (LI-0E2)                   |

---

# Machine 6 — Modbus Slave

**Owned by:** `ModbusSlave` (field device, Middleware layer).

![Modbus Slave](../diagrams/state-machine-modbus-slave.png)

## Purpose

Models the response cycle for a single Modbus transaction received from
the gateway. **Reactive machine — no timers, no retries, no polling.**
Frame reception drives every transition. Pairs with Modbus Master
(Machine 4) across the RS-485 bus.

Master-side concerns explicitly absent from the slave:
- REQ-MB-050 / REQ-NF-105 (200 ms timeout) — slave does not time out.
- REQ-MB-060 (3-retry) — slave does not retry; each master retry arrives
  as a fresh request.
- REQ-NF-103 / NF-104 (link-state hysteresis) — slave has no concept of
  link state.

## State list

### Idle — simple, initial

Wait for an incoming Modbus frame on the RS-485 bus.

- **Entry action:** enable UART RX; arm inter-frame silence detector
  (3.5 character times for Modbus RTU framing — driver-level detail).
- **Do activity:** wait for `frame_complete` event.
- **Exit action:** disable RX; pass received frame buffer to processing.

**Internal action:** on `new_processed_reading` event from
SensorService, write the value via `IModbusRegisterMap` (REQ-MB-000).
Does not change state. `IModbusRegisterMap` is a DIP interface owned by
the Application layer; `ModbusSlave` depends on the abstraction only —
the concrete binding is specified in LLD.

**Traceability:** MB-000, MB-100 (address filter applies on exit).

### ProcessingRequest — simple

Validate the received frame and produce a response (or decide not to
respond).

- **Entry action:** none.
- **Do activity:** address filter (REQ-MB-100) → CRC check → function
  code dispatch (MB-040) → execute (read regs / write regs / dispatch
  command via MB-080, -090) → build response frame; **or** build
  exception response if function code unsupported, address invalid, or
  command opcode unrecognised (MB-0E1); **or** silently mark as "no
  response" if address is not ours or CRC failed.
- **Exit action:** signal whether a response was produced.

**Traceability:** MB-010, MB-020, MB-040, MB-080, MB-090, MB-100, MB-0E1.

### Responding — simple

Transmit the response frame back to the gateway over RS-485.

- **Entry action:** enable TX driver (RS-485 direction control); start
  UART transmit of response frame.
- **Do activity:** wait for `tx_complete`.
- **Exit action:** disable TX driver (release the bus to listening).

**Traceability:** MB-010, MB-020, MB-080, MB-090.

## Transition table

### State transitions

| #   | From               | To                  | Event                  | Guard                              | Action                              |
|-----|--------------------|---------------------|------------------------|------------------------------------|-------------------------------------|
| T1  | `[*]`              | Idle                | —                      | —                                  | —                                   |
| T2  | Idle               | ProcessingRequest   | `frame_complete`       | —                                  | `hand_frame_buffer_to_processor()`  |
| T3  | ProcessingRequest  | Responding          | `processing_done`      | `[response_required]`              | `hand_response_frame_to_tx()`       |
| T4  | ProcessingRequest  | Idle                | `processing_done`      | `[!response_required]`             | `log_silent_drop()`                 |
| T5  | Responding         | Idle                | `tx_complete`          | —                                  | —                                   |

### Internal transitions

| #   | In state | Event                              | Guard | Action                                          |
|-----|----------|------------------------------------|-------|-------------------------------------------------|
| I1  | Idle     | `new_processed_reading`            | —     | `IModbusRegisterMap.write(addr, value)` (MB-000) — DIP interface; concrete binding specified in LLD |
| I2  | Idle     | `modbus_address_changed`           | —     | update address filter                           |

### Why `response_required` is a guard, not a separate state

Three outcomes after processing a frame:
1. Valid frame addressed to us → produce normal response → Responding.
2. Valid frame requesting an unsupported function or operation → produce
   exception response (per MB-040 / MB-0E1) → Responding (an exception
   response is still a response).
3. Frame not for us, or CRC failed → silent drop → Idle directly.

Cases 1 and 2 both transit to Responding (the response frame just has a
different shape). Only case 3 short-circuits to Idle. A single guard
`[response_required]` separates them without a fourth state.

---

# Cross-machine relationships

How the six machines couple at runtime.

## Within the gateway

| Coupling                                     | Mechanism                                                                                        |
|----------------------------------------------|--------------------------------------------------------------------------------------------------|
| Gateway lifecycle ↔ Cloud Connectivity       | Init triggers Cloud Connectivity start (REQ-CC-050). Otherwise independent — gateway does not block on cloud (REQ-NF-200). Cloud Connectivity emits `internet_restored` (REQ-TS-0E1) on Connected entry; consumed by `NtpClient` to trigger an immediate resync. |
| Gateway lifecycle ↔ Firmware Update          | Machine 1 T14 emits `update_started` (consumed by Machine 3 T2: Idle → Downloading). Machine 3 reaching Committed or Failed emits `update_done` (consumed by Machine 1 T18/T19/T20). Machine 1 exiting UpdatingFirmware via T18/T19/T20 emits `lifecycle_exits_updating` (consumed by Machine 3 T13/T14: terminal → Idle). Machine 3 T7 and T11 emit `reset_requested` instead of calling `NVIC_SystemReset()` directly; Machine 1 I18 (UpdatingFirmware internal transition) owns the single canonical reset call. The reboot-and-resume chain (Applying → SelfChecking; RollingBack → Failed) crosses gateway-lifecycle Init via the persisted-flag entry-point pseudo-state. |
| Gateway lifecycle ↔ Modbus Master            | Init starts Modbus Master. Modbus failures surface via `node_offline` event (consumed by HealthMonitor / CloudPublisher); they do NOT change gateway top-level state. |
| Gateway EditingConfig → Cloud Connectivity   | On apply, emits `internet_params_changed`; Cloud Connectivity force-disconnects and reconnects with new credentials (Connected → Disconnected via T7 of Machine 2). |
| Cloud Connectivity ↔ Firmware Update         | FU uses CloudPublisher (which uses Cloud Connectivity) to receive image chunks and report results. REQ-DM-051 resumability is satisfied by persisting the download offset on every successful chunk (Machine 3 I1: `persist_offset()`); a connectivity-loss restart re-enters Downloading from the persisted offset without a separate Paused state. |

## Within the field device

| Coupling                                                | Mechanism                                                                  |
|---------------------------------------------------------|----------------------------------------------------------------------------|
| Field-device lifecycle ↔ Modbus Slave                   | Init.StartingMiddleware brings up the slave; thereafter it runs autonomously. |
| Modbus Slave → Field-device lifecycle (Operational)     | A successful write to the time-push holding register (REQ-MB-020) emits `modbus_time_push_received`, consumed by Operational internal transition I5. |
| Sensor pipeline (in Operational) → Modbus Slave         | New processed readings emit `new_processed_reading` (canonical name), consumed by Machine 5 Operational I3 (register/LCD update + alarm evaluation). `ModbusSlave` I1 also reacts to this event via `IModbusRegisterMap` (DIP interface; P1-compliant). |
| Field-device EditingConfig → Modbus Slave               | On Modbus-address change, emits `modbus_address_changed`; Slave updates its address filter without restarting (I2). |

## Across the physical boundary

| Coupling                                              | Mechanism                                                                  |
|-------------------------------------------------------|----------------------------------------------------------------------------|
| Modbus Master (gateway) ↔ Modbus Slave (field device) | RS-485 half-duplex over UART. Master initiates; slave responds. The two machines never see each other's internal state — only frames on the bus and the cause/effect they imply. |

Sequence diagrams (HLD Artefact #5) will illustrate these interactions
concretely.

---

# Requirement derivation

The table below maps each SRS functional area to the states, transitions,
and guards that satisfy it, providing the reviewable forward-trace required
by the gate checklist.

| SRS area | States / transitions / guards | Justification |
|---|---|---|
| REQ-SA-000..-060 (boot / init) | Machine 1 Init sub-states 1–5; Machine 5 Init sub-states 1–5 | Sequential sub-state progression gates Operational entry on all-subsystems-up |
| REQ-SA-070..-170 (sensor ops) | Machine 1 Operational I1–I8; Machine 5 Operational I1–I8 | Periodic read cycle (I1), alarm evaluation, LCD/Modbus update, remote read |
| REQ-AM-000..-030 (alarms) | Machine 1 I2–I3; Machine 5 I3 (merged) | AlarmService evaluates after every new reading; CloudPublisher routes alarm events |
| REQ-CC-050..-060 (MQTT/TLS) | Machine 2 Connecting entry; T2/T3 | TLS handshake, X.509 auth on every connect attempt |
| REQ-BF-000..-020 (store-and-forward) | Machine 2 Disconnected do-activity; I2/I3 | Enqueue to CircularFlashLog; drop-oldest on overflow |
| REQ-DM-010..-030 (remote restart) | Machine 1 T13, Restarting, T17 | Confirmation guard on T13; restart_flag enables DM-030 on next boot |
| REQ-DM-040 (post-restart self-check) | Machine 1 Init sub-state 5 (SelfChecking) | Runs on every boot; result gates Operational entry |
| REQ-DM-050..-080 (OTA lifecycle) | Machine 3 Downloading→Validating→Applying→SelfChecking→Committed/Failed | Each stage maps to one state; cryptographic check in Validating; boot-pointer flip in Applying |
| REQ-DM-071..-074 (rollback) | Machine 3 RollingBack; Machine 1 I18 | Self-check failure triggers rollback; gateway lifecycle owns reset |
| REQ-MB-000..-100 (Modbus protocol) | Machine 4 (master); Machine 6 (slave) | Half-duplex polling + command dispatch (master); address filter + frame dispatch (slave) |
| REQ-NF-103..-104 (link hysteresis) | Machine 4 `record_poll_failure/success()` sidebar | Offline/online threshold tracked in transition action variables |
| REQ-NF-200 (cloud loss non-blocking) | Machine 2 independence from Machine 1 top level | Cloud Connectivity sub-machine owns Disconnected; Machine 1 stays Operational |
| REQ-NF-202..-204 (timing budgets) | Machine 1/5 Init time-budget timer; Machine 3 RollingBack ≤ 10 s | Timer started on Init entry; RollingBack traces NF-204 |
| REQ-NF-205 (sensor reinit) | `SensorService` internal loop (not lifecycle) | Lifecycle stays Operational; permanent failure surfaces as `unrecoverable_error` |
| REQ-LI-* (CLI provisioning) | Machine 1/5 EditingConfig; T10/T11/T12 | Snapshot-on-enter, apply-or-restore-on-exit, timeout guard |
| REQ-LD-000..-240 (LCD) | Machine 5 Init.BringingUpLCD; Operational I2; EditingConfig | LCD init gates entry to Operational; 5 Hz refresh in Operational |
| REQ-TS-030..-040 (time sync) | Machine 1 I6/I7; Machine 5 I5 | NTP resync periodic; time-push via Modbus |

---

# Decisions and open questions

## Decisions taken

1. **No "Degraded" top-level state on the gateway.** REQ-NF-200 makes it
   unnecessary; cloud loss lives inside the Cloud Connectivity sub-machine.
   Promoting Degraded to a top-level state would duplicate logic the
   sub-machine already owns.
2. **EditingConfig as a top-level state on both boards.** Symmetric flow,
   different parameters (gateway: WiFi credentials, cloud endpoint, certs;
   field device: Modbus address, serial port). Not a sub-state of
   Operational because it has a distinct exit timeout and snapshot-rollback
   semantics.
3. **Modbus link-state hysteresis as transition actions, not separate
   states.** Keeps the Modbus Master diagram readable. An alternative HSM
   approach (Online and Offline as composite states each containing the
   same polling sub-states) was considered and rejected as visually
   redundant.
4. **Reboots in Firmware Update are transition actions, not states.** The
   machine resumes via persisted flags detected by gateway-lifecycle Init.
   On the diagram, this is rendered as an entry-point pseudo-state plus a
   choice diamond branching on which flag is set.
5. **Alarm message content (REQ-AM-040) is not state persistence.** Alarm
   state is RAM-only; re-evaluated from current sensor values on boot.
6. **"All sensors failed" → Faulted on both boards.** A node with no sensor
   data has no purpose.
7. **NTP is not a precondition for reaching Operational on the gateway.**
   TimeProvider provides uptime-based timestamps until sync arrives
   (REQ-TS-040).
8. **Field-device Init has no SelfChecking sub-step.** REQ-DM-040 traces
   only to UC-17 (gateway-only restart). Sensor and LCD verification
   happens in BringingUpSensors / BringingUpLCD.
9. **`LifecycleController` introduced as the explicit owner of each
   board's top-level lifecycle.** Recorded in `components.md`. The state
   machines previously had no home component; this addition closes that
   gap and provides a natural place for the splash-screen orchestration
   on the field device (REQ-LD-200..-240).

## Open questions deferred to LLD

1. **Per-channel alarm state machine (Clear ↔ Active with hysteresis).**
   Each sensor channel has its own alarm state. The hysteresis logic
   (REQ-AM-011) suggests a small state machine per channel, but it is
   per-instance not per-system; the states are trivial (Clear, Active)
   with a single guarded transition pair driven by threshold + hysteresis;
   including it in HLD would clutter without adding clarity. LLD is the
   right place — alongside the per-channel data structures and threshold
   storage.
2. **Backoff strategy beyond REQ-NF-209's 1 Hz constant.** Keep flat 1 Hz;
   revisit if integration testing reveals problems.
3. **Buffer size [TBD]** — driven by CON-009 flash endurance and ASM-004
   SRAM budget.
4. **Cold-boot vs warm-restart Init time budgets.** REQ-NF-213 (cold) is
   distinct from REQ-NF-202 / -203 (warm). Different boot paths, different
   budgets — documented per-state in this spec.
5. **What does "self-check" cover precisely?** Currently SRS says sensors
   plus communication links (REQ-DM-040 / DM-071). HLD decision: include
   MQTT reconnect for the firmware-update self-check, otherwise a broken
   cloud config could ship in firmware. Final coverage list at LLD time.
