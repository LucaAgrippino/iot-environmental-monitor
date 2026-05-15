# Audit — `docs/hld/state-machines.md`
Auditor: Claude (gate-review chat 2)
Severity counts: 8B / 9F / 2D / 2C

---

## §Inventory: clean.

All six machines present; board assignments correct; `LifecycleController` named as
owner of Machine 1 (gateway) and Machine 5 (field device); diagram-vs-companion
discipline stated correctly in Conventions. ✓

---

## §Conventions: clean.

UML notation, pseudo-state set, behavioural-compartment split, and colour rules are
internally consistent and align with `diagram-colour-palette.md §3`. ✓

---

## §Machine 1 — Gateway Lifecycle: State List [3 defects]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-001 | F | **Init sub-states lack per-sub-state entry/do/exit semantics.** The five sub-states (CheckingIntegrity … SelfChecking) are presented in a two-column table (Activity / Failure handling) with no entry action, do activity, or exit action for each. Convention §Behavioural compartments requires every state to carry all three compartments or explicit "none." | Add a three-compartment row block for each sub-state, or explicitly annotate "Entry: none / Do: [activity] / Exit: none" inline in the table. Apply the same fix to Machine 5 Init sub-states. |
| SM-002 | F | **Operational entry action uses source-state identity, which is UML-invalid.** "publish 'restart success' to cloud *if entered from Restarting* (REQ-DM-030)" — UML entry actions cannot branch on the prior active state; the reboot destroys that history. | Replace with "if `restart_flag` set in NV flags region, publish restart-success and clear flag." The flag is already set in Restarting (entry action) so the mechanism exists; the spec wording just needs to reference it. |
| SM-003 | B | **EditingConfig entry/exit actions duplicate T10/T11/T12 transition actions — double invocation of config mutation calls.** T10 action calls `snapshot_config(); show_provision_menu()`. EditingConfig entry action then calls "open provisioning menu … snapshot current config for rollback" — the same two operations fire a second time. Likewise, EditingConfig exit action contains conditional branches ("on apply, persist … emit `internet_params_changed`; on cancel/timeout, restore snapshot") that duplicate the actions of T11 (`apply_and_persist(); emit internet_params_changed`) and T12 (`restore_snapshot()`). In UML, exit actions are unconditional and fire before every outgoing transition's action, so T11's `apply_and_persist()` and `internet_params_changed` are emitted twice; T12's `restore_snapshot()` is called twice. Same defect is present in Machine 5. | Remove duplicated calls from the transition actions (T10, T11, T12) and make the entry/exit actions the single source of truth. Exit action must be unconditional; the "on apply / on cancel" branching belongs exclusively in T11 and T12 actions, not in the exit action. |

---

## §Machine 1 — Gateway Lifecycle: Transition Table [1 defect]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-004 | C | **Restarting traceability includes DM-040.** DM-040 ("complete a self-check after restart, verifying sensor initialisations and communication links") executes in Init.SelfChecking on the *next* boot, not inside the Restarting state itself. | Move DM-040 from Restarting's traceability line to Init.SelfChecking's traceability. |

---

## §Machine 2 — Cloud Connectivity: clean.

States have correct entry/do/exit (Connected exit action explicitly "none" ✓). T1–T9
all carry events except T4/T5 which are choice-pseudo-state outgoing guards (synchronous
— no event required). I1–I5 correct. DM-051-related pause/resume is handled in Machine 3
(see SM-012). ✓

---

## §Machine 3 — Firmware Update: State List [3 defects]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-005 | B | **Applying state — three-way NVIC_SystemReset() conflict; flag set twice.** (1) The do activity says "trigger reboot," meaning it internally calls `NVIC_SystemReset()` and the MCU resets — the exit action therefore never executes. (2) The exit action also says "`NVIC_SystemReset()` — the transition action *is* the reboot," which contradicts the do-activity. (3) T7's action independently calls `set_pending_self_check(); persist(); NVIC_SystemReset()` — a third site for the same reset. (4) `pending_self_check` is set in both the entry action *and* T7's action. It is impossible to determine which of the three sites owns the reboot or when the flag is set relative to the persist(). | Decide one canonical location for the reboot call (T7 action is the natural place: `event apply_done [—] / set_pending_self_check(); persist(); NVIC_SystemReset()`). Do activity: "await apply_done from boot-pointer write." Exit action: "none" (MCU will reset in T7 action before it runs). Remove the duplicate flag-set from the entry action. |
| SM-006 | B | **RollingBack state — identical three-way conflict as Applying.** Do activity says "trigger reboot"; exit action says `NVIC_SystemReset()`; T11 action also says `NVIC_SystemReset()`. `pending_rollback` flag and boot-pointer revert appear in both entry action and T11 action. | Same resolution pattern as SM-005: T11 action owns flag, revert, persist, and NVIC_SystemReset(). Do activity: "await rollback_ready." Exit action: none. Remove duplicated entry-action flag/pointer operations. |
| SM-007 | B | **REQ-DM-055 (report update result to cloud) owned by three conflicting locations.** Machine 3 Committed entry: "report success to cloud (REQ-DM-055)." Machine 3 Failed entry: "report failure with cause to cloud (REQ-DM-055)." Machine 1 T18 action: `report_to_cloud(success)`. Machine 1 T19 action: `report_to_cloud(failed_rolled_back)`. Machine 1 UpdatingFirmware exit action: "report final outcome to cloud (REQ-DM-055)" — which would fire for T20 (Faulted path) as well, yet T20's own action has no cloud report. It is impossible to determine which component sends DM-055, and the report will be sent multiple times on the success and rollback paths. | Assign DM-055 reporting to exactly one location. The most natural owner is the Machine 1 transition action (T18/T19/T20 respectively), because UpdatingFirmware's exit action can observe the outcome via the guard. Remove the cloud-report call from Machine 3 Committed and Failed entry actions and from the UpdatingFirmware exit action. Add a `report_to_cloud(failed_unrecoverable)` call to T20. |

