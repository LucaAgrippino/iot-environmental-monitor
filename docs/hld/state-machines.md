# State Machine Diagrams — HLD Artefact #4

**Status:** Working document. Captures the step-by-step derivation of every
state machine in the system from the SRS and use cases, suitable for direct
translation into Visual Paradigm.

**Scope:** Six state machines in total.

| #   | Machine                                  | Owner          | Type                          |
|-----|------------------------------------------|----------------|-------------------------------|
| 1   | Gateway Lifecycle                        | Gateway        | Top-level (board lifecycle)   |
| 2   | Cloud Connectivity                       | Gateway        | Sub-machine                   |
| 3   | Firmware Update                          | Gateway        | Sub-machine (spans 2 reboots) |
| 4   | Modbus Master                            | Gateway        | Sub-machine (active)          |
| 5   | Field Device Lifecycle                   | Field Device   | Top-level (board lifecycle)   |
| 6   | Modbus Slave                             | Field Device   | Sub-machine (reactive)        |

A seventh candidate — a **per-alarm channel state machine** (Clear ↔ Active
with hysteresis) — is **deliberately deferred to LLD**. Reasoning is given in
the Open Questions section at the end of this document.

---

## Part A — Methodology

A state machine diagram is a downstream artefact: it is *derived from* the SRS
and use cases, not invented. Skipping any of the steps below produces diagrams
that don't trace cleanly back to requirements, and that's a serious defect
because a state machine that can't be traced cannot be tested.

The method is three steps before opening Visual Paradigm.

### Step 1 — Requirement → behaviour mapping

Walk the relevant SRS sections. For each requirement, attach exactly one of
the following labels:

| Label                          | Meaning                                                                                                                  |
|--------------------------------|--------------------------------------------------------------------------------------------------------------------------|
| **IMPLIES STATE**              | The requirement creates a top-level state. Behaviour during this state differs measurably from neighbours.               |
| **IMPLIES TRANSITION**         | The requirement creates a transition (or pair) between two states. Always names a trigger.                               |
| **ACTION in `<state>`**        | The requirement populates an existing state with behaviour — entry, do, exit, or internal transition. Doesn't add states.|
| **IMPLIES GUARD**              | The requirement adds a boolean predicate gating an existing transition.                                                  |
| **NO LIFECYCLE IMPLICATION**   | Payload format, internal algorithms, driver settings, or message content. Doesn't change with state.                     |

Two pitfalls to internalise before doing this:

- **Don't conflate "important" with "stateful".** A QoS setting (NF-206)
  matters but doesn't drive lifecycle. A polling cycle (SA-070) matters but
  is an action inside Operational, not a state in itself.
- **Don't model sub-machines twice.** If a behaviour belongs to a sub-machine
  (e.g. cloud reconnect → Cloud Connectivity), the parent machine references
  it through one event, not by re-modelling it.

### Step 2 — State list with entry / do / exit

For each state identified in Step 1, write down:

- **Type:** simple, composite, or final.
- **Purpose:** one sentence — why this state exists.
- **Entry action:** what runs once on entering the state.
- **Do activity:** what runs continuously while in the state.
- **Exit action:** what runs once on leaving the state.
- **Traceability:** which requirements/use cases earn it.

If you can't write a single-sentence purpose for a state, the state isn't
clear enough — go back and reconsider.

### Step 3 — Transition table

Notation per row: `event [guard] / action`. Two tables:

- **State transitions** — change which state we're in.
- **Internal transitions** — fire while in a state, change nothing.

After Step 3, the diagram is mechanical to draw. Every Visual Paradigm box
and arrow has a row in one of these two tables.

---

## Part B — Methodology principles

These are rules learned across the six machines below. Same rules apply to
all of them.

### 1. Convention first, then refinement

Every long-running embedded system has the same skeleton:

```
[*] → Init → Operational → ... → Faulted
```

Start there. Let the SRS *earn* additional states. Don't invent states from
scratch — that produces machines that are interesting but unmoored from
requirements. Conversely, don't bolt every requirement onto a unique state —
most requirements describe **actions inside states**, not new states.

### 2. A state earns its place if (any of)

- Behaviour differs measurably from neighbouring states.
- It has multiple incoming or outgoing transitions that wouldn't make sense
  from any other state.
- An SRS requirement explicitly demands it as a discrete mode.

If none of these is true, the candidate is an action or guard, not a state.

### 3. Sub-machines absorb internal complexity

When a state has its own rich lifecycle (firmware update, cloud connectivity,
Modbus polling), promote it to a separate machine and represent it on the
parent as a single composite box. This keeps any one diagram readable.

The cost — keeping sub-machines coordinated — is paid through clearly named
events that cross the boundary (`ota_cmd_received`, `update_done`).

### 4. No duplication across machines

If a behaviour is owned by a sub-machine, the parent references it — does
not re-model it. The clearest example in this project is **NF-200** (continue
local acquisition when cloud is lost): cloud loss is owned by the Cloud
Connectivity sub-machine, so the Gateway top-level lifecycle does **not**
have a "Degraded" state. Operational stays Operational; only the sub-machine
flips to Disconnected.

### 5. Implementation details belong in LLD

FreeRTOS task scheduling, exact timer values, register bit layouts, and CRC
algorithms all belong in LLD, not inside HLD state machines. The HLD says
"start the response timer (200 ms)"; the LLD says which TIM peripheral and
which register sets it.

### 6. Reactive vs active machines

A master initiates and times out. A slave responds. The two have completely
different shapes:

| Aspect              | Active (e.g. Modbus Master)              | Reactive (e.g. Modbus Slave)        |
|---------------------|------------------------------------------|-------------------------------------|
| Drives transitions  | Internal timers + caller events          | Incoming external events only       |
| Has retries?        | Yes                                      | No                                  |
| Has timeouts?       | Yes                                      | No                                  |
| Failure handling    | Counts and reports                       | Silent drop and wait                |

Mixing these up is a common mistake. The slave's machine must never inherit
the master's timer and retry structure.

---

## Part C — Inventory and cross-machine map

```
Gateway (B-L475E-IOT01A)                       Field Device (STM32F469)
------------------------                       -------------------------

  [Gateway Lifecycle]                            [Field Device Lifecycle]
       │                                              │
       ├─ starts ──► [Cloud Connectivity]             ├─ starts ──► [Modbus Slave]
       │                                              │
       ├─ delegates ► [Firmware Update]               
       │
       └─ starts ──► [Modbus Master] ◄──── RS-485 ────► [Modbus Slave]
                                  Modbus RTU
```

Six machines, four on the gateway, two on the field device. The only
machine-to-machine link that crosses the physical boundary is the Modbus RTU
bus connecting Master ↔ Slave.

### Difference summary — gateway vs field device

| Aspect                      | Gateway                                                       | Field Device                                                |
|-----------------------------|---------------------------------------------------------------|-------------------------------------------------------------|
| Top-level states            | 6 (Init, Operational, EditingConfig, Restarting, UpdatingFirmware, Faulted) | 4 (Init, Operational, EditingConfig, Faulted)               |
| Init sub-states             | 5 (incl. SelfChecking)                                        | 5 (incl. BringingUpLCD; no SelfChecking)                    |
| Sub-machines                | 3 (Cloud Connectivity, Firmware Update, Modbus Master)        | 1 (Modbus Slave)                                            |
| Reactive or active?         | Active                                                        | Mostly passive (only sensor poll timer is active)           |
| Cloud / network             | MQTT/TLS over WiFi                                            | None                                                        |
| Time source                 | NTP                                                           | Push from gateway via Modbus holding register               |
| Restart behaviour           | Remote restart with confirmation (UC-17)                      | Power cycle / watchdog only                                 |
| Firmware update             | OTA, dual-bank, rollback (UC-18)                              | Out of scope at this stage                                  |
| Sensors                     | On-board (temp, humidity, pressure, IMU)                      | Software simulation (CON-003)                               |
| Alarm output path           | Direct cloud publish (CC-020)                                 | Modbus alarm registers, gateway forwards                    |
| Display                     | None (CLI only)                                               | LCD                                                         |

This table is the single most useful thing to memorise for interview
discussion. It captures what makes the two boards different at a system
level.

---

# Part D — Machine 1: Gateway Top-Level Lifecycle

The richest top-level machine in the system. Models the gateway's overall
mode: booting, running normally, being provisioned via CLI, restarting,
applying a firmware update, or stopped on fault.

## Step 1 — Requirements → behaviours

### INIT

```
REQ-SA-000  Read polling config from flash at startup
            → ACTION in Init: ConfigStore.read() (sub-step LoadingConfig)

REQ-SA-010  Use default polling interval if config read fails
            → IMPLIES GUARD on Init exit: [config_loaded || defaults_applied]
              confirms config-load failure is NOT fatal

REQ-SA-020  Use default sensor min/max if config read fails
            → ACTION in Init: same fallback path as SA-010

REQ-SA-031  Initialise gateway sensors during startup
            → ACTION in Init: sensor bring-up (sub-step BringingUpSensors)

REQ-SA-040  Log error if a sensor fails to initialise
            → ACTION in Init: log per-sensor result

REQ-SA-050  Use default filter parameters if config read fails
            → ACTION in Init: same fallback path as SA-010

REQ-SA-060  Continue with available sensors if some fail
            → IMPLIES GUARD on Init exit: [≥ 1 sensor available]
              partial sensor failure is NOT fatal; all-fail → Faulted

REQ-TS-000  Sync time at boot (and periodically thereafter)
            → ACTION in Init: NTP sync attempt (boot half)
              periodic half belongs in Operational

REQ-TS-020  Update RTC with synchronised time
            → ACTION in Init: RTC write after NTP success
              (also runs in Operational on later resyncs)

REQ-DM-040  Self-check after restart, verifying sensors and links
            → ACTION in Init: SelfChecking sub-step gating exit to Operational

REQ-NF-202  Restart and resume within 5s after watchdog reset
            → IMPLIES TIME BUDGET on Init: ≤ 5s from watchdog path

REQ-NF-203  Restart and resume within 5s after normal reset
            → IMPLIES TIME BUDGET on Init: ≤ 5s from remote-restart path

REQ-NF-213  Reach normal operational state within [TBD]s of power-on
            → IMPLIES TIME BUDGET on Init: ≤ [TBD]s on cold boot
              (likely > 5s due to WiFi association and NTP)

REQ-NF-214  Recover known-good state if power lost during flash write
            → ACTION in Init: integrity check on config / buffer / firmware
              (sub-step CheckingIntegrity)

REQ-CC-050  Connect to AWS IoT Core at startup
            → ACTION in Init: trigger Cloud Connectivity sub-machine
              (Init does NOT block on cloud — proceeds to Operational
              regardless; the sub-machine continues independently)
```

