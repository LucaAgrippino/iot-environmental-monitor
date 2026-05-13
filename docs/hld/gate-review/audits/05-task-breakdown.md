# Audit — `docs/hld/task-breakdown.md`
Auditor: Claude (gate-review chat — Phase 2 → Phase 3 gate)
Severity counts: **4 B / 7 F / 2 D / 2 C**

---

## §1: Purpose and scope — clean.

---

## §2: Drawing conventions [1 defect]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| TB-01 | C | `«semaphore»` stereotype is defined in the legend but never appears in any IPC table (§4.4, §5.4) or ISR table (§6.1). All ISR→task paths use `«notify»`. A dead convention entry confuses future readers. | Remove `«semaphore»` from the legend, or add a one-line note: *"Reserved; no counting-semaphore path exists in v1.0."* |

---

## §3: Engineering method — clean.

---

## §4: Field Device — task design [4 defects]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| TB-02 | B | **LcdUiTask period contradicts REQ-NF-108.** §4.2 states `LcdUiTask` activation as "Periodic (20 ms)" (= 50 Hz); §4.3 restates "LVGL has its own refresh cadence (20 ms)." SRS REQ-NF-108 mandates *"a refresh rate of 5 Hz for the field node LCD"* (= 200 ms period). The document cites REQ-LD-050 as the anchor but REQ-LD-050 says "refresh displayed sensor readings at the polling rate or upon receiving new data" — a data-content rule, not a rendering cadence rule. REQ-NF-108 is an unambiguous non-functional constraint. No rationale is given that bridges the two rates. | Add an architectural decision (e.g. D29): "LVGL renders at 50 Hz for smooth animation; sensor data values are updated at the configurable polling rate. REQ-NF-108's '5 Hz' is interpreted as the data-refresh rate, not the LVGL render cadence." Without this decision the task period is a Blocker contradiction. |
| TB-03 | F | **SensorTask 100 ms period not traced to an SRS requirement.** §4.2 states "Periodic (100 ms tick)" and the schedulability table (§8.1) treats 100 ms as the task period. REQ-NF-100 says "full polling cycle within 100 ms" — that is a **WCET deadline**, not a period. REQ-SA-070 says "configurable." No SRS requirement mandates a 100 ms default sample period. | Document the choice as an architectural decision in §10: *"Default sample period set to 100 ms to honour REQ-NF-100's 100 ms WCET deadline with a comfortable duty cycle."* The anchor column of the task table should cite that decision, not REQ-SA-070 alone. |
| TB-04 | B | **Field Device ConfigStore/ConfigService mutex asserted but absent from §7.** §4.4 states `Any task → ConfigService: "No IPC needed; mutex guards internal state."` Multiple tasks can reach `ConfigService` (ConsoleTask via `ConsoleService`; `ModbusSlaveTask` via `ModbusRegisterMap`). However §7's shared-resource table lists `config_store_mutex` only for the **Gateway**; the Field Device has no corresponding entry. Either the Field Device ConfigStore is unguarded (race condition between `ConsoleTask` and `ModbusSlaveTask`) or a mutex exists but is undocumented. In either case the "one owner per shared resource" rule is violated. | Add a Field Device row to §7: `ConfigStore (FD) | ConfigService (called from ConsoleTask, ModbusSlaveTask) | config_store_mutex_fd | < 20 ms`. |
| TB-05 | F | **DSI_LTDC_IRQHandler in §6.1 ISR table is absent from §4.4 IPC table.** The ISR table lists `DSI_LTDC_IRQHandler → LcdUiTask «notify»` (frame-flush done). §4.4 shows only two notification sources to `LcdUiTask`: the 20 ms timer tick and the touchscreen ISR (explicit "separate bit"). The frame-flush «notify» has no documented notification-bit assignment relative to the other two bits. `LcdUiTask` must distinguish all three wake causes by bit mask. | Add a row to §4.4: `DSI_LTDC_IRQHandler → LcdUiTask | «notify» (bit 2 — frame done) | Signals DMA flush complete; task rearms DMA for next frame.` |

---

