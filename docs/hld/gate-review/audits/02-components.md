# Audit — docs/hld/components.md
Auditor: Claude (gate-review chat 3)
Severity counts: 9B / 11F / 1D / 1C

## §Preamble (Architectural patterns) [2 defects]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| components-1 | F | ISP preamble lists `LcdUi, ConsoleService, ModbusRegisterMap` as `IConfigManager` consumers (line 26) but omits `LcdUi` and `ConsoleService` from `IConfigProvider` consumers (line 26). Both need read access for displaying current parameters (REQ-LD-100 config screen; ConsoleService responsibility says "exposing… configuration… data through CLI"). | Add `LcdUi, ConsoleService` to the `IConfigProvider` consumer list. |
| components-2 | B | HealthMonitor responsibility says it `"drives the on-board LEDs to indicate device status (idle, acquiring, alarm, error)"` (preamble line 59; FD line 279; GW line 561). No SRS requirement mandates board-LED status indication (UC-02/06/UC-08 are LCD/cloud-only). P6 violation — speculative responsibility. `LedDriver` then hangs on an unrequired behaviour. | Either delete the LED-driving responsibility (and `LedDriver` if it has no other consumer) or open a new requirement against `SRS.md` and re-baseline before adding it back here. |

## §FD §1 (Bottom-up sweep): clean.

## §FD §2 (Top-down sweep) [2 defects]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| components-3 | F | Table titled `"use cases → application components"` (line 108) lists `ConfigStore` (line 118), which is classified as Middleware in FD §3 and §4. Category/heading mismatch. | Either move the `ConfigStore` row to a separate Middleware-tracing block or rename the column to `"use cases → application + persistence-tier components"`. |
| components-4 | F | LifecycleController row (line 112) cites `LD-200..-240, NF-202, NF-213, NF-214` but omits `NF-203` ("restart and resume within 5 s after a normal reset"). NF-203 applies on both boards and is cited on the GW counterpart (line 356). | Add `NF-203` to the FD LifecycleController trace. |

## §FD §3 (Final component list): clean.

## §FD §4 — Driver layer [1 defect]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| components-5 | F | 10 of 12 FD drivers (`DebugUartDriver, LcdDriver, TouchscreenDriver, SdramDriver, QspiFlashDriver, ModbusUartDriver, LedDriver, RtcDriver, I2cDriver, GpioDriver`) carry no UC/REQ citation in the responsibility sentence — only `BarometerDriver` and `HumidityTempDriver` cite Vision §5.1.1. P6 traceability check requires every component to cite at least one requirement or use case. | Add an inline REQ/UC cite to each driver's responsibility (e.g., `LcdDriver` → REQ-LD-010/REQ-NF-108; `QspiFlashDriver` → REQ-NF-405/REQ-DM-074). |

## §FD §4 — Middleware layer [1 defect]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| components-6 | F | `Logger`, `ConfigStore`, `ModbusSlave`, `GraphicsLibrary` responsibilities carry no UC/REQ citation in the responsibility sentence. Only `TimeProvider` cites Vision §8. P6 trace gap. | Add citations: `Logger` → REQ-NF-500; `ConfigStore` → REQ-DM-090/REQ-NF-214; `ModbusSlave` → REQ-MB-030..-070; `GraphicsLibrary` → REQ-LD-000/REQ-NF-108. |