---

## §Machine 3 — Firmware Update: Transition Table [1 defect]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-008 | F | **Committed and Failed terminal states use "—" for exit action, not the document's own "none" convention.** Every other terminal state in the document writes "Exit action: none." | Replace "—" with "none" in Committed and Failed exit action rows. |

---

## §Machine 4 — Modbus Master: State List [2 defects]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-009 | B | **Transmitting entry action overwrites the command frame on the T3 path — functional error.** Transmitting entry action calls `build_request_frame()` (poll frame: function code, slave addr, payload, CRC). T2 (poll path) also calls `build_request_frame()` in its action — redundant but harmless. T3 (command path) calls `build_request_frame_for_incoming_command()` in its action to produce the correct command frame. When T3 fires, the UML execution order is: T3 action (correct command frame built) → Transmitting entry action (`build_request_frame()` overwrites with a poll frame) → `enable TX driver; start UART transmit` (wrong frame transmitted). The system would transmit a polling request in place of every out-of-band command. | Move `enable TX driver; start UART transmit` into the Transmitting entry action only. Remove `build_request_frame()` from the entry action; let T2 and T3 each supply the completed frame via their own actions. The entry action becomes: `enable TX driver; start UART transmit (frame from caller)`. |
| SM-010 | B | **AwaitingResponse timer started twice on T4; stopped twice on T5.** T4 transition action calls `start_response_timer_ms(200)`. AwaitingResponse entry action *also* calls `start_response_timer_ms(200)` (UML order: T4 action first, then entry action). Similarly, T5 transition action calls `stop_response_timer()` and AwaitingResponse exit action *also* stops the timer. Double-starting the timer resets the 200 ms window after the transmit completes, silently extending the effective timeout. | Remove `start_response_timer_ms(200)` from T4's action column (entry action is the correct owner). Remove `stop_response_timer()` from T5's action column (exit action is the correct owner). The actions for T4 and T5 become empty. |

---

## §Machine 4 — Modbus Master: Transition Table [1 defect]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-011 | D | **Guard `[poll_pending]` on T2 is unexplained.** When can `poll_pending` be false while a `poll_timer_tick` is received? If the intent is to suppress polling during firmware update or while a command is in flight, the suppression mechanism is undocumented. Without a note, the guard is untestable. | Add a sidebar note (same style as the link-state hysteresis sidebar) explaining when `poll_pending` is false and what mechanism sets/clears it. Alternatively, document in LLD as a D-item if the condition is determined by higher-layer orchestration. |

---

## §Machine 5 — Field Device Lifecycle: State List [3 defects]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-012 | F | **Init sub-states lack per-sub-state entry/do/exit semantics.** Same defect as SM-001; the five sub-states (CheckingIntegrity … StartingMiddleware) are listed in a table without entry/do/exit compartments for each. | Apply same fix as SM-001. |
| SM-013 | F | **Machine 5 Operational I3 and I4 both trigger on `new_processed_reading` with no guards — non-deterministic execution ordering.** I3 (`update_modbus_registers(); update_lcd_buffer()`) and I4 (`AlarmService.evaluate()`) share the same event and carry no guards. In UML, multiple unguarded internal transitions on the same event in the same state are non-deterministic. Practically, alarm evaluation should see the already-updated data; ordering matters. | Merge I3 and I4 into a single internal transition, or add an explicit sequencing note ("I3 precedes I4") that constrains LLD task dispatch order. |
| SM-014 | F | **REQ-NF-205 traced to Machine 5 Operational but no reinitialisation internal transition or event models it.** NF-205 ("recover from a sensor failure by reinitialising the failed sensor") requires an observable state-machine action. The Operational internal transition table has no `sensor_failure` event or `SensorService.reinitialise()` action. Additionally, NF-205 is absent from Machine 1 Operational traceability, although the gateway carries physical sensors (REQ-SA-071). | Either add an internal transition `sensor_failure_detected [—] / SensorService.reinitialise()` to both Operational tables, or add a companion note stating that NF-205 is fully handled within SensorService's own internal recovery loop and should trace there rather than to the lifecycle machine. Remove the misleading trace from Machine 5 Operational; add the correct trace to Machine 1. |