## §5: Gateway — task design [3 defects]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| TB-06 | F | **`CloudPublisher → TimeService (post-reconnect)` IPC entry is mechanically ambiguous.** §5.4 states: *"direct call → `«notify»` on `TimeServiceTask`"*. A direct synchronous call from `CloudPublisherTask` cannot simultaneously constitute a `«notify»` to `TimeServiceTask` — the two primitives are incompatible. If `CloudPublisher` calls `TimeService.requestResync()` and that method internally calls `xTaskNotify(hTimeServiceTask, …)`, the IPC is the `«notify»`, not the direct call. The direct-call description implies in-task synchronous execution, which would mean the code runs in `CloudPublisherTask`'s context without involving `TimeServiceTask` at all. | Replace the entry with: `CloudPublisher → TimeServiceTask | «notify» (resync-trigger bit) | CloudPublisher calls TimeService.requestResync() which posts the notification; no blocking.` |
| TB-07 | F | **Shared alarm+event queue has no documented item-type discriminator.** §5.4 routes both `SensorTask (alarm)` and `ModbusPollerTask (event)` into the same queue (depth 8) consumed by `CloudPublisherTask`. CloudPublisherTask publishes to different MQTT topics for gateway alarms vs field-device events; without a discriminator field in the queue-item struct the consumer cannot route correctly. No payload struct or union is documented. | Add a note to the IPC table row: *"Queue item carries a type tag (e.g. `EventType { GATEWAY_ALARM, FD_EVENT }`) plus payload union. Struct definition deferred to LLD per §11 handoff but the discriminator field must exist."* |
| TB-08 | F | **"Timer-driven loop" is not an IPC primitive.** §5.4 entry: `CloudPublisherTask (telemetry timer) → self | timer-driven loop | Internal periodic publish`. An IPC table documents inter-task communication primitives. If the telemetry cadence is driven by a FreeRTOS software timer callback that calls `xTaskNotifyFromISR(hCloudPublisherTask, …)`, the entry should read `«notify»`. If it is a `vTaskDelay()` inside the task body, it is internal implementation and has no place in the IPC table. | Either replace with `FreeRTOS timer → CloudPublisherTask | «notify» (telemetry-tick bit) | Periodic publish trigger` or remove the row and add a sentence in §5.3 explaining the internal loop. |

---

## §6: ISR → task contracts [1 defect]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| TB-09 | B | **`SPI_wifi_IRQHandler` owning task is undefined.** §6.1 lists: `GW | SPI_wifi_IRQHandler | WiFi data ready | CloudPublisherTask *(or owning caller)* | «notify»`. Direct-to-task notification (`xTaskNotifyFromISR`) requires a **fixed task handle** stored at compile time or written once at init. Three tasks share the WiFi resource under `wifi_mutex`: `CloudPublisherTask` (priority 2), `TimeServiceTask` (priority 2), and `UpdateServiceTask` (priority 1). When `TimeServiceTask` or `UpdateServiceTask` holds the mutex, the ISR's hardcoded handle for `CloudPublisherTask` would notify the wrong task. "Or owning caller" is not an implementable ISR contract. This violates the §6 ISR contract ("notify the owning task") and P3 (Interface Segregation — the ISR must have one clearly-specified consumer). | Choose one of: (a) redirect WiFi I/O through a single WiFi management task that always owns the driver (removing the need for `wifi_mutex` across tasks), (b) store the current owner's task handle in a shared variable updated under the mutex, with the ISR reading it before calling `xTaskNotifyFromISR` — and document this contract explicitly in §6. |

---

## §7: Shared-resource locking strategy [1 defect]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| TB-10 | F | **HealthMonitor's internal state is a shared resource with no mutex in §7.** `HealthMonitor` is accessed from multiple task contexts: `SensorTask` and `ModbusSlaveTask` push via `IHealthReport`; `LcdUiTask` and `ConsoleTask` pull via `IHealthSnapshot` (components.md Field Device §4 and Gateway §4). The internal snapshot is mutated on every push and read on every pull. No mutex is listed in §7 for this resource and no guardian is named, violating the "one owner per shared resource" audit requirement. | Add a row to §7: `HealthMonitor snapshot (both boards) | All tasks (IHealthReport writers + IHealthSnapshot readers) | health_mutex | < 1 ms; single struct copy per critical section`. Alternatively, document the internal lock inside HealthMonitor itself and state that it is the guardian. |

---

