# Project Status — IoT Environmental Monitoring Gateway
 
**Last updated:** 23 May 2026
 
---
 
## Current Phase
 
V-Model Phase 4 (Implementation) — **bootstrap complete; first driver pending.**
 
The LLD baseline is established and tagged `lld-v1.0` on `main`. All three layers of the LLD gate review (Layer 1 mechanical, Layer 2 Pass A consumer-provider contracts, Layer 2 Pass B principle conformance, Layer 3 synthesis) have completed and blockers were resolved.
 
Phase 4 infrastructure is live: both STM32CubeIDE projects scaffolded (empty `main` builds clean on both boards), Ceedling host-test harness in place, CI pipeline green on all six checks, branch protection on `main` tightened to require all checks plus an up-to-date branch.
 
**Next: code the first module (`GpioDriver`) per the per-module workflow proven on the scaffold.**
 
---
 
## Completed since last update
 
- **LLD gate review Layers 1–3 closed.** Layer 1 (mechanical, 322 → 0) merged. Layer 2 Pass A (consumer-provider contracts) and Pass B (P1–P10 principle conformance) executed via Claude Code agent loops; findings triaged and applied. Layer 3 (synthesis) consolidated all gate-review findings and per-companion §8 items into a single resolution log.
- **`lld-v1.0` tagged on `main`.** Phase 3 closed.
- **Phase 4 bootstrap merged (PR #20).** Folder layout for `firmware/{field-device,gateway}/{drivers,middleware,application}/`; coding standard document; GitHub Actions workflow; Ceedling host-test harness; markdownlint config (heavily relaxed — only genuine-bug rules kept). Merged with red CI at the time; branch protection has since been tightened.
- **CubeIDE projects scaffolded for both boards (PR #23).** Created from CubeIDE's "Empty" template (no CubeMX, no `.ioc`); pure CMSIS, no STM32 HAL at any layer; `.cproject` and `.project` committed. Each project's `int main(void) { while(1){} }` builds clean. Also folded into PR #23: CI workflow corrections (action version pin, Ceedling pin, markdownlint expansion, duplicate-install-step removal, example-test restoration, path-validator fixes).
- **CI pipeline green on six checks.** Host unit tests (Ceedling on Ruby 3.0); firmware build × 2 (F469 + L475 via `xanderhendriks/action-build-stm32cubeide@v15.0`); `cppcheck` (scoped to `firmware/*/{drivers,middleware,application}/` only — vendor code excluded); `clang-format` check (same scoping); markdown lint.
- **Branch protection on `main` tightened.** All six CI checks required; branch must be up to date before merge. Eliminates the path that allowed PR #20 to merge red.
- **Repo cleanup.** `project_status.md` removed from repo root (it is Claude project-knowledge content, not git content). Stale `.gitkeep` files removed from folders that now contain real files.
- **Scope decision recorded in README.** Layers 1–5 non-negotiable. Layer 6 (OTA) and Layer 7 (CI extras) classified "good to have" — cut last if behind schedule.
---
 
## Completed Deliverables (merged to main)
 
| Deliverable | Branch | Status |
|---|---|---|
| Repository scaffold + README + .gitignore | main | Done |
| UML colour palette (`docs/diagram-colour-palette.md`) | main | Done |
| Vision document v1.5 (`docs/vision.md`) | main | Done |
| BOM + hardware setup guide (`docs/bom.md`, `docs/hardware-setup-guide.md`) | main | Done |
| Use case elicitation working document | feature/use-case-model | Merged via PR #1 |
| System Use Case Diagram (20 use cases, 3 actors, `.vpp` + PNG) | feature/use-case-model | Merged via PR #1 |
| Use case descriptions — all 20 (`docs/use-case-descriptions.md`) | feature/use-case-descriptions | Merged via PR #2 |
| SRS v1.1 (`docs/SRS.md`) — 190 requirements, 11 constraints, 7 assumptions | feature/srs + fixes | Merged via PR #3 + subsequent fixes |
| VP package restructure | feature/hld | Merged via PR #4 |
| System Deployment Diagram — Physical Topology | feature/hld | Merged via PR #4 |
| Domain Model diagram + companion document | feature/hld | Merged via PR #4 |
| Component diagrams: 5 Field Device views, 6 Gateway views (`.vpp` + 11 PNGs) | feature/hld-components | Merged via PR #6 |
| Component specification (`docs/hld/components.md`) — 41 unique / 59 total components, ISP/DIP/Metric Producer Pattern | feature/hld-components + fixes | Merged via PR #6 + subsequent fixes |
| Master HLD document v1.0 | multiple branches | Merged — tagged `hld-v1.0` |
| HLD #4 — State machine diagrams (6) + `state-machines.md` + D1–D12 | feature/hld-state-machines | Merged via PR #7 |
| HLD #5 — Sequence diagrams (18) + `sequence-diagrams.md` + D13–D20 | feature/hld-sequence-diagrams | Merged via PR #8 |
| `DeviceProfileRegistry` component added to Gateway Application layer + consumer wiring | fix/add-device-profile-registry | Merged via PR #9 |
| SRS requirements REQ-MB-110/-111/-120/-130, REQ-DM-100/-101 (device profile registry) | fix/add-device-profile-srs-requirements | Merged via PR #10 |
| HLD #6 — Task breakdown companion document + 2 task interaction diagrams + D21–D28 | feature/hld-task-breakdown | Merged via PR #11 |
| HLD #7 — Modbus register map companion document + D29–D34 | feature/hld-register-map | Merged via PR #12 |
| HLD #8 — Flash partition layout companion document + 2 flash layout diagrams + D35–D41 | feature/hld-flash-partition-layout | Merged |
| Gate review artefacts (`docs/hld/gate-review/`, `scripts/gate_review_check.py`) | feature/hld-gate-review | Merged |
| HLD gate review — all 38 blockers resolved across 8 companion documents | fix/hld-gate-review-remediation | Merged via PR #14 |
| LLD master scaffold + methodology v1.1 + all driver/middleware/application companions; HLD adjustments (ExtiDriver, SpiDriver frame size) | feature/lld-phase-opening | Merged |
| LLD gate review Layer 1 (mechanical, 322 → 0) + `scripts/lld_gate_review_check.py` + `docs/lld/gate-review/layer-1-report.md` | feature/lld-gate-review-layer-1 | Merged |
| LLD gate review Layer 2 Pass A (consumer-provider contracts) — script, report, and remediation | feature/lld-gate-review-layer-2-pass-a | Merged |
| LLD gate review Layer 2 Pass B (P1–P10 principle conformance) — script, report, and remediation | feature/lld-gate-review-layer-2-pass-b | Merged |
| LLD gate review Layer 3 (synthesis log + deferral document) | feature/lld-gate-review-layer-3 | Merged |
| `lld-v1.0` tag on `main` | — | Tagged |
| Phase 4 bootstrap — folder layout, coding standard, GitHub Actions workflow, Ceedling harness, markdownlint config | feature/phase-4-bootstrap | Merged via PR #20 |
| CubeIDE projects (F469 + L475) scaffolded; CI corrections (action pin, Ceedling pin, markdownlint expansion, path-validator fixes) | feature/phase-4-cubeide-projects | Merged via PR #23 |
| Branch-protection rules tightened on `main` (all 6 checks required + branch up-to-date) | — | Applied |
 
---
 
## Decisions made — HLD phase
 
*(D1–D41 settled during Phase 2; preserved here for reference. See `hld.md` §14 for full text.)*
 
| Decision | Rationale | Settled in |
|---|---|---|
| **D13** — Periodic NTP re-sync triggered by `CloudPublisher` via async event to `TimeService` | Connectivity state owned by `CloudPublisher`; routes the trigger so `TimeService` stays decoupled from the timing source | SD-09 derivation |
| **D14** — Per-slave probe with profile-bound device-ID validation; fall-through to Running on failure | Deny-by-default; supports 5 s boot budget (REQ-NF-203) | SD-00b derivation |
| **D15** — SD-06d self-check probes serial, not parallel | Three probes complete in ~500 ms; well within 10 s rollback budget (REQ-NF-204); coordination primitives unjustified | SD-06d derivation |
| **D16** — Bootloader modelled as `«bootloader»` boundary actor | Analogous to `«cloud»` for AWS IoT Core; internal logic out of HLD scope | SD-06a–d derivation |
| **D17** — Per-slave link state with profile binding | Polling allowlist is a derived view over `DeviceProfileRegistry` (REQ-MB-100) | SD-02 / SD-00b |
| **D18** — `DeviceProfileRegistry` as first-class Application component (Gateway only) | Industry EDS/GSD pattern; decouples register-map knowledge from firmware | SD-02 architectural feedback |
| **D19** — Pull-based downstream consumption represented via UML notes only | Consumers read on their own schedules per P7; redrawing each clutters the artefact | Sequence-diagram conventions |
| **D20** — Event-driven dispatch consistent with pull-based access | Events trigger access; data flows via pull. The two patterns are complementary | Sequence-diagram conventions |
| **D21** — `AlarmService` runs in `SensorTask` (no separate task) | Observer subscriber in producer context; extra queue + context switch unjustified by REQ-NF-101 | Task breakdown §4.3 |
| **D22** — `TimeService` and `CloudPublisher` in separate tasks despite shared WiFi | Different activation patterns (hourly vs continuous); P5 honoured; WiFi protected by `wifi_mutex` | Task breakdown §5.3 |
| **D23** — `UpdateServiceTask` at priority 1 despite OTA criticality | OTA rare and seconds-tolerant; criticality is correctness (signature, rollback), not scheduling priority | Task breakdown §5.3 |
| **D24** — `LifecycleTask` as dedicated task | Cross-task lifecycle events processed in one context; eliminates need for external mutex on lifecycle state machine | Task breakdown §4.2 / §5.2 |
| **D25** — Direct-to-task notification preferred for 1:1 single-event paths | Lighter than queue (no kernel object beyond TCB notification value) | Task breakdown §3 Step 8 |
| **D26** — Priority inheritance enabled on all mutexes | Prevents unbounded priority inversion; FreeRTOS-standard practice | Task breakdown §7 |
| **D27** — ISRs perform only acknowledge / capture / notify | Bounded interrupt latency; all driver state machines in task context | Task breakdown §6 |
| **D28** — Stack sizes estimated; refined via `uxTaskGetStackHighWaterMark()` | Conservative initial values minimise risk; runtime measurement provides real numbers | Task breakdown §3 Step 7 |
| **D29** — Scaled integers over IEEE-754 floats for physical quantities | Predictable decoders on any master; no NaN/Inf; half the bandwidth of float32 | Register map §5.3 |
| **D30** — Big-endian word order ("ABCD") for multi-register values | Modbus de-facto standard; aligns with most master tools' default | Register map §5.1 |
| **D31** — Single access pattern: registers only, no coils/discrete inputs | All bit-level data packed into `bitfield16` registers; keeps the slave to one access pattern | Register map §3 |
| **D32** — Magic value `0xA5A5` on destructive commands | `CMD_SOFT_RESTART` requires the magic; wrong value returns exception 0x03; prevents accidental triggers | Register map §6.5 |
| **D33** — `MAP_VERSION` at register 0x0000 as primary compatibility signal | Read during link establishment (SD-00b); enables `DeviceProfileRegistry` to bind correct profile per slave (D14, D17, D18) | Register map §8 |
| **D34** — Reserved sentinel values for unavailable readings | `0x8000` for `int16`, `0xFFFF` for `uint16`, `0xFFFFFFFF` for `uint32`; master tests for sentinel before using the value | Register map §5.4 |
| **D35** — Field Device has no OTA; single-bank firmware | OTA is Gateway-only per project narrative; FD firmware updated via SWD | Flash layout §4 |
| **D36** — Custom secondary bootloader on Gateway (16 KB) | STM32 ROM bootloader not used at runtime; required for OTA, dual-bank, and rollback logic | Flash layout §7 |
| **D37** — Both Gateway firmware banks on-chip (480 KB each) | Instant swap (no QSPI-to-on-chip copy on boot); both banks XIP; simpler bootloader. Caps firmware at 480 KB, well above expected footprint | Flash layout §5.1 |
| **D38** — Metadata partition uses A/B sector rotation for frequently updated fields | Spreads wear on the most write-hot fields; protects against power loss during metadata update | Flash layout §6.4 |
| **D39** — `ConfigStore` uses A/B sector rotation across two 32 KB slots | Doubles effective endurance per logical config; power-loss-safe via CRC-protected sequence numbers | Flash layout §6.1 |
| **D40** — `CircularFlashLog` is a sector-wrap ring buffer with persistent head pointer | Continuous logging without endurance concern; head pointer in dedicated A/B-rotated sector | Flash layout §6.2 |
| **D41** — OTA staging region (4 MB QSPI) retained | Enables resumable downloads and full-image signature verification before any on-chip write; prevents partial-write corruption of Bank B | Flash layout §5.2 |
 
---
 
## Decisions made — LLD phase
 
| Decision | Rationale | Settled in |
|---|---|---|
| **LLD-D1** — Bottom-up dependency-ordered companion drafting; GPIO driver first | Top-down would restate the HLD; just-in-time breaks V-Model gate discipline; risk-driven starts on components whose dependencies aren't specified yet. Bottom-up honours P1 at design time and gives every completed companion concrete unblocking value downstream | `lld.md` §6 D1 |
| **LLD-D2** — Error enum naming convention is `<module>_err_t`, not `<module>_status_t` | Aligns with embedded idiom (ESP-IDF `esp_err_t` etc.); `status_t` ambiguous with status-flag concept | `lld.md` §3.2 |
| **LLD-D3** — Drivers do not depend on FreeRTOS; ISR-driven consumer notification uses function-pointer callbacks | `components.md` `USES (downward): CMSIS` is binding; driver-layer purity allows portability across RTOSes; matches the pattern shared by every bus driver in components.md | `lld-methodology.md` v1.1 Step 2; `drivers/debug-uart-driver.md` §2.2 (canonical pattern) |
| **LLD-D4** — Caller serialises driver calls; drivers are not internally synchronised | Mutex/queue logic belongs in middleware where the concurrency lives. Internal driver mutex would duplicate consumer mutexes and require FreeRTOS dependency | `drivers/debug-uart-driver.md` §3.3 (pattern) |
| **LLD-D5** — Bus drivers (UART, I2C, SPI, QSPI, Modbus-UART) configure their own pins via direct CMSIS; do not consume `IGpio` | Follows the `components.md` convention: bus drivers `USES CMSIS` only; only LedDriver and device drivers with non-bus control lines (CS, DRDY, RESET) `USES GpioDriver` | `drivers/gpio-driver.md` §1.2; `drivers/debug-uart-driver.md` §3.4 |
| **LLD-D6** — Two-phase init pattern for components with split consumer lifecycles | When TX is needed pre-scheduler (Logger) but RX needs a consumer task to exist (ConsoleService), split `<module>_init()` (always-available) from `<module>_attach_<thing>(callback, context)` (post-task-create) | `drivers/debug-uart-driver.md` §2.3 |
| **LLD-D7** — `ExtiDriver` introduced as a separate Tier 1 driver companion | EXTI configuration is intentionally outside the GPIO driver; consumers needing interrupt-driven pin events (WiFi DRDY, touchscreen, Group B sensors) call both GPIO and EXTI drivers. The two drivers do not depend on each other | `drivers/gpio-driver.md` §8 GPIO-O2; `components.md` (HLD adjustment) |
| **LLD-D8** — LLD companion folder layout by layer: `docs/lld/{drivers,middleware,application,cross-cutting}/` | Companions split by layer prevent flat-folder mess; mirrors `firmware/` source layout; master and methodology stay at `docs/lld/` root for discoverability | `lld.md` §4 |
| **LLD-D9** — Layer 1 gate review = mechanical script + Claude Code agent loop | Mechanical fixes are tedious in chat (round-trip per edit); ideal for Claude Code (read, run, edit, re-run). Result: 322 → 0 defects in 8 commits. Pattern reusable for any subsequent mechanical sweep | LLD gate review Layer 1 |
 
---
 
## Decisions made — Phase 4 (Implementation)
 
| Decision | Rationale | Settled in |
|---|---|---|
| **P4-D1** — STM32CubeIDE 2.1.1 managed projects; `.cproject` / `.project` committed; no CMake | One build system, one toolchain configuration, no parallel CMake drift to maintain. Matches the IDE most STM32 jobs in Ireland use day-to-day | PR #23 |
| **P4-D2** — No CubeMX scaffolding; projects from CubeIDE "Empty" template | CubeMX generates HAL plumbing that conflicts with the no-HAL policy (P4-D3). Empty template guarantees the source tree contains only what we write | PR #23 |
| **P4-D3** — Pure CMSIS, no STM32 HAL at any layer | Confirms HLD/LLD policy: drivers go register-level via CMSIS headers; nothing above the driver layer ever sees a HAL type. Interview-defensible: demonstrates register-level competence | HLD `components.md`; LLD `lld.md` §3 |
| **P4-D4** — One workspace, two projects; workspace metadata outside the repo | `firmware/field-device/` and `firmware/gateway/` are independent CubeIDE projects sharing one workspace. Workspace `.metadata/` not committed — workspace is per-developer, projects are per-repo | PR #23 |
| **P4-D5** — Per-component folder layout, created incrementally | `firmware/<board>/{drivers,middleware,application}/<component-name>/`. Folders appear only when the first module lands; no pre-allocated empty trees | PR #20 |
| **P4-D6** — Ceedling 0.31.1 + Ruby 3.0 pinned for host unit tests | Ceedling 1.0.x has API breakage that makes the 1.x line non-viable. 0.31.1 is the last stable on the legacy API. CMock to be added when the first real driver needs hardware-boundary mocks | PR #20 + PR #23 |
| **P4-D7** — `clang-tidy` deferred until three drivers exist | Running `clang-tidy` against an empty tree produces noise without signal. Three drivers gives enough surface area for the warnings to be meaningful and to tune the config | PR #20 |
| **P4-D8** — Per-module workflow: "Pass H" before any code | Before coding any module, fill §7 (unit test plan with concrete TC IDs) of its LLD companion. Tests drive the implementation, not the reverse. The LLD §7 baseline is the eight-step methodology output; Pass H hardens it into TC-level test cases bound to the Ceedling harness | This session |
| **P4-D9** — Module order follows `boot-order-graph.md` topology; GpioDriver first | GpioDriver chosen as toolchain canary: small, no interrupts, exercises the full per-module workflow end-to-end (branch → Pass H → code → register source folder → tests → PR) | This session |
| **P4-D10** — Scope discipline: Layers 1–5 non-negotiable; Layers 6 and 7 cut first if behind schedule | Recorded in `README.md`. Layer 6 (OTA) and Layer 7 (CI extras) are good-to-have stretch. Quality of Layers 1–5 takes precedence over breadth | README |
| **P4-D11** — `cppcheck` and `clang-format` CI checks scoped to project source only | Path filter on `firmware/*/{drivers,middleware,application}/` excludes CMSIS vendor headers, which would otherwise flood the report. Vendor code is not ours to fix | PR #23 |
| **P4-D12** — Branch protection requires all six checks + branch up-to-date | Closes the hole that allowed PR #20 to merge red. The 6 checks: Ceedling host tests, F469 firmware build, L475 firmware build, `cppcheck`, `clang-format`, markdown lint | This session |
 
---
 
## New or resolved TBDs
 
**Resolved since last update:**
 
- **LLD gate review Layers 1–3** — all closed; `lld-v1.0` tagged.
- **Per-companion §8 items** — triaged in Layer 3 synthesis; either resolved in companion edits, escalated to HLD, or formally deferred with rationale.
- **Build system choice** — STM32CubeIDE managed projects (no CMake).
- **Project scaffolding source** — CubeIDE Empty template (no CubeMX).
- **Host test harness pin** — Ceedling 0.31.1 on Ruby 3.0.
- **Pass H workflow** — defined per P4-D8.
- **Layer 6/7 prioritisation** — formally classified "good to have"; cut last (P4-D10).
**Open / new:**
 
- **GpioDriver Pass H** — `docs/lld/drivers/gpio-driver.md` §7 to be hardened into concrete `TC-GPIO-NNN` cases before any `.c`/`.h` is written.
- **CMock integration** — first real driver (GpioDriver) will be the trigger to add CMock to the Ceedling harness.
- **`clang-tidy` config** — deferred until three drivers exist (P4-D7); revisit then.
- **PCLK values** — drivers reference assumed values (PCLK1 ≈ 45 MHz F469, PCLK2 ≈ 80 MHz L475); to be pinned in startup/clock-config code during Layer 1 implementation.
- **Worst-case stack measurements** (inherited) — integration-time; no change.
- **CircularFlashLog representative log rate** (inherited) — validated at Layer 5/integration.
- **26 numeric TBD markers in HLD documents** (boot time, NTP period, buffer capacity, polling rate) — resolved at integration-time measurement.
---
 
## What Comes Next
 
1. **Confirm `docs/lld/drivers/gpio-driver.md` §7 starting state.** Determines whether Pass H is fill-in-the-blanks or fresh draft.
2. **GpioDriver Pass H.** Promote §7 into concrete `TC-GPIO-NNN` cases ready to drive Ceedling tests. Companion edit on `feature/phase-4-gpio-driver`.
3. **GpioDriver implementation.** `firmware/field-device/drivers/gpio/gpio_driver.{c,h}` per LLD companion §2 (API) and §3 (internal design). Register the source folder in CubeIDE (Properties → Paths and Symbols).
4. **GpioDriver unit tests.** `tests/field-device/drivers/gpio/test_gpio_driver.c`. Update `tests/project.yml` with the new test path.
5. **PR with companion + code + tests.** Single review cycle.
6. **UartDriver.** Logger depends on it (per `boot-order-graph.md`).
7. **RtcDriver.** Logger uses it directly per the bootstrap exception in `architecture-principles.md`.
8. **Logger.** Lands after GpioDriver, UartDriver, RtcDriver.
9. **Continue Layer 1 topologically per `boot-order-graph.md`.**
---
 
## SRS Functional Area Progress
 
| Section | Area | Status | Notes |
|---|---|---|---|
| 2.1 | | 2.1 | Sensor Acquisition [SA] | Complete | 25 requirements (18 original + 7 for history buffer: REQ-SA-180..-240). Traces to UC-01, UC-07, UC-14. |
| 2.2 | Alarm Management [AM] | Complete | 6 requirements. Traces to UC-08, UC-09, UC-03. |
| 2.3 | | 2.3 | Local Display — LCD [LD] | Complete | 22 requirements (12 original + 5 for boot splash + 5 for trend screen: REQ-LD-160..-195). Traces to UC-01, UC-02, UC-03, UC-15. |
| 2.4 | Local Interface — CLI [LI] | Complete | 16 requirements. Traces to UC-04, UC-16. |
| 2.5 | Modbus Communication [MB] | Complete | 15 requirements (11 original + 4 for device profile registry: REQ-MB-110/-111/-120/-130). Traces to UC-07, UC-10, UC-13, UC-14, UC-16, UC-19. |
| 2.6 | Cloud Communication [CC] | Complete | 10 requirements. Traces to UC-05, UC-06, UC-09, UC-10, UC-11, UC-12. |
| 2.7 | Time Synchronisation [TS] | Complete | 6 requirements. Traces to UC-13. |
| 2.8 | Device Management [DM] | Complete | 24 requirements (22 original + 2 for profile updates: REQ-DM-100/-101). Traces to UC-15, UC-16, UC-17, UC-18, UC-20. |
| 2.9 | Data Buffering [BF] | Complete | 3 requirements. Traces to UC-10, UC-11, UC-12 exception flows. |
| 3.1 | Performance | Complete | 15 requirements. |
| 3.2 | Reliability | Complete | 16 requirements. |
| 3.3 | Security | Complete | 8 requirements. |
| 3.4 | Memory and Resource Constraints | Complete | 9 requirements. |
| 3.5 | Maintainability | Complete | 7 requirements. |
| 4 | Constraints | Complete | 11 constraints (CON-001 to CON-011). |
| 5 | Assumptions | Complete | 7 assumptions (ASM-001 to ASM-007). |
| 6 | Traceability Matrix | Complete | 190 rows, all requirements mapped. |
 
**Total: 202 requirements, 11 constraints, 7 assumptions.**
 
---
 
## HLD Artefact Progress
 
| # | Artefact | Status |
|---|----------|--------|
| 1 | System Deployment Diagram (Physical Topology) | Complete — merged via PR #4 |
| 2 | Domain Model (diagram + companion doc) | Complete — merged via PR #4 |
| 3 | Component Diagrams + components.md + master hld.md v0.1 | Complete — gate review blockers resolved (PR #14) |
| 4 | State Machine Diagrams + state-machines.md + master hld.md v0.2 | Complete — gate review blockers resolved (PR #14) |
| 5 | Sequence Diagrams (18) + sequence-diagrams.md + master hld.md v0.3 | Complete — gate review blockers resolved (PR #14) |
| 6 | Task Breakdown + task-breakdown.md + master hld.md v0.4 | Complete — gate review blockers resolved (PR #14) |
| 7 | Modbus Register Map + modbus-register-map.md + master hld.md v0.5 | Complete — gate review blockers resolved (PR #14) |
| 8 | Flash Partition Layout + flash-partition-layout.md + master hld.md v0.6 | Complete — gate review blockers resolved (PR #14) |
 
**Phase 2 gate: PASSED. HLD v1.0 baseline established. Tagged `hld-v1.0`.**
 
---
 
## LLD Artefact Progress
 
| Tier | Layer | Status |
|---|---|---|
| — | Master `lld.md` (v0.1) and `lld-methodology.md` (v1.1) | Complete — merged |
| 1 | Drivers tier 1 (GpioDriver, DebugUartDriver, RtcDriver, ResetDriver, ExtiDriver) | Complete — gate clean |
| 2 | Drivers tier 2 (I2cDriver, SpiDriver, ModbusUartDriver, QspiFlashDriver, SdramDriver) | Complete — gate clean |
| 3 | Drivers tier 3 (sensors, TouchscreenDriver, LcdDriver, WifiDriver, LedDriver) | Complete — gate clean |
| — | Middleware (Logger, TimeProvider, ModbusSlave, ModbusMaster, ModbusPoller, ConfigStore, NtpClient, MqttClient, CircularFlashLog, FirmwareStore, GraphicsLibrary) | Complete — gate clean |
| — | Application (LifecycleController, SensorService, AlarmService, HealthMonitor, ModbusRegisterMap, LcdUi, ConsoleService, CloudPublisher, StoreAndForward, TimeService, DeviceProfileRegistry, UpdateService) | Complete — gate clean |
| — | Gate review Layer 1 (mechanical, 322 → 0) | Complete — merged |
| — | Gate review Layer 2 Pass A (consumer-provider contracts) | Complete — merged |
| — | Gate review Layer 2 Pass B (P1–P10 principle conformance) | Complete — merged |
| — | Gate review Layer 3 (synthesis) | Complete — merged |
 
**Phase 3 gate: PASSED. LLD v1.0 baseline established. Tagged `lld-v1.0`.**
 
---
 
## Phase 4 (Implementation) Progress
 
| Item | Status |
|---|---|
| Repo folder layout for firmware/tests | Complete — PR #20 |
| Coding standard document | Complete — PR #20 |
| GitHub Actions CI workflow | Complete — PR #20 (CI fixes in PR #23) |
| Ceedling host-test harness (0.31.1 / Ruby 3.0) | Complete — PR #20 |
| markdownlint config (relaxed) | Complete — PR #20 |
| CubeIDE project — Field Device (F469) | Complete — PR #23 (empty `main` builds clean) |
| CubeIDE project — Gateway (L475) | Complete — PR #23 (empty `main` builds clean) |
| Branch protection on `main` (6 checks + up-to-date) | Complete |
| CMock added to Ceedling harness | Pending — at first real driver |
| `clang-tidy` integration | Deferred — until 3 drivers exist (P4-D7) |
| **Layer 1 — Sensor Acquisition** | Pending — GpioDriver up first |
| Layer 2 — Data Processing & State Machine | Pending |
| Layer 3 — Modbus RTU | Pending |
| Layer 4 — LCD Display | Pending |
| Layer 5 — WiFi/MQTT Cloud Connectivity | Pending |
| Layer 6 — Custom Bootloader | Good-to-have (cut first if behind) |
| Layer 7 — CI/CD extras | Good-to-have (cut first if behind) |
 
---
 
## Per-module workflow (Phase 4)
 
Proven on the scaffold; repeat for every subsequent module.
 
1. Branch `feature/phase-4-<module>` off fresh `main`.
2. **Pass H first.** Fill §7 (unit test plan, concrete TC IDs) in the relevant LLD companion before any code.
3. Implementation in `firmware/<board>/<layer>/<component>/<module>.{c,h}` per LLD companion §2 + §3.
4. Register the source folder in CubeIDE (Properties → Paths and Symbols).
5. Unit tests in `tests/<board>/<layer>/<component>/test_<module>.c`. Update `tests/project.yml` adding the new test path.
6. Local build + local Ceedling both pass.
7. PR with companion edit + code + tests. Single review cycle.
---
 
## CI pipeline (6 required checks)
 
| Check | Tool | Scope |
|---|---|---|
| Host unit tests | Ceedling 0.31.1 / Ruby 3.0 / Unity (CMock pending) | `tests/**` |
| Firmware build — Field Device | `xanderhendriks/action-build-stm32cubeide@v15.0` | `firmware/field-device/` |
| Firmware build — Gateway | `xanderhendriks/action-build-stm32cubeide@v15.0` | `firmware/gateway/` |
| `cppcheck` | cppcheck | `firmware/*/{drivers,middleware,application}/` only |
| `clang-format` | clang-format | `firmware/*/{drivers,middleware,application}/` only |
| Markdown lint | markdownlint-cli | `docs/**`, `README.md`; relaxed ruleset |
 
All six required; branch must be up-to-date before merge.
 
---
 
## Open Questions / TBDs
 
*(Items inherited from HLD that remain open are preserved; LLD- and Phase 4-era items appended below.)*
 
- Store-and-forward buffer sizing — refined in `middleware/store-and-forward.md`; final validation at integration (CON-009 flash endurance, ASM-004 SRAM budget).
- Exact LCD screen flow/navigation — covered in `application/lcd-ui.md`; final pixel-level mockups deferred to Phase 4.
- CLI command set details — covered in `application/console-service.md`; final command surface validated at first integration milestone.
- Low-pass filter parameters for sensor conditioning — covered in `application/sensor-service.md` §8; values at integration.
- Sensor range boundary values — covered in `application/sensor-service.md`; final values at first integration milestone.
- Number of recent readings to store per sensor [REQ-SA-090] — covered in `application/sensor-service.md` §8; final figure at integration.
- Simulation module parameters (noise profile, fault injection config) — covered in sensor driver companions; final values at integration.
- RTC synchronisation interval [REQ-NF-210, REQ-NF-211] — covered in `application/time-service.md`; determined by RTC drift measurement during integration.
- Polling frequency valid range [REQ-NF-114] — covered in `middleware/modbus-poller.md`; final range at integration.
- Boot-to-operational time [REQ-NF-213] — measured during integration; cold-boot vs warm-restart distinction documented in `state-machines.md`.
- NTP delta threshold for sanity-check rejection — covered in `middleware/ntp-client.md`; final value at integration.
- Logger output sink configuration — covered in `middleware/logger.md`.
- Per-channel alarm state machine (Clear ↔ Active with hysteresis) — covered in `application/alarm-service.md`.
- Self-check coverage list — covered in `application/health-monitor.md` and `application/update-service.md`.
- Worst-case stack measurements — initial estimates in task breakdown; refined via `uxTaskGetStackHighWaterMark()` at integration.
- CircularFlashLog representative log rate — 1 MB ring sizing validated during integration.
- 26 numeric TBD markers in HLD documents (boot time, NTP period, buffer capacity, polling rate) — resolved at integration-time measurement.
- **PCLK values for driver baud/timing calculations** — drivers assume PCLK1 ≈ 45 MHz (F469) and PCLK2 ≈ 80 MHz (L475); pin in Phase 4 startup code or in a `clock-config.md` cross-cutting companion.
- **Starting state of `docs/lld/drivers/gpio-driver.md` §7** — placeholder, partially drafted, or fully drafted? Determines whether Pass H is fill-in-the-blanks or fresh draft.
- **CMock integration into Ceedling harness** — to land alongside the GpioDriver PR.
- **`clang-tidy` configuration** — to be drafted when the third driver lands (P4-D7).
---
 
## Recurring Patterns / Lessons Learned
 
*Earlier session lessons preserved; new patterns from the LLD and Phase 4 bootstrap appended.*
 
- **Documentation pattern for each HLD artefact** is uniform: companion document under `docs/hld/`, summary section in master `hld.md` referencing it, decisions appended to log, version bumped, artefact table marked complete. Typical PR contains 2–4 commits.
- **Forward-looking section as a shrinking checklist** is a visible signal of phase completion. Applied identically to the LLD master's catalogue.
- **Pre-PR fix list as a coordination mechanism.** Enumerating outstanding fixes before opening a PR catches issues that would otherwise surface during review.
- **`git rebase --onto` for clean branch separation** when commits land on the wrong branch.
- **Gate review mechanical checks are cheap and high-yield.** A single Python script over the repo surfaces ~70% of consistency defects before any expensive semantic reasoning. Idempotent; near-zero tokens per run.
- **Divide-et-impera audit parallelism.** Multiple parallel semantic audit chats (one per artefact, fresh context per chat, tight template) is faster and cheaper than one large serial review.
- **Mechanical check script false positives.** Free-text regex matches state names, task names, and external library names as "components". Use structural extraction (bold, heading, table-cell patterns) for trustworthy inventories.
- **Verify diagram content before assuming a defect is real.** Never assume a diagram is wrong without opening it.
- **Force-delete local merged branches with `git branch -D`** when `git branch -d` refuses due to merge-verification state.
- **Verify `.gitignore` effectiveness with `git status`** after every `.gitignore` change.
- **Drivers must not import FreeRTOS unless `components.md USES` says so.** The seam for ISR-driven consumer notification is a function-pointer callback. The consumer wires the callback to its RTOS primitive (typically `xTaskNotifyFromISR`); the driver knows nothing about tasks. Caught during DebugUartDriver review; codified in `lld-methodology.md` v1.1 Step 2.
- **`components.md USES (downward)` is the authoritative dependency footprint.** Widening it at the LLD layer requires HLD escalation — never silent. Methodology v1.1 Step 2 validation enforces this.
- **Bus drivers configure their own pins via direct CMSIS.** They do not consume `IGpio`. Discriminator: UART/I2C/SPI/QSPI drivers self-configure; LedDriver and device drivers with non-bus control lines (CS, DRDY, RESET) `USES GpioDriver`.
- **Two-phase init pattern.** When a driver has consumers with split lifecycles (one pre-scheduler, one post-task-create), split init from attach. Generalises beyond DebugUartDriver — applicable to any driver whose initialisation cost can be paid early but whose runtime requires a task to exist.
- **Chat is for design conversation; Claude Code is for agent loops.** Mechanical work that requires "run, see output, fix, re-run" (Layer 1 gate review) belongs in Claude Code. Architectural decisions, design reviews, and anything that needs pushback belong in chat where reasoning stays visible. The Layer 1 split saved hours of round-trip editing.
- **Board UMs in `/mnt/project/*.pdf` are zip archives of JPEG pages, not real PDFs.** Extract with `unzip <um>.pdf -d /tmp/<dir>` then view individual `.jpeg` files. The next session/Claude won't know this without being told.
- **HLD baseline can be revised during LLD drafting.** ExtiDriver was added to `components.md` mid-LLD because the GpioDriver §8 surfaced the EXTI-vs-GPIO split. The change went through proper HLD revision channels (commit message, file edit, consumer wiring updates), not silent edits. Pattern: if LLD work surfaces a real architectural gap, fix the HLD first, then the LLD inherits a clean source.
- **Branch protection must be tightened *before* the first PR that depends on it.** PR #20 merged red because protection rules at that moment didn't require all checks. Fix: tighten protection as part of the scaffold PR itself, or as the first follow-up. Verified: PR #23 was forced green by the corrected rules.
- **CI-scope path filters matter more than they look.** `cppcheck` and `clang-format` against the whole tree flood the report with vendor-header noise that drowns real findings. Scope to project source paths only.
- **Tool version pins are a Phase-4 deliverable, not an aside.** Ceedling 0.31.1 + Ruby 3.0 (because Ceedling 1.x has API breakage) and `xanderhendriks/action-build-stm32cubeide@v15.0` (specific version) are recorded as decisions. Unpinned tools produce phantom CI failures that waste a debugging session.
- **CubeIDE Empty template over CubeMX-generated when no-HAL policy is in force.** CubeMX generates HAL plumbing that conflicts with pure-CMSIS choices. Empty template guarantees a clean source tree.
- **Per-component folder layout created incrementally.** Pre-allocating `firmware/<board>/<layer>/<module>/` for every planned module produces a tree of `.gitkeep` files that confuses readers. Create folders only when the first real file lands.
- **`.gitkeep` files become litter after their folder fills.** Sweep them when the folder gains real content. Caught during Phase 4 cleanup.
- **`project_status.md` belongs in Claude project knowledge, not git.** It is cross-session sync content, not project deliverable. Removing it from the repo eliminates confusion about which copy is authoritative.
---
 
## Git Conventions
 
- Commit prefixes: `docs:`, `design:`, `chore:`, `feat:`, `fix:`, `test:`. **No parenthesised scope** (`docs(hld):` is forbidden; use `docs:`).
- Feature branches merged via PR to main.
- **Create the feature branch as command zero** of any new artefact, before any work begins.
- Interactive rebase (`git rebase -i` with `--force-with-lease`) for clean history.
- Multi-paragraph commit messages written via editor (`git commit` with no `-m` flag).
- After amending, always run `git show HEAD --stat` to verify before pushing.
- Use `git push --force-with-lease` (never plain `--force`) after amends.
- PR merges preserve commit history via merge commits (not squash) when intermediate commits each represent a meaningful step.
- Binary file extraction from git in PowerShell: `cmd /c "git show <ref>:<path> > <output>"` (never PowerShell's `>` redirect for binary files).
- Cherry-pick acceptable for small post-merge fixes where opening another PR would be ceremonial overhead.
- `git switch -c <branch>` preferred over `git checkout -b <branch>`.
- **`git rebase --onto <target> <upstream> <branch>`** to extract commits from a wrong branch onto a correct one without loss.
- **Branch cleanup after merge:** delete both local (`git branch -D`) and remote (`git push origin --delete`) immediately after PR merge to keep branch list clean.
- **Layer-grouped commits for mechanical sweeps.** When a tool reports many findings, commit fixes in batches grouped by category (one commit per fix-type) rather than one giant commit. Improves bisectability and makes review more tractable. Pattern proven by the Layer 1 gate review's 8 commits.
- **Consolidate stale feature-branch labels with `git branch -f`** when multiple branches point at different positions along the same linear history (common when work continues past the original branch's stopping point). Verify linear history first via `git log --oneline --graph --all` — diagonal markers `\` or `/` indicate genuine divergence and require `git rebase --onto` instead.
 