---

## §Machine 5 — Field Device Lifecycle: Transition Table [1 defect]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-015 | B | **EditingConfig entry/exit actions duplicate T10/T11/T12 actions — same defect as SM-003, present symmetrically on the field device.** T10 action calls `snapshot_config(); show_provision_menu()`, which Machine 5 EditingConfig entry also performs. EditingConfig exit action contains conditional "on apply … emit `modbus_address_changed`; on cancel/timeout, restore snapshot" logic, duplicating T11 and T12 actions. | Apply same resolution as SM-003: exit action unconditional (close menu, clear timer); branching logic exclusively in T11 and T12 actions. |

---

## §Machine 6 — Modbus Slave: State List [2 defects]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-016 | B | **P1 violation (Strict Directional Layering) — Machine 6 I1 action calls `ModbusRegisterMap.write()` from a Middleware-owned state machine.** Machine 6 is owned by `ModbusSlave` (Middleware). `ModbusRegisterMap` is Application layer. `components.md` lists `ModbusSlave` USES only `ModbusUartDriver, ILogger` — no mention of `ModbusRegisterMap`. Middleware calling Application breaks P1 and contradicts the component specification. Furthermore, Machine 5 Operational I3 already calls `update_modbus_registers()` on the same sensor-reading event — two machines claim to write the same registers on the same event. The cross-machine section compounds the confusion by attributing I1 to "Modbus Slave internal transition." | Remove I1 entirely from Machine 6. The correct owner is Machine 5 Operational (I3 already exists). Update the cross-machine field-device section to remove "consumed by Modbus Slave internal transition I1" and confirm that I3 in Machine 5 is the sole register-update path. |
| SM-017 | B | **P10 violation (Naming Conventions) — `new_processed_reading` (Machine 5 I3) and `new_processed_reading_available` (Machine 6 I1) are two names for the same event.** Machine 5 I3 uses `new_processed_reading`; Machine 6 I1, the Idle state description, and the cross-machine field-device section all use `new_processed_reading_available`. With inconsistent names, the cross-machine coupling table cannot unambiguously map the producer to the consumer, and generated code will fail to route the event. | Pick one canonical name and apply it throughout (all six machines, all cross-machine tables, and the companion prose). The shorter form `new_processed_reading` matches the gateway usage (Machine 1 I2) and should be preferred for symmetry. |

---

## §Machine 6 — Modbus Slave: Transition Table: clean.

T1–T5 carry correct events; T3/T4 guard pair is exhaustive (`[response_required]` / `[!response_required]`). I2 (`modbus_address_changed`) correctly scoped to Idle. ✓

---

## §Cross-machine relationships [3 defects]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-018 | F | **`internet_restored` event emitter documented; consumer is not.** Machine 2 Connected entry action emits `internet_restored` citing REQ-TS-0E1 ("retry time update as soon as internet is restored"). The cross-machine section documents no consumer. `TimeService`/`NtpClient` presumably consume it, but this is absent from every cross-machine table. | Add a row to the within-gateway coupling table: "Cloud Connectivity → TimeService: `internet_restored` triggers `NtpClient.query()` (TS-0E1)." |
| SM-019 | F | **`update_done` and `lifecycle_exits_updating` event emitters are undocumented.** Machine 1 T18/T19/T20 consume `update_done` but no producer is listed. Machine 3 T13/T14 consume `lifecycle_exits_updating` but no emitter is listed. Without these, the gateway-lifecycle ↔ firmware-update coupling table is incomplete. | In the coupling table row "Gateway lifecycle ↔ Firmware Update," specify: (a) Machine 3 reaching Committed/Failed emits `update_done` to Machine 1; (b) Machine 1 exiting UpdatingFirmware (via T18/T19/T20) emits `lifecycle_exits_updating` to Machine 3. If UML sub-machine completion semantics are used for (a), state that explicitly. |
| SM-020 | F | **`update_started` event (Machine 3 T2: Idle → Downloading) has no documented emitter anywhere in the document.** Machine 1 T14 action only calls `acquire_update_mutex()`. Once UpdatingFirmware becomes the active state, Machine 3 sits in Idle waiting for `update_started` — but nothing emits it. The download never begins. | Document the emitter. If `LifecycleController` or `UpdateService` emits `update_started` as part of entering UpdatingFirmware, add it to T14's action column and to the cross-machine table. If entry into the UpdatingFirmware composite state is supposed to auto-start Machine 3 via sub-machine semantics, state that the composite entry-point fires unconditionally (in which case T2 should become a completion transition from Machine 3's own initial pseudo-state and `update_started` is not needed). |