## §8: Schedulability check [2 defects]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| TB-11 | B | **Retry-storm analysis violates REQ-NF-101.** §8.2 states `ModbusPollerTask` WCET up to 600 ms (timeout + 3 retries) at priority 4, and concludes "other tasks are starved only briefly." `SensorTask` runs at priority 3 with a 100 ms period. During a 600 ms retry storm, `ModbusPollerTask` (p4) preempts `SensorTask` (p3) for the full 600 ms, causing **five to six consecutive missed polling cycles**. REQ-NF-101 requires alarm detection *within one polling cycle*. The Gateway's own sensor alarms (temperature, humidity, IMU, etc.) cannot be detected during this window. "Only briefly" directly contradicts the document's own 600 ms figure and has no quantitative justification. | Either: (a) add a schedulability note quantifying that Gateway sensor alarms are REQ-NF-101 compliant because the FD evaluates its own alarms independently and the Gateway sensor alarm path is only best-effort; (b) lower `ModbusPollerTask` priority to 3 and raise `SensorTask` to 4 and justify the reversal; or (c) document a ceiling on `ModbusPollerTask` WCET (e.g. chunked retries with a yield point). The current text is not testable. |
| TB-12 | F | **§8.2 Gateway schedulability commentary repeats "starved only briefly" without quantification.** The sentence reads: *"acceptable because `ModbusPollerTask` runs at priority 4 and other tasks are starved only briefly during the retry window — by design, not by accident."* The very next cell of the table gives the number: 600 ms peak. A 600 ms starvation of a 100 ms task is not "brief." The rationale is internally self-contradicting and is therefore untestable as a correctness statement. | Replace with a quantified statement: *"During a retry storm (up to 600 ms), `SensorTask` (p3) is preempted. Gateway sensor alarm latency may reach 600 ms — within the accepted operational tolerance because [justification]. `CloudPublisherTask` (p2), `TimeServiceTask` (p2), and lower-priority tasks queue until the storm resolves."* |

---

## §9: Task interaction diagrams — clean.

> Note: diagrams are referenced but not yet committed (`docs/diagrams/`). This is acknowledged in §9 and is a D-level item — see D-01 below.

---

## §10: Architectural decisions — clean.

---

## §11: LLD handoff — clean.

---

## Deferred items

| ID | Severity | Description |
|----|----------|-------------|
| TB-D01 | D | §9 — Task interaction diagrams (`task-interaction-field-device.png`, `task-interaction-gateway.png`) are referenced but not present in `docs/diagrams/`. Acknowledged as work in progress; block LLD start if missing at gate close. |
| TB-D02 | D | §8 — Formal RMS schedulability analysis explicitly deferred to LLD "when WCETs are measured." Acceptable; record for LLD checklist. Note that TB-11 above is a Blocker that must be resolved before this deferral is valid. |

---

## Summary

**Top 3 blockers:**

1. **TB-02 (LcdUiTask period vs REQ-NF-108):** The document sets a 50 Hz LVGL render cadence against an SRS mandate of 5 Hz LCD refresh. The rationale distinguishing data-update rate from rendering cadence is absent; without it the requirement is breached on paper and no reviewer can sign it off.

2. **TB-09 (SPI_wifi_IRQHandler ambiguous owner):** The ISR contract for the WiFi peripheral lists two possible owning tasks. `xTaskNotifyFromISR` takes a fixed handle; the document offers no mechanism for resolving which task is current. This is unimplementable as written and risks notifying the wrong task at runtime.

3. **TB-11 (Retry-storm starves SensorTask for 600 ms, breaching REQ-NF-101):** The schedulability section concedes a 600 ms preemption window for a 100 ms periodic task and calls it "brief." REQ-NF-101 requires one-cycle alarm detection. The contradiction between the claim and the number is literal and untestable.

**Overall posture:** The artefact is structurally sound — the ten-step engineering method is applied consistently, priority rationale is well-reasoned, ISR contracts are correctly constrained, and the decision log (D21–D28) is complete. The weaknesses are localised: a requirement conflict on LCD refresh rate, one unresolvable ISR target, one unquantified schedulability hazard, and a set of specification-quality gaps (missing FD ConfigStore mutex in §7, ambiguous WiFi IPC entry, no queue-item discriminator, HealthMonitor unlisted as shared resource). **Recommend: do not advance to LLD until TB-02, TB-04, TB-09, and TB-11 are resolved.** TB-03, TB-05 through TB-08, TB-10, and TB-12 should be resolved in the same pass to avoid re-opening the artefact during LLD.