## §FD §4 — Application layer [4 defects]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| components-7 | F | LifecycleController responsibility `"Coordinates the field device's top-level lifecycle"` (line 267) is too generic to support the LD-200..-240 (splash screen) and NF-202/-213/-214 (reset behaviour) coverage claimed in §2. Untestable as written — what specific behaviour does it coordinate, and where does the splash drawing live? | Expand the responsibility to name the splash-screen drawing duty (drives `GraphicsLibrary` during Init), the boot-time `ConfigStore` integrity check, and the watchdog/normal-reset recovery sequencing. |
| components-8 | B | `LcdUi` USES list (line 275) contains `IConfigManager` but not `IConfigProvider`. Responsibility plus REQ-LD-100 ("display a configuration screen which lists these user configurable parameters") require read access to current configuration — unreachable through `IConfigManager` (write-side). Component cannot fulfil its responsibility with the listed dependencies. | Add `IConfigProvider` to `LcdUi` USES (downward). |
| components-9 | B | `ConsoleService` USES list (line 299) contains `IConfigManager` but not `IConfigProvider`. Responsibility `"exposing sensor, configuration, and health data through CLI commands"` requires read access for the `show config` style commands. Same gap as components-8. | Add `IConfigProvider` to FD `ConsoleService` USES. |
| components-10 | B | `ModbusRegisterMap` responsibility says `"Polls IModbusSlaveStats and reports Modbus protocol metrics via IHealthReport"` (line 303), but USES (line 305) lists only `ModbusSlave` (concrete) — `IModbusSlaveStats` is not separately listed. P5 Metric Producer Pattern documentation: the stats consumption is invisible in the dependency list, contradicting the responsibility. | List `IModbusSlaveStats` explicitly alongside `ModbusSlave` in the USES list (or document a convention that concrete-component USES implies all its provided interfaces — but apply it uniformly). |

## §GW §1 (Bottom-up sweep): clean.

## §GW §2 (Top-down sweep) [1 defect]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| components-11 | F | Same as components-3: table titled `"use cases → application components"` (line 354) includes `ConfigStore` (line 364), which is Middleware in GW §3 and §4. | Same fix as components-3, applied to the GW table. |

## §GW §3 (Final component list): clean.

## §GW §4 — Driver layer [1 defect]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| components-12 | F | None of the 14 GW drivers cites a UC/REQ in its responsibility sentence. P6 trace gap; same defect class as components-5. | Add inline REQ/UC citations per driver (e.g., `WifiDriver` → REQ-CC-050/CON-001; `MagnetometerDriver`/`ImuDriver`/`BarometerDriver`/`HumidityTempDriver` → REQ-SA-031/-071; `ResetDriver` → REQ-DM-010/REQ-NF-202). |

## §GW §4 — Middleware layer [2 defects]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| components-13 | B | `FirmwareStore` responsibility states `"verifies image integrity and digital signature (REQ-DM-070)"` (line 547). REQ-DM-070 in SRS.md is `"set the firmware partition as boot partition"` — not signature verification. The intended requirements are REQ-DM-060 (image validation) and REQ-DM-080 (cryptographic signature). Cross-document traceability error. | Replace the `(REQ-DM-070)` cite with `(REQ-DM-060, REQ-DM-080)`; consider separating "verify integrity & signature" from "commit slot switch" since the latter is what DM-070 actually covers. |
| components-14 | F | `Logger`, `MqttClient`, `ModbusMaster`, `CircularFlashLog`, `NtpClient`, `ConfigStore`, `FirmwareStore` responsibilities carry no UC/REQ citation. P6 trace gap; same class as components-6. | Add citations per component (e.g., `MqttClient` → REQ-CC-050/-060; `NtpClient` → REQ-TS-010; `CircularFlashLog` → REQ-BF-000..-020). |