### OPERATIONAL

```
REQ-SA-071  Read gateway sensors at configurable polling interval
            → ACTION in Operational: timer-driven sensor poll

REQ-SA-080  Log error code if reading fails
            → ACTION in Operational: error logging (SensorService internal)

REQ-SA-090  Store the most recent [TBD] readings per sensor
            → ACTION in Operational: ring-buffer push

REQ-SA-100  Store a timestamp with each measurement
            → ACTION in Operational: tag with TimeProvider.now()

REQ-SA-110..160, REQ-SA-0E1
            → ACTION in Operational: SensorService pipeline (LLD detail)

REQ-SA-170  Perform additional sensor read on remote read request
            → SELF-TRANSITION in Operational
              trigger: remote_read_received / SensorService.read_now()

REQ-AM-000  Compare sensor measurements with alarm thresholds
REQ-AM-010  Clear alarm when measurement returns to range
REQ-AM-011  Apply hysteresis when clearing alarms
REQ-AM-020  Trigger alarm notification when out of range
            → ACTION set in Operational: AlarmService evaluates per polling
              cycle (gateway has its own sensors per SA-031 → its own alarms).
              Per-channel detail deferred to LLD per-alarm machine.

REQ-AM-030  Send alarm notification to AWS IoT Core
REQ-AM-040  Notification content: sensor ID, type, value, threshold,
            timestamp, device ID
            → ACTION in Operational: on alarm event from AlarmService,
              CloudPublisher.publish_alarm()
            → AM-040 is NO LIFECYCLE IMPLICATION (notification content)

REQ-CC-000  Publish telemetry periodically
            → INTERNAL TRANSITION in Operational
              trigger: telemetry_timer / CloudPublisher.publish_telemetry()

REQ-CC-010  Publish device health periodically
            → INTERNAL TRANSITION in Operational
              trigger: health_timer / CloudPublisher.publish_health()

REQ-CC-020  Publish alarm notifications to cloud
            → covered above by AM-030 path

REQ-CC-030  Configurable telemetry interval
REQ-CC-040  Configurable health interval
            → No new state — affects timer period only

REQ-TS-000 (periodic) / REQ-NF-210 / REQ-NF-211
            → INTERNAL TRANSITIONS in Operational: timer-driven NTP, RTC,
              and field-node-time resync

REQ-TS-030  Push current time to field device via Modbus holding register
            → INTERNAL TRANSITION in Operational
              trigger: time_push_timer / ModbusMaster.write_time_to_field()

REQ-TS-040  Use uptime + "unsynchronised" flag if NTP not done
REQ-NF-212  Same constraint, restated
            → ACTION in Operational: tag every payload via TimeProvider

REQ-NF-200  Continue local acquisition + alarm eval when cloud lost
            → CRITICAL ARCHITECTURAL FACT: cloud loss does NOT change
              gateway top-level state. Operational stays Operational.
              Cloud loss is owned by the Cloud Connectivity sub-machine.
              → confirms NO "Degraded" top-level state needed

REQ-DM-000  Accept operational parameter changes from cloud
REQ-DM-001  Validate remote parameter changes before applying
REQ-DM-002  Send confirm/reject response after processing
            → INTERNAL TRANSITION in Operational
              trigger: remote_config_received / handle_and_ack()
              Note: DIFFERENT from CLI provisioning (which goes to
              EditingConfig). Cloud commands are smaller-scope, self-contained,
              and don't need a confirmation flow — they're already validated
              and acknowledged in this internal transition.

REQ-DM-090  Persist all configuration changes to non-volatile storage
            → ACTION on apply: ConfigStore.write() on every change

REQ-LI-000..LI-020, LI-130..LI-160  CLI diagnostic commands and self-test
            → INTERNAL TRANSITION in Operational
              trigger: cli_diagnostic_received / dispatch_and_respond()
              Diagnostic commands don't change state; provisioning does
              (see EditingConfig).
```

### EDITINGCONFIG

```
REQ-LI-030  CLI provision menu (entry to provisioning flow)
            → IMPLIES TRANSITION: Operational → EditingConfig
              trigger: cli_provision_entered

REQ-LI-040  Receive WiFi credentials via CLI
REQ-LI-050  Receive cloud endpoint via CLI
REQ-LI-060  Receive cloud certificates via CLI
REQ-LI-080  Receive serial port parameters via CLI
            → ACTION in EditingConfig: receive_param() on each input
              Note: REQ-LI-070 (Modbus address) is field-device-only —
              the gateway has no Modbus slave address.

REQ-LI-090  Validate format and range of input values
            → ACTION in EditingConfig: validate_input() on each entry

REQ-LI-100  Request confirmation before applying changes
            → ACTION in EditingConfig: prompt and await confirmation

REQ-LI-110  Apply change if confirmation received
            → IMPLIES TRANSITION: EditingConfig → Operational
              trigger: confirmation_received [validation_ok]
              action: apply_and_persist()

REQ-LI-120  Retain previous configuration until new params successfully applied
            → IMPLIES GUARD on apply: write-then-verify pattern
              ACTION in EditingConfig: snapshot config on entry; restore
              on rollback path

REQ-LI-0E1  Display error if diagnostic command fails (Operational, not here)
REQ-LI-0E2  Display error if input validation fails
REQ-LI-0E3  Discard input parameters if no confirmation received
            → ACTION in EditingConfig: timeout handling and error display
```

### RESTARTING / UPDATINGFIRMWARE / FAULTED

```
REQ-DM-010  Execute remote restart command
            → IMPLIES TRANSITION: Operational → Restarting
              trigger: restart_cmd_received [confirmed]

REQ-DM-020  Request confirmation before executing restart
            → IMPLIES GUARD on Op → Restarting: [user_confirmed]
              ACTION in Operational: send confirm-request to cloud
              (the request lives in Operational; only on confirmation
              do we transition)

REQ-DM-021  Cancel restart if confirmation declined
            → No transition fires when [!user_confirmed]
              Operational stays Operational; cmd discarded

REQ-DM-030  Report restart success after reboot and self-check
            → ACTION in Init (post-restart path): publish success message

UC-18 + DM-054
            → IMPLIES STATE: UpdatingFirmware (composite — opens Firmware
              Update sub-machine).
              IMPLIES GUARD on entry: [authenticated && !update_in_progress]

SRS §3.2 (cross-cutting reliability)
            → IMPLIES STATE: Faulted (terminal-ish; exit only via reset)
            → No single requirement names this state. Catch-all entry
              triggers:
                - Init: integrity unrecoverable (NF-214 partial-write)
                - Init: all sensors failed
                - Init: SelfChecking failed (DM-040)
                - Operational: watchdog imminent (NF-109)
                - UpdatingFirmware: rollback also failed (NF-204 corner)
```

### Out of scope — routed to other machines

| Reqs                                                       | Routed to                                          |
|------------------------------------------------------------|----------------------------------------------------|
| CC-000, -010, -020, -030, -040 (publish actions)            | Owned by Cloud Connectivity (Machine 2) at the protocol level; surfaced here as internal transitions |
| CC-050 (initial connect), CC-060 (auto-reconnect), BF-*, NF-209, TS-010, TS-0E1 | Cloud Connectivity (Machine 2)                     |
| DM-050..-074, -080, -055, -056, NF-204                      | Firmware Update (Machine 3)                        |
| MB-* (master), NF-103..-105, NF-201, NF-215                 | Modbus Master (Machine 4)                          |
| CC-070, -071, -080, -090, NF-206, -207, -216                | No lifecycle implication (payload/QoS details)     |

## Step 2 — State list

### 1. Init — composite

**Purpose:** bring all subsystems up, validate post-reset integrity, gate
entry to Operational on a self-check.

- **Entry action:** record reset cause from RCC; start Init time-budget timer.
- **Do activity:** progress through sub-steps below.
- **Exit action:** stop Init time-budget timer.

| # | Sub-state           | Activity                                                         | Failure handling                                            |
|---|---------------------|------------------------------------------------------------------|-------------------------------------------------------------|
| 1 | CheckingIntegrity   | Verify config / buffer / firmware partitions are not partially written | Unrecoverable corruption → Faulted (NF-214)            |
| 2 | LoadingConfig       | `ConfigStore.read()`; fall back to defaults on failure           | Partial fallback OK, never fatal (SA-010, -020, -050)       |
| 3 | BringingUpSensors   | Initialise on-board sensors with manufacturer defaults           | ≥1 sensor OK → continue; all fail → Faulted                  |
| 4 | StartingMiddleware  | Trigger Cloud Connectivity, NTP, Modbus Master sub-machines      | Sub-machines own their own failure recovery                 |
| 5 | SelfChecking        | Verify sensors readable and Modbus link present                  | Pass → Operational; Fail → Faulted (DM-040)                 |

**Time budgets:**
- Cold boot ≤ [TBD]s (NF-213) — includes WiFi association
- Watchdog reset ≤ 5s (NF-202)
- Normal/remote reset ≤ 5s (NF-203)

**Note:** NTP completion is *not* a precondition for SelfChecking. The system
must reach Operational regardless of NTP success — TimeProvider provides
uptime-based timestamps until sync arrives (TS-040).

**Traceability:** SA-000, -010, -020, -031, -040, -050, -060; TS-000, -020;
DM-030, -040; NF-202, -203, -213, -214; CC-050.

### 2. Operational — simple

**Purpose:** the gateway does its job — sensors, alarms, Modbus polling,
publishing, remote commands, CLI diagnostics.

- **Entry action:** publish "restart success" to cloud if entered from
  Restarting (DM-030); start all periodic timers.
- **Do activity:** see internal-transition table below.
- **Exit action:** stop periodic timers; flush in-flight Modbus transaction.

