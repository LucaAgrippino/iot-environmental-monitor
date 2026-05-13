# Gate Review — Intake Summary
Generated: 2026-05-13  
Source: `audits/01-srs.md` through `audits/08-hld-master.md` + `mechanical-report.md`

---

## 1. Total Defects by Severity

### Audit reports (01–08) — B / F / D / C classification

| Report | Artefact | B | F | D | C | Subtotal |
|--------|----------|---|---|---|---|----------|
| 01 | SRS.md | 17 | 17 | 4 | 6 | 44 |
| 02 | components.md | 9 | 11 | 1 | 1 | 22 |
| 03 | state-machines.md | 8 | 9 | 2 | 2 | 21 |
| 04 | sequence-diagrams.md | 5 | 5 | 1 | 2 | 13 |
| 05 | task-breakdown.md | 4 | 7 | 2 | 2 | 15 |
| 06 | modbus-register-map.md | 5 | 7 | 1 | 1 | 14 |
| 07 | flash-partition-layout.md | 4 | 6 | 1 | 0 | 11 |
| 08 | hld.md | 13 | 10 | 0 | 1 | 24 |
| **TOTAL** | | **65** | **72** | **12** | **15** | **164** |

### Mechanical report (Check 1–8) — defects by check

| Check | Description | Defects |
|-------|-------------|---------|
| 1 | Requirement-ID orphans (SRS req never cited in HLD) | 71 |
| 2 | Decision-ID coherence (D## out-of-sync master ↔ companions) | 25 |
| 3 | Component-name consistency (name in companion ≠ components.md) | 36 |
| 4 | Interface-name consistency (IXxx defined but not referenced, or vice-versa) | 68 |
| 5 | Diagram embedding | 0 |
| 6 | Markdown link integrity | 0 |
| 7 | Open-marker scan (unresolved [TBD] in HLD/SRS files) | 19 |
| 8 | Use-case-ID cross-reference | 0 |
| **TOTAL** | | **219** |

**Grand total across all 9 reports: 383 defects** (164 audit + 219 mechanical).  
**Gate verdict: DO NOT ADVANCE TO LLD.** Every audit report independently recommends holding the gate.

---

## 2. Top 10 Blockers (verbatim)

| # | File | ID | Description |
|---|------|----|-------------|
| 1 | `audits/01-srs.md` | SRS-016 | `[REQ-MB-060]` "retry a failed Modbus request up to 3 times" (1 original + 3 retries = 4 total transmissions) contradicts `[REQ-NF-103]` "declare Modbus communication failure after 3 consecutive unanswered requests" (failure after 3 total). The retry count is irreconcilable as written. |
| 2 | `audits/03-state-machines.md` | SM-005 | **Applying state — three-way NVIC_SystemReset() conflict; flag set twice.** The do activity says "trigger reboot," meaning it internally calls `NVIC_SystemReset()` and the MCU resets — the exit action therefore never executes. The exit action also says "`NVIC_SystemReset()` — the transition action *is* the reboot," which contradicts the do-activity. T7's action independently calls `set_pending_self_check(); persist(); NVIC_SystemReset()` — a third site for the same reset. `pending_self_check` is set in both the entry action *and* T7's action. It is impossible to determine which of the three sites owns the reboot or when the flag is set relative to the persist(). |
| 3 | `audits/03-state-machines.md` | SM-009 | **Transmitting entry action overwrites the command frame on the T3 path — functional error.** When T3 fires, the UML execution order is: T3 action (correct command frame built) → Transmitting entry action (`build_request_frame()` overwrites with a poll frame) → `enable TX driver; start UART transmit` (wrong frame transmitted). The system would transmit a polling request in place of every out-of-band command. |
| 4 | `audits/04-sequence-diagrams.md` | SD-B02 | SD-00b omits the Init.SelfChecking sub-state and its Faulted rainy path. `state-machines.md` Machine 1 Init sub-states are: CheckingIntegrity → LoadingConfig → BringingUpSensors → StartingMiddleware → SelfChecking → Operational. T7 (StartingMiddleware → SelfChecking) and T9 (SelfChecking → Faulted on !self_check_ok) are entirely absent from SD-00b. §1 commits: "every flow is specified for both sunny and rainy outcomes." A failure of the boot-time self-check silently drops out of scope. |
| 5 | `audits/04-sequence-diagrams.md` | SD-B04 | SD-06a message 4: CloudPublisher → UpdateService: sync: start update — UpdateService is absent from CloudPublisher's USES list in components.md. The same pattern recurs in SD-07 message 4 (CloudPublisher → ConfigService) and SD-08 message 4 (CloudPublisher → LifecycleController) — none of these three targets appear in CloudPublisher's USES. This is a systematic cross-document contradiction affecting the command-routing architecture. |
| 6 | `audits/07-flash-partition.md` | FPL-03 | §6.2 ring-size justification is arithmetically wrong by a factor of 111×. The document states: "At a representative log rate of one record per second averaging 64 bytes, the 1 MB ring offers approximately three weeks of history before wrap." Verified: 1 048 576 B ÷ 64 B/s = 16 384 s ≈ 4.55 hours, not three weeks (1 814 400 s). A 110.7 MB ring would be needed for three weeks at the stated rate. This is the sole ring-size justification; it is numerically unsound and untestable as written. |
| 7 | `audits/07-flash-partition.md` | FPL-02 | Gateway QSPI has no X.509 certificate partition, violating CON-006. CON-006: "X.509 client certificates and private keys shall be stored in a dedicated flash partition, separate from configuration and telemetry buffer storage." The QSPI layout contains ConfigStore (configuration), CircularFlashLog (telemetry/log buffer), OTA staging, and Reserved — but no certificate partition. |
| 8 | `audits/07-flash-partition.md` | FPL-04 | §8 claims StoreAndForward is "Handled in RAM" — this is factually wrong and contradicts two reference documents. `components.md` explicitly lists StoreAndForward USES (downward): CircularFlashLog, IHealthReport, ILogger — CircularFlashLog is QSPI-flash-backed. SRS REQ-BF-000 mandates "non-volatile storage" for outbound cloud message buffering. RAM is volatile; an in-RAM buffer would lose all buffered messages on power-cycle, violating REQ-BF-000. |
| 9 | `audits/05-task-breakdown.md` | TB-09 | **`SPI_wifi_IRQHandler` owning task is undefined.** §6.1 lists: `GW \| SPI_wifi_IRQHandler \| WiFi data ready \| CloudPublisherTask *(or owning caller)* \| «notify»`. Direct-to-task notification (`xTaskNotifyFromISR`) requires a **fixed task handle** stored at compile time or written once at init. Three tasks share the WiFi resource under `wifi_mutex`: `CloudPublisherTask`, `TimeServiceTask`, `UpdateServiceTask`. "Or owning caller" is not an implementable ISR contract. |
| 10 | `audits/05-task-breakdown.md` | TB-11 | **Retry-storm analysis violates REQ-NF-101.** §8.2 states `ModbusPollerTask` WCET up to 600 ms (timeout + 3 retries) at priority 4. `SensorTask` runs at priority 3 with a 100 ms period. During a 600 ms retry storm, `ModbusPollerTask` (p4) preempts `SensorTask` (p3) for the full 600 ms, causing five to six consecutive missed polling cycles. REQ-NF-101 requires alarm detection *within one polling cycle*. The Gateway's own sensor alarms cannot be detected during this window. "Only briefly" directly contradicts the document's own 600 ms figure. |

---

## 3. Cross-Cutting Defects (present in more than one audit)

| Issue | Defect IDs | Artefacts |
|-------|-----------|-----------|
| **P1 layering violation — ModbusSlave calls ModbusRegisterMap without a DIP interface** | SM-016, SD-B03 | state-machines.md, sequence-diagrams.md |
| **EditingConfig entry/exit actions duplicate T10/T11/T12 transition actions (double invocation)** | SM-003, SM-015 | state-machines.md (Machine 1 GW, Machine 5 FD — symmetric on both boards) |
| **IConfigProvider absent from LcdUi and ConsoleService USES lists on both boards** | components-1, components-8, components-9 | components.md (FD and GW sections) |
| **P6 trace gaps — driver and middleware components carry no REQ citation** | components-5, components-6, components-12, components-14, RM-07, FPL-09 | components.md, modbus-register-map.md, flash-partition-layout.md |
| **Decision log orphans — D## labels missing from master log or companion docs** | hld-21, hld-22, mechanical Check 2 (25 defects) | hld.md, mechanical-report.md |
| **REQ-NF-213 boot-time [TBD] propagated unresolved into HLD artefacts** | SRS-032, mechanical Check 7 (state-machines.md:119, :686; sequence-diagrams.md:223, :273) | SRS.md, state-machines.md, sequence-diagrams.md |
| **TimeService routes directly to ModbusMaster, bypassing ModbusPoller** | SD-B05, SM I7 (companion note) | sequence-diagrams.md, state-machines.md |
| **ModbusRegisterMap update path claimed by three separate locations across two documents** | SM-016, SM-017, SD-B03 | state-machines.md (Machine 5 I3, Machine 6 I1), sequence-diagrams.md (SD-02 msgs 6/8) |