## §GW §4 — Application layer [7 defects]

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| components-15 | F | LifecycleController responsibility `"Coordinates the gateway's top-level lifecycle"` (line 555) is too generic to support UC-17, UC-18, NF-202/-203/-213/-214 trace. Same defect class as components-7. | Expand to name the restart orchestration (UC-17), firmware-update entry handoff to `UpdateService` (UC-18), boot-time `ConfigStore` integrity check, and watchdog/normal-reset recovery. |
| components-16 | D | LifecycleController USES list (line 557) contains 11 direct dependencies — crosses the P8 `"8+ direct dependencies"` trigger. The doc neither defends the fan-out nor proposes splitting. | Record only — review at LLD whether a startup-orchestrator/shutdown-orchestrator split is justified, or document that LifecycleController's fan-out is intentional and acceptable for a top-level state machine. |
| components-17 | B | GW `ConsoleService` USES (line 599) uses concrete `SensorService, ConfigService, HealthMonitor, Logger` instead of the ISP-split / cross-cutting interfaces (`ISensorService, IConfigProvider, IConfigManager, IHealthSnapshot, ILogger`). Directly contradicts the ISP preamble (`"IConfigManager… consumed by… ConsoleService"`, `"IHealthSnapshot… consumed by… ConsoleService"`) and the FD `ConsoleService` spec (line 299). P3 ISP violation. | Replace concrete names with the ISP-split interfaces, matching the FD `ConsoleService` USES list and adding `IConfigProvider` (see components-9 sibling defect). |
| components-18 | B | GW `ModbusPoller` USES (line 605) uses concrete `HealthMonitor, Logger` instead of `IHealthReport, ILogger`. Responsibility (line 603) explicitly says `"reports Modbus protocol metrics via IHealthReport"` — USES list directly contradicts the responsibility. P3 ISP and P5 Metric Producer Pattern violation. | Replace `HealthMonitor` with `IHealthReport` and `Logger` with `ILogger` in the USES list. |
| components-19 | B | GW `ModbusPoller` responsibility says `"Polls IModbusMasterStats"` but USES (line 605) omits `IModbusMasterStats`. Same defect class as components-10. P5 documentation gap. | Add `IModbusMasterStats` to the USES list alongside `ModbusMaster`. |
| components-20 | B | GW `CloudPublisher` responsibility says `"Polls IMqttStats and reports connectivity metrics via IHealthReport"` (line 579) but USES (line 581) omits `IMqttStats`. Same defect class as components-10 / -19. P5 documentation gap. | Add `IMqttStats` to the USES list alongside `MqttClient`. |
| components-21 | F | Cross-doc naming inconsistency in USES lists: `Logger` (concrete) appears on FD LifecycleController, GW LifecycleController, GW ConsoleService, GW ModbusPoller, GW DeviceProfileRegistry; everywhere else the convention is `ILogger`. Similarly, MW components are sometimes referenced concretely (`TimeProvider, MqttClient, NtpClient, StoreAndForward, FirmwareStore`) and sometimes only their `IXxx` interfaces are visible. The doc never states when concrete vs interface form is required. P10 naming convention not applied uniformly. | Either (a) adopt the rule "USES lists always reference `IXxx` interfaces, never concrete component names" and update every entry, or (b) state the rule explicitly in the preamble and apply it consistently across both boards. |
| components-22 | B | DeviceProfileRegistry exists in components.md (PR #9) and is referenced in all companion docs, but is absent from every Gateway component diagram PNG. Diagram is the authoritative source per V-Model; this is a traceability gap (P6, P9). | Add DeviceProfileRegistry to Gateway Application layer in VP, wire IDeviceProfileRegistry upward, regenerate PNGs. |

## §GW §4 — Application layer (cosmetic)

| ID | Severity | Description | Suggested fix |
|----|----------|-------------|---------------|
| components-22 | C | `UpdateService` responsibility (line 591) ends `"reboot via ResetDriver"` — no terminating full stop. Every other responsibility sentence ends with a period. | Add the trailing period. |

## Summary

The artefact is structurally sound — both boards have layered component lists, ISP/DIP/Metric-Producer patterns are documented in the preamble, and the FD/GW split is internally coherent. However, it is not ready for v1.0 bump: nine blockers cluster around three themes — (1) the GW Application layer breaks its own ISP/DIP rules by referencing concrete `SensorService / ConfigService / HealthMonitor / Logger` in `ConsoleService` and `ModbusPoller` USES lists, contradicting both the preamble and the FD counterpart spec (components-17, -18); (2) the Metric Producer Pattern is described in the preamble but the consumers (`ModbusRegisterMap`, `ModbusPoller`, `CloudPublisher`) never list the `IXxxStats` interfaces they claim to poll (components-10, -19, -20); and (3) two cross-document trace faults — `FirmwareStore` cites the wrong REQ-DM-070 for signature verification (components-13) and `HealthMonitor`'s LED-driving responsibility has no SRS requirement at all (components-2). Recommendation: hold the v1.0 HLD bump until the nine B-class items are resolved; the eleven F-class items (chiefly P6 trace gaps in driver/middleware responsibilities and the P10 USES-list naming inconsistency) should be cleared before LLD starts; the single D-class P8 fan-out flag on `LifecycleController` can be revisited at LLD time.