**Why simple, not composite:** the apparent variations (cloud-up vs cloud-down,
time-synced vs not, Modbus-up vs not, alarm-active vs alarm-clear) are tracked
by sub-machines and flags, not by lifecycle sub-states. Promoting these
distinctions to top-level sub-states would duplicate logic that already lives
in sub-machines and per-channel alarm logic (Open Question #2).

**Traceability:** SA-071, -080, -090, -100, -110..160, -170;
AM-000, -010, -011, -020, -030; CC-000, -010, -020, -030, -040;
TS-000 (periodic), -030, -040; DM-000, -001, -002, -090;
LI-000..020, -130..160; NF-200, -210, -211, -212.

### 3. EditingConfig — simple

**Purpose:** Field Technician is provisioning the gateway via CLI. Sensor
acquisition, alarm evaluation, and Modbus polling continue (handled by their
own tasks/sub-machines below the lifecycle); but configuration parameters —
especially WiFi credentials, cloud endpoint, and certificates — take effect
only on confirmation, with rollback on failure.

- **Entry action:** open provisioning menu on CLI; start confirmation-timeout
  timer; snapshot current config for rollback.
- **Do activity:** receive parameter inputs; validate each (LI-090); display
  feedback; await confirmation.
- **Exit action:** close provisioning menu; clear timeout timer; on apply,
  persist to flash and signal Cloud Connectivity to reconnect with new params
  (LI-110, LI-120, DM-090); on cancel/timeout, restore snapshot.

**Why a top-level state and not an Operational sub-state:**
- Behaviour differs measurably — the CLI is dedicated to the provisioning
  flow.
- Has a timeout with a distinct outcome (LI-0E3).
- Triggered by an external user action and exited by a distinct event.
- Symmetric to the field-device EditingConfig — easier to reason across both
  boards if both have the same shape.

**Traceability:** LI-030, -040, -050, -060, -080, -090, -100, -110, -120,
-0E2, -0E3; DM-090.

**Note:** unlike the field device's EditingConfig, gateway-side parameter
changes interact with Cloud Connectivity. On apply, Cloud Connectivity must
disconnect and reconnect using the new parameters. This is signalled across
machine boundaries by an `internet_params_changed` event consumed by the
Cloud Connectivity machine.

### 4. Restarting — simple

**Purpose:** persist state and trigger a controlled MCU reset on remote
command.

- **Entry action:** persist pending writes (`ConfigStore.flush()`, buffer
  flush); log restart cause; signal LED pattern.
- **Do activity:** wait briefly for in-flight MQTT publish (bounded
  timeout); trigger MCU reset.
- **Exit action:** none — the MCU resets; "exit" is the reset itself. Next
  Init publishes DM-030 ("restart success") on this transition's behalf.

**Note:** the restart-confirmation request (DM-020) lives in Operational, not
Restarting. We only enter Restarting *after* confirmation. If declined
(DM-021), no transition fires.

**Traceability:** UC-17; DM-010, -020, -021, -030, -040.

### 5. UpdatingFirmware — simple at top, composite in own diagram

**Purpose:** delegate the update lifecycle to the Firmware Update sub-machine
(Machine 3); isolate it from normal operation because flash banks, signature
verification, and rollback are too internally complex to nest in the
top-level diagram.

- **Entry action:** acquire firmware-update mutex; suspend periodic telemetry
  publishing; log entry.
- **Do activity:** delegated entirely to the Firmware Update sub-machine.
- **Exit action:** release firmware-update mutex; report final outcome to
  cloud (DM-055):
  - on success → Restarting (boot the new image)
  - on failure with rollback OK → Operational
  - on unrecoverable failure → Faulted

**Traceability at this level:** UC-18; DM-054, DM-055.

### 6. Faulted — simple, terminal-ish

**Purpose:** unrecoverable condition detected. Stay in a safe, observable
state until external/watchdog reset.

- **Entry action:** stop all periodic timers; capture fault context to flash
  log; set fault-indicator LED pattern; if cloud still up, attempt single
  best-effort fault report (no retry — system will reset before retrying).
- **Do activity:** maintain LED pattern; refuse to start any normal operation.
- **Exit action:** none — exit only via hardware/watchdog reset.

**Traceability:** Cross-cutting in §3.2 Reliability.

## Step 3 — Transition table

### State transitions

| #   | From                       | To                          | Event                                             | Guard                                          | Action                                                    |
|-----|----------------------------|-----------------------------|---------------------------------------------------|------------------------------------------------|-----------------------------------------------------------|
| T1  | `[*]`                      | Init.CheckingIntegrity      | —                                                 | —                                              | —                                                         |
| T2  | Init.CheckingIntegrity     | Init.LoadingConfig          | `integrity_check_done`                            | `[partitions_ok]`                              | —                                                         |
| T3  | Init.CheckingIntegrity     | Faulted                     | `integrity_check_done`                            | `[!partitions_ok]`                             | `log_fault(corrupt_flash)`                                |
| T4  | Init.LoadingConfig         | Init.BringingUpSensors      | `config_load_done`                                | —                                              | —                                                         |
| T5  | Init.BringingUpSensors     | Init.StartingMiddleware     | `sensor_init_done`                                | `[≥1 sensor available]`                        | —                                                         |
| T6  | Init.BringingUpSensors     | Faulted                     | `sensor_init_done`                                | `[no sensor available]`                        | `log_fault(no_sensors)`                                   |
| T7  | Init.StartingMiddleware    | Init.SelfChecking           | `middleware_started`                              | —                                              | —                                                         |
| T8  | Init.SelfChecking          | Operational                 | `self_check_done`                                 | `[self_check_ok]`                              | —                                                         |
| T9  | Init.SelfChecking          | Faulted                     | `self_check_done`                                 | `[!self_check_ok]`                             | `log_fault(self_check_failed)`                            |
| T10 | Operational                | EditingConfig               | `cli_provision_entered`                           | —                                              | snapshot config; open provision menu                      |
| T11 | EditingConfig              | Operational                 | `confirmation_received`                           | `[validation_ok]`                              | apply_and_persist(); emit internet_params_changed         |
| T12 | EditingConfig              | Operational                 | `cancel_received` / `confirmation_timeout`        | —                                              | restore snapshot (LI-0E3)                                 |
| T13 | Operational                | Restarting                  | `restart_cmd_received`                            | `[user_confirmed]`                             | persist_state(); log(restart_requested)                   |
| T14 | Operational                | UpdatingFirmware            | `ota_cmd_received`                                | `[authenticated && !update_in_progress]`       | acquire_update_mutex()                                    |
| T15 | Operational                | Faulted                     | `watchdog_imminent` / `unrecoverable_error`       | —                                              | log_fault(<cause>)                                        |
| T16 | EditingConfig              | Faulted                     | `watchdog_imminent` / `unrecoverable_error`       | —                                              | restore snapshot; log_fault(<cause>)                      |
| T17 | Restarting                 | (MCU reset)                 | `reset_triggered`                                 | —                                              | NVIC_SystemReset()                                        |
| T18 | UpdatingFirmware           | Restarting                  | `update_done`                                     | `[update_success]`                             | report_to_cloud(success); release_mutex()                 |
| T19 | UpdatingFirmware           | Operational                 | `update_done`                                     | `[update_failed && rollback_ok]`               | report_to_cloud(failed_rolled_back); release_mutex()      |
| T20 | UpdatingFirmware           | Faulted                     | `update_done`                                     | `[update_failed && !rollback_ok]`              | log_fault(update_unrecoverable); release_mutex()          |
| T21 | Faulted                    | (MCU reset)                 | `watchdog_expiry` / `external_reset`              | —                                              | —                                                         |

### Internal transitions in Operational

| #   | Event                              | Guard                  | Action                                                      |
|-----|------------------------------------|------------------------|-------------------------------------------------------------|
| I1  | `polling_timer_tick`               | —                      | SensorService.read_cycle()                                  |
| I2  | `new_processed_reading`            | —                      | AlarmService.evaluate() (AM-000, -010, -011)                |
| I3  | `alarm_event`                      | —                      | CloudPublisher.publish_alarm() (AM-020, -030, CC-020)       |
| I4  | `telemetry_timer`                  | —                      | CloudPublisher.publish_telemetry() (CC-000, -030)           |
| I5  | `health_timer`                     | —                      | CloudPublisher.publish_health() (CC-010, -040)              |
| I6  | `ntp_resync_timer`                 | —                      | TimeProvider.resync() (NF-210)                              |
| I7  | `time_push_timer`                  | —                      | ModbusMaster.write_time_to_field_device() (TS-030, NF-211)  |
| I8  | `remote_read_received`             | —                      | SensorService.read_now() (SA-170)                           |
| I9  | `remote_config_received`           | `[validation_ok]`      | apply_config(); persist(); ack_cloud(ok) (DM-000..002, -090)|
| I10 | `remote_config_received`           | `[!validation_ok]`     | ack_cloud(rejected)                                         |
| I11 | `restart_cmd_received`             | `[!user_confirmed]`    | discard_cmd() (DM-021)                                      |
| I12 | `ota_cmd_received`                 | `[!authenticated]`     | reject_cmd()                                                |
| I13 | `ota_cmd_received`                 | `[update_in_progress]` | reject_cmd() (DM-054)                                       |
| I14 | `cli_diagnostic_received`          | `[cmd_recognised]`     | dispatch_and_respond() (LI-000..020, -130..160)             |
| I15 | `cli_diagnostic_received`          | `[!cmd_recognised]`    | display error (LI-0E1)                                      |

### Internal transitions in EditingConfig

| #   | Event             | Guard               | Action                                              |
|-----|-------------------|---------------------|-----------------------------------------------------|
| I16 | `cli_param_input` | `[validation_ok]`   | accept input; show on CLI                           |
| I17 | `cli_param_input` | `[!validation_ok]`  | reject; show error (LI-0E2)                         |

---

# Part E — Machine 2: Cloud Connectivity Sub-Machine

Owned by the gateway. Models the gateway's relationship with AWS IoT Core:
connect, publish, lose connection, reconnect, drain buffer.

## Step 1 — Requirements → behaviours

```
REQ-CC-000  Publish telemetry periodically
            → ACTION in Connected.Publishing: forward telemetry to MQTT

REQ-CC-010  Publish device health periodically
            → ACTION in Connected.Publishing: forward health to MQTT

REQ-CC-020  Publish alarm notifications
            → ACTION in Connected.Publishing: forward alarm to MQTT

REQ-CC-030  Configurable telemetry interval
REQ-CC-040  Configurable health interval
            → No new state — affects timer period in caller (Operational)

REQ-CC-050  Connect to AWS IoT Core via MQTT/TLS at startup
            → ACTION in Connecting (initial state): mqtt.connect()

REQ-CC-060  Reconnect automatically if MQTT lost
            → IMPLIES STATE: Disconnected
              IMPLIES TRANSITION pair: Connected ↔ Disconnected ↔ Connecting

REQ-BF-000  Buffer outbound messages when connectivity lost
            → ACTION in Disconnected: enqueue all outbound to flash log

REQ-BF-010  Publish buffered in chronological order on reconnect
            → IMPLIES SUB-STATE Connected.Draining (distinct from Publishing)
              ACTION on Connecting → Connected entry: choice point picks
              Draining if buffer non-empty, else Publishing

REQ-BF-020  Discard oldest when buffer full
            → IMPLIES GUARD on Disconnected internal enqueue:
              [buffer_full] / drop_oldest(); enqueue(msg)

REQ-NF-209  Reconnect attempts at 1 Hz
            → ACTION in Disconnected: 1 Hz reconnect timer drives the
              Disconnected → Connecting transition

REQ-TS-0E1  Retry NTP sync as soon as internet is restored
            → ACTION on entry to Connected: emit "internet_restored" event
              for TimeProvider/NtpClient (consumed outside this machine)

EVENT (cross-machine)
            internet_params_changed (from Gateway lifecycle EditingConfig
            apply path) → forces Connected → Disconnected to re-handshake
            with new credentials/endpoint/cert

REQ-CC-070, -071, -080, -090, NF-206, -207, -216
            → NO LIFECYCLE IMPLICATION (payload/topic/QoS details)
```

## Step 2 — State list

### 1. Connecting — simple, initial

**Purpose:** establish the MQTT/TLS session — at boot, or recovering from
Disconnected.

- **Entry action:** `mqtt_client.connect()` (TLS handshake, X.509 cert auth).
- **Do activity:** await connect outcome.
- **Exit action:** none.

**Traceability:** CC-050; CC-060 (recovery path).

### 2. Connected — composite

**Purpose:** MQTT session is up; gateway publishes outbound messages.

- **Entry action:** reset retry counter; emit `internet_restored` event
  (TS-0E1); choose initial sub-state via choice point.
- **Do activity:** delegated to sub-state.
- **Exit action:** none.

#### 2a. Connected.Draining — simple

**Purpose:** when reconnecting after a disconnection, drain the
store-and-forward buffer in chronological order before resuming live
publishing.

- **Entry action:** start drain (dequeue oldest from CircularFlashLog).
- **Do activity:** publish each buffered message in order; on each ack,
  remove from buffer; continue until buffer empty.
- **Exit action:** none — transitions to Publishing on `buffer_empty`.

**Traceability:** BF-010.

#### 2b. Connected.Publishing — simple

**Purpose:** steady state. Live messages from CloudPublisher are forwarded
to MQTT directly.

- **Entry action:** none.
- **Do activity:** publish telemetry / health / alarm messages as they
  arrive (CC-000, -010, -020, -030, -040).
- **Exit action:** none.

**Traceability:** CC-000, CC-010, CC-020, CC-030, CC-040.

### 3. Disconnected — simple

**Purpose:** MQTT session is down. Buffer outbound messages and retry connect
at 1 Hz.

- **Entry action:** start 1 Hz `reconnect_timer` (NF-209).
- **Do activity:** enqueue outbound messages to CircularFlashLog (BF-000);
  if buffer full, drop oldest and enqueue (BF-020).
- **Exit action:** stop `reconnect_timer`.

**Traceability:** BF-000, BF-020, NF-209.

## Step 3 — Transition table

### State transitions

| #   | From                  | To                                | Event                       | Guard                  | Action                                |
|-----|-----------------------|-----------------------------------|-----------------------------|------------------------|---------------------------------------|
| T1  | `[*]`                 | Connecting                        | —                           | —                      | —                                     |
| T2  | Connecting            | (choice inside Connected)         | `connect_success`           | —                      | —                                     |
| T3  | Connecting            | Disconnected                      | `connect_failure`           | —                      | —                                     |
| T4  | choice                | Connected.Draining                | —                           | `[!buffer_empty]`      | —                                     |
| T5  | choice                | Connected.Publishing              | —                           | `[buffer_empty]`       | —                                     |
| T6  | Connected (boundary)  | Disconnected                      | `connection_lost`           | —                      | —                                     |
| T7  | Connected (boundary)  | Disconnected                      | `internet_params_changed`   | —                      | force_close_session() (cross-machine) |
| T8  | Disconnected          | Connecting                        | `reconnect_timer_tick`      | —                      | —                                     |
| T9  | Connected.Draining    | Connected.Publishing              | `buffer_empty`              | —                      | —                                     |

### Internal transitions

| #   | In state              | Event             | Guard                | Action                                  |
|-----|-----------------------|-------------------|----------------------|-----------------------------------------|
| I1  | Connecting            | `new_message`     | —                    | enqueue(msg)                            |
| I2  | Disconnected          | `new_message`     | `[!buffer_full]`     | enqueue(msg) (BF-000)                   |
| I3  | Disconnected          | `new_message`     | `[buffer_full]`      | drop_oldest(); enqueue(msg) (BF-020)    |
| I4  | Connected.Publishing  | `new_message`     | —                    | mqtt_publish(msg)                       |
| I5  | Connected.Draining    | `new_message`     | —                    | enqueue(msg) (preserve order)           |

**Why I5 enqueues instead of publishing:** while draining the buffer, new
live messages must wait their turn in chronological order. Publishing them
immediately would violate REQ-BF-010 ordering.

---

# Part F — Machine 3: Firmware Update Sub-Machine

The most state-rich machine in the system. Owned by the gateway. Spans two
reboots in the worst case. Corresponds 1:1 with the `UpdatingFirmware`
composite state on the gateway top-level lifecycle.

## Why this is its own machine

Firmware update has a self-contained lifecycle distinct from normal operation:
download → validate → apply → reboot → self-check → commit-or-rollback. The
states inside it are unlike anything in the gateway lifecycle. Modelling it
inline would double the size of the gateway lifecycle diagram with content
that nobody reads when looking at the high-level lifecycle. Promoting it
keeps both diagrams readable.

The cost is the cross-machine coupling: gateway-lifecycle.UpdatingFirmware
↔ FU machine. Coupling is one event in (`update_started`) and one event out
(`update_done` with success/failure flag).

## Step 1 — Requirements → behaviours

```
REQ-DM-050  Download firmware image
            → ACTION in Downloading: write incoming chunks to inactive bank

REQ-DM-051  Resume download from interruption
            → ACTION in Downloading: persist offset; resume on reconnect

REQ-DM-052  Delete partial download after 3 retries fail
            → IMPLIES TRANSITION: Downloading → Failed
              guard: [retries == 3]
              action: delete inactive-bank contents; log error

REQ-DM-053  Log error if download fails
            → ACTION on Downloading → Failed transition: log

REQ-DM-054  Reject new update if one in progress
            → handled at gateway-lifecycle level — out of scope here

REQ-DM-055  Report update result to cloud
            → ACTION on transitions to Committed and Failed: publish result

REQ-DM-056  Only accept updates from authenticated source
            → handled by CloudPublisher / authentication layer before this
              machine starts — pre-condition for entry

REQ-DM-060  Validate firmware image
            → ACTION in Validating: integrity check on downloaded image

REQ-DM-061  Discard and restore current firmware if validation fails
            → IMPLIES TRANSITION: Validating → Failed
              action: invalidate inactive-bank header; do NOT touch active

REQ-DM-062  Log error if validation fails
            → ACTION on Validating → Failed transition: log

REQ-DM-070  Set new firmware partition as boot partition
            → ACTION in Applying: update boot pointer

REQ-DM-071  Trigger self-check on new firmware after boot
            → ACTION on entry to SelfChecking (post-reboot in new firmware):
              run self-check sequence

REQ-DM-072  Roll back if self-check fails
            → IMPLIES TRANSITION: SelfChecking → RollingBack
              guard: [!self_check_ok]

REQ-DM-073  Retain current firmware until self-check success
            → ACTION in Applying: do NOT erase old image
              IMPLIES GUARD on commit: [self_check_ok]

REQ-DM-074  Maintain dual-bank partition scheme
            → ARCHITECTURAL CONSTRAINT (not a state); shapes flash layout

REQ-DM-080  Verify cryptographic signature before applying
            → ACTION in Validating: signature verification (subset of -060)

REQ-NF-204  Roll back and resume normal operation within 10s if update fails
            post-installation
            → IMPLIES TIME BUDGET on RollingBack: ≤ 10s
```

## Note on the two reboot crossings

This machine spans up to two reboots:
1. **End of Applying** — reboot into the new firmware to begin SelfChecking.
2. **End of RollingBack** (if reached) — reboot into the previous firmware
   to reach Failed.

Reboots are not states. They are **transition actions** (`trigger_reboot()`)
followed by `NVIC_SystemReset()`. The machine resumes at a specific state
determined by a flag persisted to non-volatile flags region just before the
reboot:

- `pending_self_check` set → on next boot, gateway-lifecycle Init detects
  the flag and resumes this machine in SelfChecking.
- `pending_rollback` set → on next boot, gateway-lifecycle Init detects the
  flag and resumes this machine in Failed.

This pattern (flag-driven resume after reboot) is standard for OTA update
systems and worth being able to articulate in interview.

## Step 2 — State list

### 1. Idle — simple, initial

**Purpose:** no update in progress. Waits for the gateway top-level to enter
UpdatingFirmware (which only happens after the cloud cmd has passed
authentication and overlap checks).

- **Entry action:** clear update flags from non-volatile flags region.
- **Do activity:** none — passive.
- **Exit action:** none.

### 2. Downloading — simple

**Purpose:** receive firmware image and write it to the inactive flash bank.

- **Entry action:** lock inactive bank; reset retry counter; persist
  offset = 0.
- **Do activity:** receive next chunk; write to inactive bank; persist
  offset (DM-051 resumability); on chunk failure, increment retry counter.
- **Exit action:** finalise inactive bank contents.

**Traceability:** DM-050, -051, -052, -053.

### 3. Validating — simple

**Purpose:** verify cryptographic signature and image integrity of the
downloaded image *before* changing the boot pointer.

- **Entry action:** none.
- **Do activity:** verify signature (DM-080); verify image checksum / format
  (DM-060).
- **Exit action:** none.

**Traceability:** DM-060, -080.

### 4. Applying — simple

**Purpose:** atomically commit the new image as the boot partition; current
firmware retained (DM-073).

- **Entry action:** set `pending_self_check` flag.
- **Do activity:** update boot pointer (DM-070); persist; trigger reboot.
- **Exit action:** `NVIC_SystemReset()` — the transition action *is* the
  reboot.

**Traceability:** DM-070, -073, -074.

### 5. SelfChecking — simple, **resumed after reboot in new firmware**

**Purpose:** verify the freshly-booted firmware is functional.

- **Entry action:** detect `pending_self_check` flag; run self-check sequence
  (sensors readable, communication links up — same checks as gateway-lifecycle
  SelfChecking step).
- **Do activity:** await self-check outcome.
- **Exit action:** clear `pending_self_check` flag.

**Traceability:** DM-071.

**Note:** entry to SelfChecking is via the gateway-lifecycle Init detecting
the `pending_self_check` flag. The Init normally exits to Operational; when
this flag is set, Init exits via SelfChecking of *this* machine.

### 6. RollingBack — simple

**Purpose:** revert to the previous firmware after self-check failed.

- **Entry action:** set `pending_rollback` flag; update boot pointer back
  to the previous partition.
- **Do activity:** persist; trigger reboot.
- **Exit action:** `NVIC_SystemReset()`.

**Time budget:** ≤ 10 s end-to-end including the reboot (NF-204).

**Traceability:** DM-072, NF-204.

### 7. Committed — final

**Purpose:** new firmware accepted. Update lifecycle complete.

- **Entry action:** clear update flags; report success to cloud (DM-055);
  invalidate the now-old bank to free it for the next update.
- **Do activity:** none — terminal.
- **Exit action:** —.

### 8. Failed — final

**Purpose:** update did not complete successfully (download, validation, or
self-check + rollback). Gateway is running on the previous firmware.

- **Entry action:** clear update flags; report failure with cause to cloud
  (DM-055); ensure inactive bank is invalidated.
- **Do activity:** none — terminal.
- **Exit action:** —.

## Step 3 — Transition table

### State transitions

| #   | From          | To             | Event                       | Guard                                | Action                                                                  |
|-----|---------------|----------------|-----------------------------|--------------------------------------|-------------------------------------------------------------------------|
| T1  | `[*]`         | Idle           | —                           | —                                    | —                                                                       |
| T2  | Idle          | Downloading    | `update_started`            | —                                    | start downloader; persist offset = 0                                    |
| T3  | Downloading   | Validating     | `download_complete`         | —                                    | finalise image                                                          |
| T4  | Downloading   | Failed         | `download_chunk_failure`    | `[retries == 3]`                     | delete partial image; log error (DM-052, -053)                          |
| T5  | Validating    | Applying       | `validation_done`           | `[signature_ok && image_ok]`         | —                                                                       |
| T6  | Validating    | Failed         | `validation_done`           | `[!signature_ok || !image_ok]`       | discard image; log error (DM-061, -062)                                 |
| T7  | Applying      | (MCU reset)    | `apply_done`                | —                                    | set `pending_self_check`; persist; NVIC_SystemReset()                   |
| T8  | (post-reboot) | SelfChecking   | `boot_with_pending_flag`    | `[pending_self_check]`               | resumed by gateway Init detecting flag                                  |
| T9  | SelfChecking  | Committed      | `self_check_done`           | `[self_check_ok]`                    | clear flag; report success (DM-055); invalidate old bank                |
| T10 | SelfChecking  | RollingBack    | `self_check_done`           | `[!self_check_ok]`                   | —                                                                       |
| T11 | RollingBack   | (MCU reset)    | `rollback_ready`            | —                                    | set `pending_rollback`; revert boot ptr; persist; NVIC_SystemReset()    |
| T12 | (post-reboot) | Failed         | `boot_with_rollback_flag`   | `[pending_rollback]`                 | clear flag; report failure (DM-055)                                     |
| T13 | Committed     | Idle           | `lifecycle_exits_updating`  | —                                    | —                                                                       |
| T14 | Failed        | Idle           | `lifecycle_exits_updating`  | —                                    | —                                                                       |

### Internal transitions

| #   | In state    | Event                       | Guard               | Action                                              |
|-----|-------------|-----------------------------|---------------------|-----------------------------------------------------|
| I1  | Downloading | `download_chunk_success`    | —                   | write_to_inactive_bank(chunk); persist_offset()     |
| I2  | Downloading | `download_chunk_failure`    | `[retries < 3]`     | retries++; retry chunk (DM-051)                     |

---

# Part G — Machine 4: Modbus Master Sub-Machine

Owned by the gateway. Models the polling cycle for a single Modbus
transaction against the field device. **Active machine — drives transitions
via internal timers.**

## Step 1 — Requirements → behaviours

```
REQ-MB-000  Update Modbus register on new sensor measurement
            → SLAVE-SIDE behaviour. Out of scope.

REQ-MB-010  Read sensor data from field device via input registers
            → ACTION in Transmitting: build read-input-registers request

REQ-MB-020  Write current time to field device via holding register
            → ACTION in Transmitting: build write-holding-register request

REQ-MB-030  RTU @ 9600 / 8N1
            → NO LIFECYCLE IMPLICATION (driver setting)

REQ-MB-040  Support function codes 03, 04, 06, 16
            → NO LIFECYCLE IMPLICATION (request-builder dispatch)

REQ-MB-050  Timeout request after 200 ms
REQ-NF-105  Same constraint, restated
            → IMPLIES STATE: AwaitingResponse with 200 ms timer
              IMPLIES TRANSITION: AwaitingResponse → Idle on timer expiry

REQ-MB-060  Retry up to 3 times before reporting failure
            → ACTION on AwaitingResponse → Idle (timeout) transition:
              if (per_request_retries < 3) rebuild and retransmit;
              else count as one failed poll.

REQ-NF-103  Declare Modbus failure after 3 consecutive unanswered polls
REQ-NF-215  Report "node offline" to cloud after 3 consecutive failed polls
            → ACTION on poll failure: increment consecutive_failures;
              if consecutive_failures == 3 and link == Online,
              transition link to Offline and emit "node_offline" event

REQ-NF-104  Declare Modbus restored after 3 consecutive successful polls
            → ACTION on poll success: increment consecutive_successes;
              if consecutive_successes == 3 and link == Offline,
              transition link to Online and emit "node_online" event

REQ-NF-201  Recover Modbus comms automatically without restart
            → No additional state. Polling cycle resumes naturally.

REQ-MB-070  Define register map (addresses, types, modes)
            → NO LIFECYCLE IMPLICATION (LLD register-map document)

REQ-MB-080, -090  Forward remote commands and return result
            → ACTION in Transmitting / ProcessingResponse: special request
              flavour + return result to caller

REQ-MB-100  Support multiple field devices by unique slave address
            → ARCHITECTURAL CONSTRAINT, not a state

REQ-MB-0E1  Reject unrecognised remote command, log
            → ACTION in ProcessingResponse: validate format
```

### Modelling decision — link state hysteresis

The Online/Offline link state (NF-103, NF-104, NF-215) is hysteresis on top
of the polling cycle, **not** a separate set of polling states. It's tracked
as a model variable (`link_state ∈ {Online, Offline}`) and `consecutive_*`
counters. Transitions to/from Offline emit `node_offline` / `node_online`
events consumed by HealthMonitor and CloudPublisher.

Alternative — an HSM with Online and Offline as composite states each
containing the same polling sub-states — was rejected because it duplicates
the polling cycle visually with no behavioural difference inside the
composites.

## Step 2 — State list

### 1. Idle — simple, initial

**Purpose:** between polls. Polling timer drives the next transaction.

- **Entry action:** none.
- **Do activity:** await `poll_timer_tick` or `command_to_send`.
- **Exit action:** select next request based on caller.

### 2. Transmitting — simple

**Purpose:** drive the request frame onto the RS-485 bus.

- **Entry action:** build request frame (function code, slave addr, payload,
  CRC); enable TX driver; start UART transmit.
- **Do activity:** wait for `tx_complete`.
- **Exit action:** disable TX driver (release the bus).

### 3. AwaitingResponse — simple

**Purpose:** wait for the slave's response within the 200 ms timeout.

- **Entry action:** start 200 ms response timer; enable RX.
- **Do activity:** wait for `rx_complete` or `response_timer_expired`.
- **Exit action:** stop response timer.

### 4. ProcessingResponse — simple

**Purpose:** validate the received frame and dispatch result.

- **Entry action:** none.
- **Do activity:** check CRC; verify slave address and function code match
  request; parse payload; dispatch to caller.
- **Exit action:** update counters; emit `node_offline` / `node_online`
  event if hysteresis threshold crossed.

## Step 3 — Transition table

### State transitions

| #   | From               | To                  | Event                       | Guard                              | Action                                                          |
|-----|--------------------|---------------------|-----------------------------|------------------------------------|-----------------------------------------------------------------|
| T1  | `[*]`              | Idle                | —                           | —                                  | —                                                               |
| T2  | Idle               | Transmitting        | `poll_timer_tick`           | `[poll_pending]`                   | build request frame                                             |
| T3  | Idle               | Transmitting        | `command_to_send`           | —                                  | build request frame for incoming command                        |
| T4  | Transmitting       | AwaitingResponse    | `tx_complete`               | —                                  | start 200 ms response timer                                     |
| T5  | AwaitingResponse   | ProcessingResponse  | `rx_complete`               | —                                  | stop response timer                                             |
| T6  | AwaitingResponse   | Transmitting        | `response_timer_expired`    | `[per_request_retries < 3]`        | per_request_retries++; retransmit (MB-060)                      |
| T7  | AwaitingResponse   | Idle                | `response_timer_expired`    | `[per_request_retries == 3]`       | record poll failure (see action sidebar)                        |
| T8  | ProcessingResponse | Idle                | `processing_done`           | `[crc_ok && format_ok]`            | record poll success (see sidebar)                               |
| T9  | ProcessingResponse | Idle                | `processing_done`           | `[!crc_ok || !format_ok]`          | record poll failure (see sidebar)                               |

### Action sidebar — counter and link-state logic

**On `record poll failure` (T7, T9):**
```
per_request_retries = 0
consecutive_successes = 0
consecutive_failures += 1
if (link_state == Online && consecutive_failures >= 3) {
    link_state = Offline
    emit node_offline   // → HealthMonitor / CloudPublisher (NF-215)
}
```

**On `record poll success` (T8):**
```
per_request_retries = 0
consecutive_failures = 0
consecutive_successes += 1
if (link_state == Offline && consecutive_successes >= 3) {
    link_state = Online
    emit node_online    // → HealthMonitor / CloudPublisher (NF-104)
}
```

These are transition actions, not separate states.

---

# Part H — Machine 5: Field Device Top-Level Lifecycle

Simpler than the gateway: no cloud, no firmware update, no remote restart.
But complete in its own right.

## Field-device characteristics that shape the lifecycle

- **No cloud connectivity.** Field device never publishes to AWS IoT Core.
- **No NTP / no time source.** Time arrives from the gateway via Modbus
  holding register (REQ-MB-020). Field device updates its RTC from this push.
- **No remote restart.** UC-17 is gateway-only.
- **No firmware update.** UC-18 is gateway-only at this stage.
- **Modbus slave (passive).** Receives polls; never initiates.
- **LCD display.** Major output channel.
- **CLI for provisioning.** Field Technician can change Modbus address,
  serial-port parameters.
- **Simulated sensors.** Software simulation module replaces hardware
  sensors the F469 board lacks (CON-003).

## Step 1 — Requirements → behaviours

### INIT

```
REQ-SA-000  Read polling config from flash at startup
            → ACTION in Init: ConfigStore.read() (sub-step LoadingConfig)

REQ-SA-010  Use default polling interval if config read fails
            → IMPLIES GUARD on Init exit: [config_loaded || defaults_applied]

REQ-SA-020  Use default sensor min/max if config read fails
            → ACTION in Init: same fallback as SA-010

REQ-SA-030  Initialise field node sensor simulation during startup
            → ACTION in Init: simulation bring-up (sub-step BringingUpSensors)
              Note: field-node-specific. CON-003 says F469 has no on-board
              sensors — the simulation module replaces them.

REQ-SA-040  Log error if a sensor fails to initialise
            → ACTION in Init: log per-sensor result

REQ-SA-050  Use default filter parameters if config read fails
            → ACTION in Init: same fallback as SA-010

REQ-SA-060  Continue with available sensors if some fail
            → IMPLIES GUARD on Init exit: [≥ 1 sensor available]

REQ-LD-000  Display required (LCD operational)
            → ACTION in Init: LCD bring-up (sub-step BringingUpLCD)

REQ-NF-202  Restart and resume within 5s after watchdog reset
            → IMPLIES TIME BUDGET on Init: ≤ 5s from watchdog path

REQ-NF-213  Reach normal operational state within [TBD]s of power-on
            → IMPLIES TIME BUDGET on Init: ≤ [TBD]s on cold boot
              (no WiFi/NTP, so likely close to 5s)

REQ-NF-214  Recover known-good state if power lost during flash write
            → ACTION in Init: integrity check on config / firmware
              (sub-step CheckingIntegrity)
              Note: no buffer to check (no store-and-forward on field node).

REQ-LD-200..-240   Splash screen with progress bar during boot
                   → ACTION in Init.BringingUpLCD: render splash on LCD ready
                   → ACTION at each Init sub-step completion: update progress
                   → ACTION on T9 (Init → Operational): dismiss splash
                     Note: splash cannot start before LCD is up. First two
                     sub-steps (CheckingIntegrity, LoadingConfig) run before
                     LCD initialisation; the device is silent during them.

OUT OF SCOPE for field device:
- DM-040 (self-check after restart) — traces only to UC-17, gateway-only
- NF-203 (5s after normal reset) — traces only to UC-17, gateway-only
- TS-000, -010, -020 (NTP-driven sync) — gateway-only
- CC-050 (cloud connect) — gateway-only
```

### OPERATIONAL

```
REQ-SA-070  Read sensors at configurable polling interval
            → ACTION in Operational: timer-driven sensor poll
              (delegates to SensorService task; sensors are simulated on F469)

REQ-SA-080  Log error code if reading fails
            → ACTION in Operational: error logging on read failure

REQ-SA-090  Store the most recent [TBD] readings per sensor
            → ACTION in Operational: ring-buffer push

REQ-SA-100  Store a timestamp with each measurement
            → ACTION in Operational: tag with TimeProvider.now()

REQ-SA-110..160, REQ-SA-0E1
            → ACTION in Operational: SensorService pipeline (LLD detail)

REQ-SA-150  Make processed data available for Modbus and LCD
            → ACTION in Operational: write to ModbusRegisterMap and
              update LCD presentation buffer on each new processed reading

REQ-SA-170  Perform additional sensor read on remote read request
            → SELF-TRANSITION in Operational
              trigger: remote_read_received_via_modbus / SensorService.read_now()
              Note: on the field device, "remote" means via Modbus from
              gateway, not from cloud.

REQ-AM-000  Compare sensor measurements with alarm thresholds
REQ-AM-010  Clear alarm when measurement returns to range
REQ-AM-011  Apply hysteresis when clearing alarms
REQ-AM-020  Trigger alarm notification when out of range
            → ACTION set in Operational: AlarmService evaluates per polling
              cycle; updates ModbusRegisterMap (alarm registers).
              Per-channel detail deferred to LLD per-alarm machine.

REQ-AM-030  Send alarm notification to AWS IoT Core
REQ-AM-040  Notification content
            → AM-030 is GATEWAY-ONLY (field device has no cloud).
              The field device's contribution is updating Modbus alarm
              registers; the gateway reads those and forwards to cloud.
            → AM-040 is NO LIFECYCLE IMPLICATION (notification content)

REQ-LD-010..LD-090  LCD display behaviour (status, sensor values, alarms,
                    Modbus state, navigation)
            → INTERNAL TRANSITION in Operational
              trigger: lcd_refresh_timer (5 Hz, NF-108) / GraphicsLibrary.refresh()

REQ-LI-000..LI-020, LI-130..LI-160  CLI diagnostic commands and self-test
            → SELF-TRANSITION in Operational
              trigger: cli_diagnostic_received / dispatch_and_respond()

REQ-LI-030  Provision menu (entry to provisioning flow)
            → IMPLIES TRANSITION: Operational → EditingConfig
              trigger: cli_provision_entered

REQ-MB-000  Update Modbus register on new sensor measurement
            → ACTION in Operational: write to ModbusRegisterMap on new reading

REQ-DM-090  Persist all configuration changes to non-volatile storage
            → ACTION on apply: ConfigStore.write() on every change
              Note: only LOCAL config changes apply on field device
              (no remote operator path).

REQ-NF-200  Continue local sensor acquisition + alarm evaluation when
            internet connectivity is lost
            → ARCHITECTURAL FACT: field device never knows or cares about
              internet connectivity. It always operates locally. No state
              implication, but worth recording.

REQ-NF-205  Recover from sensor failure by reinitialising the failed sensor
REQ-NF-208  Substitute defined error indicator for failed sensor
            → ACTION in Operational (SensorService internal): retry init
              on failure; substitute error indicator on permanent failure.

REQ-NF-212  Use uptime timestamps + "unsynchronised" flag if RTC unsynced
            → ACTION in Operational: TimeProvider tags measurements
              "unsynchronised" until time push (REQ-MB-020) arrives from gateway

EVENT (cross-machine)
            modbus_time_push_received (from Modbus Slave when gateway writes
            time-holding register) → INTERNAL TRANSITION:
              RtcDriver.set_time(); TimeProvider.mark_synced()
```

### EDITINGCONFIG

```
REQ-LI-030  Provision menu entry
REQ-LI-070  Receive Modbus address via CLI
REQ-LI-080  Receive serial port parameters via CLI
            → ACTION in EditingConfig: receive_param() on input
              Note: REQ-LI-040 (WiFi creds), -050 (cloud endpoint), -060
              (cloud certs) are GATEWAY-ONLY. The field device's CLI does
              not handle these.

REQ-LI-090  Validate format and range of input values
REQ-LI-100  Request confirmation
REQ-LI-110  Apply change if confirmation received
REQ-LI-120  Retain previous configuration until apply succeeds
REQ-LI-0E2  Display error if validation fails
REQ-LI-0E3  Discard input if no confirmation
            → Same flow as gateway EditingConfig — see Machine 1 for details

REQ-LD-100..LD-150  LCD shows provisioning menu and feedback
            → ACTION in EditingConfig: LCD displays edit menu (different
              from normal display in Operational)
```

### FAULTED

```
SRS §3.2   Cross-cutting reliability
           → IMPLIES STATE: Faulted
           → Catch-all entry triggers:
             - Init: integrity unrecoverable (NF-214)
             - Init: all sensors failed
             - Init: LCD bring-up failed
             - Operational: watchdog imminent (NF-109)
```

## Step 2 — State list

### 1. Init — composite

**Purpose:** bring all subsystems up, validate post-reset integrity, gate
entry to Operational.

- **Entry action:** record reset cause; start Init time-budget timer.
- **Do activity:** progress through sub-steps below.
- **Exit action:** stop Init time-budget timer.

| # | Sub-state           | Activity                                                          | Failure handling                                            |
|---|---------------------|-------------------------------------------------------------------|-------------------------------------------------------------|
| 1 | CheckingIntegrity   | Verify config / firmware partitions are not partially written     | Unrecoverable corruption → Faulted (NF-214)                 |
| 2 | LoadingConfig       | `ConfigStore.read()`; fall back to defaults on failure            | Partial fallback OK, never fatal (SA-010, -020, -050)       |
| 3 | BringingUpSensors   | Initialise sensor simulation module                               | ≥1 sensor OK → continue; all fail → Faulted (SA-030, -040, -060) |
| 4 | BringingUpLCD | Initialise LCD driver and display layer; on success, render initial splash screen with progress bar at 50% | Failure → Faulted |
| 5 | StartingMiddleware | Start Modbus Slave sub-machine; start AlarmService; start CLI; update splash progress bar to 100% on completion of each sub-step | Sub-machines own their own failure recovery |

**Note:** unlike the gateway, the field-device Init has **no SelfChecking
sub-step** — DM-040 traces only to UC-17 (remote restart, gateway-only).
Sensor and LCD verification happens implicitly in BringingUpSensors /
BringingUpLCD.

**Time budgets:** Cold boot ≤ [TBD]s (NF-213); Watchdog reset ≤ 5s (NF-202).

**Traceability:** SA-000, -010, -020, -030, -040, -050, -060;
LD-000; NF-202, -213, -214.

### 2. Operational — simple

**Purpose:** the field device does its job — acquire and process simulated
sensor data, update LCD and Modbus registers, evaluate alarms, respond to
gateway polls and CLI commands.

- **Entry action:** start periodic timers (sensor polling, LCD refresh).
- **Do activity:** see internal-transition table below.
- **Exit action:** stop periodic timers.

**Why simple:** same reasoning as gateway. Variations (alarm-active,
RTC-synced, Modbus-link-up) are tracked by flags and the Modbus Slave
sub-machine, not by lifecycle sub-states. NF-200 reinforces: field device
always operates locally regardless of any external state.

**Traceability:** SA-070, -080, -090, -100, -110..160, -170;
AM-000, -010, -011, -020; LD-010..090;
LI-000..020, -130..160; MB-000; NF-205, -208, -212.

### 3. EditingConfig — simple

**Purpose:** Field Technician is provisioning the field device via CLI.
Sensor acquisition and alarm evaluation continue (handled by their own
tasks); but Modbus address and serial-port settings take effect only on
confirmation, with rollback on failure.

- **Entry action:** open provisioning menu on LCD (LD-100); start
  confirmation-timeout timer; snapshot current config.
- **Do activity:** receive parameter inputs; validate each (LI-090); display
  feedback on LCD; await confirmation.
- **Exit action:** close provisioning menu; clear timeout timer; on apply,
  persist (LI-110, -120, DM-090); on cancel/timeout, restore snapshot.

**Cross-machine impact:** if the Modbus address is changed, on apply we
emit `modbus_address_changed` for the Modbus Slave to pick up. The slave
restarts its address-filter logic without restarting the slave machine
itself — Modbus stays in Idle, just with a new address.

**Traceability:** LI-030, -070, -080, -090, -100, -110, -120, -0E2, -0E3;
LD-100..150; DM-090.

### 4. Faulted — simple, terminal-ish

Same shape as gateway Faulted. Entry triggers listed in Step 1.

## Step 3 — Transition table

### State transitions

| #   | From                       | To                          | Event                                          | Guard                            | Action                                                  |
|-----|----------------------------|-----------------------------|------------------------------------------------|----------------------------------|---------------------------------------------------------|
| T1  | `[*]`                      | Init.CheckingIntegrity      | —                                              | —                                | —                                                       |
| T2  | Init.CheckingIntegrity     | Init.LoadingConfig          | `integrity_check_done`                         | `[partitions_ok]`                | —                                                       |
| T3  | Init.CheckingIntegrity     | Faulted                     | `integrity_check_done`                         | `[!partitions_ok]`               | log_fault(corrupt_flash)                                |
| T4  | Init.LoadingConfig         | Init.BringingUpSensors      | `config_load_done`                             | —                                | —                                                       |
| T5  | Init.BringingUpSensors     | Init.BringingUpLCD          | `sensor_init_done`                             | `[≥1 sensor available]`          | —                                                       |
| T6  | Init.BringingUpSensors     | Faulted                     | `sensor_init_done`                             | `[no sensor available]`          | log_fault(no_sensors)                                   |
| T7  | Init.BringingUpLCD         | Init.StartingMiddleware     | `lcd_init_done`                                | `[lcd_ok]`                       | —                                                       |
| T8  | Init.BringingUpLCD         | Faulted                     | `lcd_init_done`                                | `[!lcd_ok]`                      | log_fault(lcd_failed)                                   |
| T9 | Init.StartingMiddleware | Operational | middleware_started | — | dismiss splash screen | —                                | —                                                       |
| T10 | Operational                | EditingConfig               | `cli_provision_entered`                        | —                                | snapshot config; show provision menu (LD-100)           |
| T11 | EditingConfig              | Operational                 | `confirmation_received`                        | `[validation_ok]`                | apply_and_persist(); emit modbus_address_changed if applicable |
| T12 | EditingConfig              | Operational                 | `cancel_received` / `confirmation_timeout`     | —                                | restore snapshot (LI-0E3)                               |
| T13 | Operational                | Faulted                     | `watchdog_imminent` / `unrecoverable_error`    | —                                | log_fault(<cause>)                                      |
| T14 | EditingConfig              | Faulted                     | `watchdog_imminent` / `unrecoverable_error`    | —                                | restore snapshot; log_fault(<cause>)                    |
| T15 | Faulted                    | (MCU reset)                 | `watchdog_expiry` / `external_reset`           | —                                | —                                                       |

### Internal transitions in Operational

| #   | Event                              | Guard               | Action                                                |
|-----|------------------------------------|---------------------|-------------------------------------------------------|
| I1  | `polling_timer_tick`               | —                   | SensorService.read_cycle()                            |
| I2  | `lcd_refresh_timer` (5 Hz)         | —                   | GraphicsLibrary.refresh() (NF-108)                    |
| I3  | `new_processed_reading`            | —                   | update_modbus_registers(); update_lcd_buffer() (SA-150, MB-000) |
| I4  | `new_processed_reading`            | —                   | AlarmService.evaluate() (AM-000..-020)                |
| I5  | `modbus_time_push_received`        | —                   | RtcDriver.set_time(); TimeProvider.mark_synced()      |
| I6  | `remote_read_received_via_modbus`  | —                   | SensorService.read_now() (SA-170)                     |
| I7  | `cli_diagnostic_received`          | `[cmd_recognised]`  | dispatch_and_respond() (LI-000..020, -130..160)       |
| I8  | `cli_diagnostic_received`          | `[!cmd_recognised]` | display error (LI-0E1)                                |

### Internal transitions in EditingConfig

| #   | Event                | Guard               | Action                                              |
|-----|----------------------|---------------------|-----------------------------------------------------|
| I9  | `cli_param_input`    | `[validation_ok]`   | accept input; show on LCD                           |
| I10 | `cli_param_input`    | `[!validation_ok]`  | reject; show error (LI-0E2)                         |

---

# Part I — Machine 6: Modbus Slave Sub-Machine

Owned by the field device. Models the response cycle for a single Modbus
transaction received from the gateway. **Reactive: no timers, no retries,
no polling.** Frame reception drives every transition.

## Step 1 — Requirements → behaviours

```
REQ-MB-000  Update Modbus register on new sensor measurement
            → INTERNAL ACTION (in Idle): write to ModbusRegisterMap when
              SensorService produces a new reading. Not a state change —
              register memory updates independently of slave state.

REQ-MB-010  Gateway reads sensor data via input registers
            → ACTION in ProcessingRequest: handle function code 04
              (read input registers); read from ModbusRegisterMap;
              build response

REQ-MB-020  Gateway writes time via holding register
            → ACTION in ProcessingRequest: handle function code 06/16
              (write holding register); update local time/RTC; emit
              modbus_time_push_received event for Operational

REQ-MB-030  RTU @ 9600 / 8N1
            → NO LIFECYCLE IMPLICATION (UART driver setting)

REQ-MB-040  Support function codes 03, 04, 06, 16
            → NO LIFECYCLE IMPLICATION (request dispatch table)
              Note: any other function code triggers a Modbus exception
              response (illegal function — code 0x01).

REQ-MB-070  Define register map (addresses, types, modes)
            → NO LIFECYCLE IMPLICATION (LLD register-map document)

REQ-MB-080  Receive remote commands via holding register
            → ACTION in ProcessingRequest: when function 06/16 targets the
              command register, dispatch to command handler; write result
              to result register

REQ-MB-090  Make command result readable via Modbus
            → ACTION in ProcessingRequest: handle function code 03 read of
              the result register

REQ-MB-100  Support multiple field devices by unique slave address
            → ARCHITECTURAL CONSTRAINT: address comparison is the first
              validation step on every received frame. Frames not addressed
              to this slave are silently dropped.

REQ-MB-0E1  Reject unrecognised remote command, log
            → ACTION in ProcessingRequest: validate command opcode; if
              not recognised, build Modbus exception response and log

REQ-NF-201  Recover Modbus comms automatically without restart
            → No state. A bad frame is silently dropped; slave returns to
              Idle ready for next frame.

EVENT (cross-machine)
            modbus_address_changed (from field-device EditingConfig apply
            path) → updates the address-filter without restarting machine

CRITICAL SCOPING NOTE:
- REQ-MB-050 (timeout 200 ms) and REQ-NF-105 are MASTER-SIDE.
  The slave does not time out — it either receives a frame or it doesn't.
- REQ-MB-060 (retry up to 3 times) is MASTER-SIDE. The slave never retries.
- REQ-NF-103 / -104 (link-state hysteresis counters) are MASTER-SIDE. The
  slave has no link-state hysteresis.
```

## Step 2 — State list

### 1. Idle — simple, initial

**Purpose:** wait for an incoming Modbus frame on the RS-485 bus.

- **Entry action:** enable UART RX; arm inter-frame silence detector
  (3.5 character times for Modbus RTU framing — driver-level detail).
- **Do activity:** wait for `frame_complete` event.
- **Exit action:** disable RX; pass received frame buffer to processing.

**Internal action:** on `new_processed_reading_available` event (from
SensorService outside the slave), write the value to ModbusRegisterMap
(REQ-MB-000). Does not change state.

**Traceability:** MB-000, MB-100 (address filter applies on exit).

### 2. ProcessingRequest — simple

**Purpose:** validate the received frame and produce a response (or decide
not to respond).

- **Entry action:** none.
- **Do activity:** address filter (MB-100) → CRC check → function code
  dispatch (MB-040) → execute (read regs / write regs / dispatch command
  via MB-080, -090) → build response frame; **or** build exception response
  if function code unsupported, address invalid, or command opcode
  unrecognised (MB-0E1); **or** silently mark as "no response" if address
  is not ours or CRC failed.
- **Exit action:** signal whether a response was produced.

**Traceability:** MB-010, MB-020, MB-040, MB-080, MB-090, MB-100, MB-0E1.

### 3. Responding — simple

**Purpose:** transmit the response frame back to the gateway over RS-485.

- **Entry action:** enable TX driver (RS-485 direction control); start
  UART transmit of response frame.
- **Do activity:** wait for `tx_complete`.
- **Exit action:** disable TX driver (release the bus to listening state).

**Traceability:** MB-010, MB-020, MB-080, MB-090.

## Step 3 — Transition table

### State transitions

| #   | From               | To                  | Event                  | Guard                              | Action                              |
|-----|--------------------|---------------------|------------------------|------------------------------------|-------------------------------------|
| T1  | `[*]`              | Idle                | —                      | —                                  | —                                   |
| T2  | Idle               | ProcessingRequest   | `frame_complete`       | —                                  | hand frame buffer to processor      |
| T3  | ProcessingRequest  | Responding          | `processing_done`      | `[response_required]`              | hand response frame to TX           |
| T4  | ProcessingRequest  | Idle                | `processing_done`      | `[!response_required]`             | log silent drop (CRC fail / wrong addr) |
| T5  | Responding         | Idle                | `tx_complete`          | —                                  | —                                   |

### Internal transitions

| #   | In state | Event                              | Guard | Action                                          |
|-----|----------|------------------------------------|-------|-------------------------------------------------|
| I1  | Idle     | `new_processed_reading_available`  | —     | ModbusRegisterMap.write(addr, value) (MB-000)   |
| I2  | Idle     | `modbus_address_changed`           | —     | update address filter                           |

### Why `response_required` is a guard, not a separate state

Three outcomes after processing a frame:
1. Valid frame addressed to us → produce normal response → Responding.
2. Valid frame requesting an unsupported function or operation → produce
   exception response (MB-040 / MB-0E1) → Responding (an exception response
   is still a response).
3. Frame not for us, or CRC failed → silent drop → Idle directly.

Cases 1 and 2 both transit to Responding (the response frame just has a
different shape). Only case 3 short-circuits to Idle. A single guard
`[response_required]` cleanly separates them without inventing a fourth
state.

---

# Part J — Cross-machine relationships

How the six machines connect at runtime.

## Within the gateway

| Coupling                                  | Mechanism                                                                                        |
|-------------------------------------------|--------------------------------------------------------------------------------------------------|
| Gateway lifecycle ↔ Cloud Connectivity    | Init triggers Cloud Connectivity start (CC-050). Otherwise independent — gateway does not block on cloud (NF-200). |
| Gateway lifecycle ↔ Firmware Update       | Top-level UpdatingFirmware composite delegates to FU machine. FU's Committed/Failed return to gateway via T18/T19/T20. The Applying → SelfChecking reboot chain crosses gateway-lifecycle Init in the new firmware. |
| Gateway lifecycle ↔ Modbus Master         | Init starts Modbus Master. Modbus failures surface via `node_offline` event (consumed by HealthMonitor / CloudPublisher); they do NOT change gateway top-level state. |
| Gateway EditingConfig → Cloud Connectivity| On apply, emits `internet_params_changed`; Cloud Connectivity force-disconnects and reconnects with new credentials. |
| Cloud Connectivity ↔ Firmware Update      | FU uses CloudPublisher (which uses Cloud Connectivity) to receive image chunks and report results. If Cloud Connectivity drops mid-update, FU pauses in Downloading and resumes via DM-051. |

## Within the field device

| Coupling                                              | Mechanism                                                                  |
|-------------------------------------------------------|----------------------------------------------------------------------------|
| Field-device lifecycle ↔ Modbus Slave                 | Init.StartingMiddleware brings up the slave; thereafter it runs autonomously. |
| Modbus Slave → Field-device lifecycle (Operational)   | A successful write to the time-push holding register (MB-020) emits `modbus_time_push_received`, consumed by Operational internal transition I5. |
| Sensor pipeline (in Operational) → Modbus Slave       | New processed readings emit `new_processed_reading_available`, consumed by Modbus Slave internal transition I1 (write to register map). |
| Field-device EditingConfig → Modbus Slave             | On Modbus-address change, emits `modbus_address_changed`; Slave updates its address filter without restarting. |

## Across the physical boundary (gateway ↔ field device)

| Coupling                                  | Mechanism                                                                                        |
|-------------------------------------------|--------------------------------------------------------------------------------------------------|
| Modbus Master (gateway) ↔ Modbus Slave (field device) | RS-485 half-duplex. Master initiates; slave responds. The two machines never see each other's internal state — only frames on the bus and the cause/effect they imply. |

Sequence diagrams (Artefact #5) will illustrate these interactions
concretely.

---

# Part K — Open Questions and Decisions Recorded

## Decisions taken

1. **No "Degraded" top-level state on the gateway.** NF-200 makes it
   unnecessary; cloud loss lives inside the Cloud Connectivity sub-machine.
2. **EditingConfig as top-level state on both boards.** Symmetric flow,
   different parameters. Not a sub-state of Operational because it has a
   distinct exit timeout and snapshot-rollback semantics.
3. **Modbus link-state hysteresis as transition actions, not separate
   states.** Keeps the Modbus Master diagram readable; alternative HSM
   approach considered and rejected.
4. **Reboots in Firmware Update are transition actions, not states.** The
   machine resumes via persisted flags detected by gateway-lifecycle Init.
5. **AM-040 is alarm message content, not state persistence.** Alarm state
   is RAM-only; re-evaluated from current sensor values on boot.
6. **"All sensors failed" → Faulted on both boards.** A node with no sensor
   data has no purpose.
7. **NTP is not a precondition for reaching Operational on the gateway.**
   TimeProvider provides uptime-based timestamps until sync arrives (TS-040).
8. **Behaviour during faulted boot** if BringingUpSensors or LoadingConfig fails 
    before BringingUpLCD completes, there's no LCD to display anything. 
    Fault indication in those cases falls back to LED pattern only.
## Open questions deferred to LLD

1. **Per-channel alarm state machine (Clear ↔ Active with hysteresis).**
   Each sensor channel has its own alarm state. The hysteresis logic
   (AM-011) suggests a small state machine per channel, but:
   - It's per-instance, not per-system.
   - The states are trivial (Clear, Active) with a single guarded
     transition pair driven by threshold + hysteresis.
   - Including it in HLD would clutter without adding clarity.
   - LLD is the right place — alongside the per-channel data structures
     and threshold storage.
   Worth mentioning in the HLD companion doc with the rationale above.
2. **Backoff strategy beyond NF-209's 1 Hz constant.** Keep flat 1 Hz;
   revisit if integration testing reveals problems.
3. **Buffer size [TBD]** — driven by CON-009 flash endurance and ASM-004
   SRAM budget.
4. **Cold-boot vs warm-restart Init time budgets** are different (NF-213 vs
   NF-202/-203) — different boot paths, different budgets. Document on the
   diagram.
5. **What does "self-check" cover precisely?** Currently SRS says sensors +
   communication links (DM-040 / DM-071). HLD decision: include MQTT
   reconnect for the firmware-update self-check. Otherwise a broken cloud
   config could ship in firmware.

---

# Part L — Visual Paradigm Drawing Instructions

Six diagrams total in `iot_monitor.vpp`. Apply consistent conventions
across all six.

## Project structure

```
02-architecture/
  state-machines/
    gateway-lifecycle.vpd
    cloud-connectivity.vpd
    firmware-update.vpd
    modbus-master.vpd
    field-device-lifecycle.vpd
    modbus-slave.vpd
```

## Diagram conventions

- **Colour palette:** apply gateway colour (from `diagram-colour-palette.md`)
  to the four gateway diagrams. Field-device colour for the two field-device
  diagrams.
- **State boxes:**
  - Simple state — single box.
  - Composite state — outer box containing inner sub-state diagram.
  - Final state — small filled circle (for Committed / Failed in FU,
    "MCU reset" in lifecycle and FU).
  - Initial pseudo-state — small filled circle with single outgoing arrow.
  - Choice pseudo-state — small diamond.
- **Transition labels:** `event [guard] / action`. If the action is long,
  abbreviate on the diagram and put the full action in this companion doc.
- **Internal transitions** go *inside* the state box, listed below the
  state name. VP supports this — right-click the state, "Add Internal
  Activity".
- **State entry / do / exit:** VP supports these as compartments within the
  state box. Use them, but keep text short.

## Drawing order, per machine

1. **Place state boxes first.** Don't draw transitions yet.
   - Top-down or left-right flow that matches the dominant happy path.
   - Init at top-left, Faulted at bottom-right is conventional.
2. **Place pseudo-states** (initial, final, choice).
3. **Draw state-changing transitions.** Manhattan routing where possible.
4. **Draw composite-state boundary transitions last.** A transition from
   the boundary of a composite covers all sub-states without N parallel
   arrows.
5. **Add internal transitions** inside the relevant state boxes.
6. **Add labels** to all transitions: `event [guard] / action`.

## Per-machine specifics

### gateway-lifecycle.vpd

- Init drawn as **composite** with five sub-states.
- Operational drawn as **simple**. List I1..I15 internal transitions
  inside the box.
- EditingConfig drawn as **simple**. List I16, I17 inside.
- Restarting, UpdatingFirmware, Faulted drawn as **simple**.
- Two final-state circles for the two reset paths (T17, T21).
- 21 state-changing transitions per Step 3 table.

### cloud-connectivity.vpd

- Connected drawn as **composite** with two sub-states (Draining,
  Publishing) and a choice diamond at entry.
- Connecting and Disconnected drawn as **simple**.
- Boundary transitions on Connected (T6, T7) cover both sub-states — don't
  draw two parallel arrows.
- 9 state-changing transitions; 5 internal transitions.

### firmware-update.vpd

- All states **simple** at this level.
- Two final states: Committed and Failed.
- Two "MCU reset" final-state circles for T7 and T11. Add a UML note on
  the diagram explaining "reboot semantics: state machine resumes at
  SelfChecking / Failed via persisted flag detected by gateway Init".

### modbus-master.vpd

- All four states **simple**.
- 9 state-changing transitions.
- Add a UML note titled "Link state hysteresis" containing the action
  sidebar pseudocode from Step 3.

### field-device-lifecycle.vpd

- Init drawn as **composite** with five sub-states (note: BringingUpLCD
  replaces the gateway's SelfChecking).
- Operational drawn as **simple**. List I1..I8 internal transitions inside.
- EditingConfig drawn as **simple**. List I9, I10 inside.
- Faulted drawn as **simple**.
- One final-state circle for the post-Faulted reset (T15).
- 15 state-changing transitions per Step 3 table.

### modbus-slave.vpd

- All three states **simple**: Idle, ProcessingRequest, Responding.
- 5 state-changing transitions (the smallest machine in the system).
- Two internal transitions inside Idle (I1, I2).
- Add a UML note titled "Reactive machine — no timers, no retries" with a
  one-line explanation of why this differs from the Modbus Master diagram.

## After drawing each diagram

- Export as PNG to `docs/diagrams/state-machine-<name>.png`.
- Reference the PNG in `docs/hld/state-machines.md` (the companion
  document — this working document distilled).

## Sanity checklist before merging

For each diagram, walk this list:

- [ ] Every state has at least one incoming transition (no orphans).
- [ ] Every non-final state has at least one outgoing transition.
- [ ] Every transition has an `event [guard] / action` label (any of the
      three may be empty, but the form is consistent).
- [ ] Every state and every transition traces to ≥ 1 REQ-ID or use case.
- [ ] No requirement marked "IMPLIES STATE/TRANSITION/GUARD/ACTION" in
      Step 1 is unaccounted for in the final diagram.
- [ ] Composite states don't have transitions that should be on the
      boundary drawn parallel from each sub-state.
- [ ] Final states (filled circles) are used only for "MCU reset" or
      terminal machine outcomes.

---

**End of working document.** Used as input to:
- `docs/hld/state-machines.md` (companion doc, written after diagrams)
- `iot_monitor.vpp` Visual Paradigm project
- HLD Artefact #4 deliverable on branch `feature/hld-state-machines`