---

## §Machine 3 — Downloading: connectivity-loss gap [1 defect]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-021 | F | **DM-051 resumability contradicted by the transition table.** The cross-machine section states: "If Cloud Connectivity drops mid-update, FU pauses in Downloading and resumes via REQ-DM-051." Machine 3's Downloading transition table has no Paused state, no `connection_lost` trigger, and no resume transition. The only failure path is I2 (chunk retry, up to 3) → T4 (retries == 3 → Failed). After 3 failed chunks due to connectivity loss, the machine goes to Failed and deletes the partial image — the opposite of the claimed resumability. | Either add a `Paused` state inside Downloading (entered on `connection_lost`, exited on `internet_restored` → resume from persisted offset), or remove the claim from the cross-machine section and document that DM-051 is satisfied by the retry-from-persisted-offset mechanism without a distinct pause, then align the cross-machine narrative accordingly. |

---

## §Step 1 Derivation [1 defect]

| ID | Sev | Description | Suggested fix |
|----|-----|-------------|---------------|
| SM-022 | F | **No Step 1 requirement-to-state derivation section exists.** The document's preamble describes four sections; none is a derivation step. Requirement IDs are scattered inline (e.g., `(REQ-MB-000)`) but there is no formal mapping of the form "REQ-X implies state S / transition T / guard G / action A." The specialisation check requires every claimed requirement to be labelled in the derivation. Without it, traceability is forward-only (state → req) and cannot be reviewed for gaps (req → no state). | Add a §Derivation section (or renumber as §1 within the machine sections) that lists each SRS functional area covered, the states/transitions it implies, and a one-line justification. The inline trace IDs can remain as quick-look markers; the derivation section is the reviewable record. |

---

## §Decisions and open questions: clean.

All nine decisions are internally consistent. DM-040 trace to Restarting (SM-004) is a
cosmetic issue already flagged. Open questions 1–5 are correctly scoped to LLD. ✓

---

## Summary

**Top three blockers.**

1. **SM-005 / SM-006 (Applying, RollingBack)**: the Firmware Update machine's two
   reboot states each carry three independent sites for `NVIC_SystemReset()` (do-activity,
   exit action, transition action) and duplicate flag/pointer writes in entry and transition
   actions. The MCU reset in the do-activity makes the exit and transition actions
   unreachable; the intended sequencing (set flag → persist → reset) cannot be determined
   from the spec. This must be resolved before LLD can generate correct OTA sequences.

2. **SM-009 / SM-010 (Transmitting, AwaitingResponse in Modbus Master)**: the entry and
   exit actions of these two states duplicate the actions already present on the adjoining
   transitions, causing the response timer to be started and stopped twice, and — critically
   — the command-frame built by T3 to be overwritten by the polling-frame builder in
   Transmitting's entry action. Every out-of-band Modbus command would be silently discarded
   and replaced by a polling read.

3. **SM-007 (DM-055 reporting) and SM-016 (P1 violation in Machine 6)**: two independent
   architectural blockers. DM-055 is claimed by five separate locations across two machines;
   T20's unrecoverable-failure path has no cloud report at all despite the exit action
   claiming otherwise. Machine 6's I1 action calls an Application-layer component from a
   Middleware-owned state machine, contradicting both P1 and `components.md`; the same
   register-update is already handled by Machine 5 I3, making I1 both illegal and redundant.

**Overall posture.** The document demonstrates strong structural coverage — all six
machines are present, `LifecycleController` ownership is correctly recorded,
diagram-vs-companion discipline is stated and followed, and the cross-machine section has
the right shape. The defects cluster around three failure modes: (a) UML entry/exit/action
placement mistakes that would cause double-invocation of side-effecting operations (SM-003,
SM-009, SM-010, SM-015); (b) multi-site ownership of a single behaviour without a declared
canonical site (SM-005 / SM-007); and (c) missing cross-machine event emitters leaving
the gateway ↔ firmware-update coupling table with three undocumented event origins
(SM-019, SM-020, SM-021).

**Recommendation.** Do not bump to v1.0 HLD. Resolve the 8 blockers in a focused revision
sprint (most can be fixed by moving action text between transition and entry/exit cells
without redesign). Re-review the cross-machine section and Firmware Update machine as a
pair after SM-005, SM-006, SM-007, SM-019, SM-020 are addressed, since those five are
mutually dependent.