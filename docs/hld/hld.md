# High-Level Design — IoT Environmental Monitoring Gateway

**Version:** 0.1 (draft — components phase)
**Date:** April 2026
**Status:** In progress

---

## 1. Introduction

### 1.1 Purpose

This document is the master High-Level Design for the IoT Environmental Monitoring Gateway. It consolidates the architectural decisions, structural views, and design patterns applied across both nodes of the system.

Where this document references detailed specifications, the source material is in companion files:

- `vision.md` — system concept, scope, success criteria.
- `SRS.md` — testable functional and non-functional requirements.
- `use-case-descriptions.md` — narrative of every actor-system interaction.
- `domain-model.md` — entity catalogue and relationships.
- `components.md` — full per-component responsibility and interface specification.

### 1.2 How to read this document

Each architectural concern is presented through a *view* — a focused diagram answering one question. The document is layered: Section 2 sets the system context, Section 3 establishes the physical topology, Section 4 introduces the domain entities, Sections 5 and 6 decompose each node into software components, and Sections 7–8 explain the architectural patterns and hardware abstraction strategy that bind everything together.

The reader should expect to scroll through diagrams and prose in roughly equal measure. The diagrams anchor the structure; the prose carries the reasoning.

### 1.3 Methodology

The project follows a V-Model with model-based design. UML diagrams produced in Visual Paradigm are the authoritative design source; code is required to follow the model. Each artefact is traceable to the use case and requirement that motivated it.

The HLD phase consists of the following artefacts:

| # | Artefact | Status |
|---|----------|--------|
| 1 | System Deployment Diagram | Complete |
| 2 | Domain Model | Complete |
| 3 | Component Diagrams (per board, multi-view) | Complete |
| 4 | State Machine Diagrams | Pending |
| 5 | Data Flow / Sequence Diagrams | Pending |
| 6 | FreeRTOS Task Breakdown | Pending |
| 7 | Modbus Register Map | Pending |
| 8 | Flash Partition Layout | Pending |

This document is updated incrementally as each artefact is completed.

---

## 2. System context

The system addresses a recurring problem in industrial environmental monitoring: sensor data is captured at remote sites but is not visible until a technician visits, alarms go unnoticed, and configuration changes require a physical visit. The Vision document (`vision.md`) details the problem statement and stakeholder needs.

The system has three actors: a **Local Operator** physically present at the site, a **Remote Operator** working through a cloud interface, and a **Field Technician** maintaining the device.

![Use Case Diagram](../diagrams/use-case-diagram.png)

Twenty use cases are described in full in `use-case-descriptions.md`. They cluster into four concerns: data acquisition and display (UC-01 to UC-08), cloud communication (UC-05, UC-09 to UC-14), provisioning and diagnostics (UC-04, UC-15 to UC-17), and device lifecycle (UC-18 to UC-20).

---

## 3. System architecture — physical topology

The system is a two-node embedded network plus a cloud endpoint.

![System Deployment — Physical Topology](../diagrams/system-deployment-physical-topology.png)

The **Field Device** (STM32F469 Discovery) acquires sensor readings, displays them locally on its LCD, and exposes them as Modbus RTU registers. It is a Modbus slave.

The **Gateway** (B-L475E-IOT01A) polls the field device over RS-485, aggregates the data with its own onboard sensors, and publishes telemetry to AWS IoT Core via MQTT over TLS. It is the Modbus master on the local fieldbus and the cloud-facing edge.

The two nodes are physically connected by a half-duplex RS-485 link carrying Modbus RTU. The Gateway maintains an outbound TLS-secured MQTT connection to AWS IoT Core via WiFi.

The deployment diagram also marks trust boundaries: the local fieldbus is one trust zone, the gateway-to-cloud path is another, separated by the WiFi/TLS authenticated connection. Trust boundaries reflect authentication mechanisms, not physical location.

---

## 4. Domain model

The domain model captures the conceptual entities that the software manipulates and the relationships between them. It is the noun catalogue of the system, deliberately divorced from any implementation concern.

![Domain Model](../diagrams/domain-model.png)

Thirteen entities cover the operational domain: sensor data (`Sensor`, `SensorReading`, `MeasurementValue`), alarms (`Alarm`, `AlarmThreshold`), system state (`Device`, `DeviceHealthSnapshot`), persistence (`Configuration`, `LogEntry`, `BufferedRecord`), commands (`Command`), and cloud-bound payloads (`Telemetry`, `FirmwareImage`).

The `domain-model.md` companion document explains each entity, the relationships and their multiplicities, and the modelling decisions taken — notably the composition of `SensorReading` from one to three `MeasurementValue` instances (cardinality-based variation), the denormalisation of `Alarm` (so historical accuracy survives reconfiguration), and the polymorphic-by-composition treatment of `BufferedRecord` (preserving each payload's structure without inheritance).

---

## 5. Component design — Field Device

The Field Device firmware decomposes into 24 software components across four layers: Application, Middleware, Driver, and Hardware. The full per-component specification is in `components.md`. This section presents the components through five focused views, each answering one architectural question.

### 5.1 Component overview

The Field Device hosts seven application components, five middleware components, and twelve drivers. The complete list, with use case ownership and interface contracts, is in `components.md`.

The application layer is dominated by data exposure (LcdUi for the screen, ModbusRegisterMap for the fieldbus, ConsoleService for the CLI) and data production (SensorService, AlarmService). The middleware layer hosts the protocol stack (ModbusSlave), the graphics library (LVGL), persistence (ConfigStore), and cross-cutting services (Logger, TimeProvider). The driver layer is direct register access via CMSIS, with two drivers — BarometerDriver and HumidityTempDriver — implemented in software per Vision §5.1.1 (the field device simulates its sensors).

### 5.2 Data flow view

This view answers: *how does sensor data reach the LCD and the Modbus register table?*

![Field Device — Data Flow](../diagrams/Field%20Device%20-%20Data%20Flow.png)

Sensor data flows upward through the SensorService, which exposes the latest validated readings via `ISensorService`. Two consumers — LcdUi (for display) and ModbusRegisterMap (for register exposure) — pull data through this interface. The reading is timestamped via TimeProvider, which encapsulates the synchronisation-state flag mandated by Vision §8. AlarmService subscribes to SensorService's new-reading events and evaluates each reading against configured thresholds; the resulting alarm state is consumed by both LcdUi and ModbusRegisterMap.

The Modbus path runs from ModbusRegisterMap down through ModbusSlave (the protocol stack) to ModbusUartDriver. The LCD path runs from LcdUi through the GraphicsLibrary middleware (LVGL) down to LcdDriver and TouchscreenDriver. The CLI path runs from ConsoleService directly to DebugUartDriver.

### 5.3 Sensor and alarm pipeline view

This view answers: *how is alarm evaluation wired and where do thresholds come from?*

![Field Device — Sensor and Alarm Pipeline](../diagrams/Field%20Device%20-%20Sensor%20and%20Alarm%20Pipeline.png)

The sensor pipeline at the application level is event-driven. SensorService produces readings periodically, applies range validation (REQ-SA-120) and signal conditioning (REQ-SA-130, REQ-SA-140), and emits new-reading events to subscribers. AlarmService subscribes, reads thresholds and hysteresis settings from `IConfigProvider` (the read-side of ConfigService), and notifies its own subscribers when alarm state transitions occur.

The driver-level acquisition path is shown on the data flow view; this view abstracts it away to keep focus on the alarm evaluation logic.

### 5.4 System diagnostic and traceability view

This view answers: *how is logging wired across the system?*

![Field Device — System Diagnostic and Traceability View](../diagrams/Field%20Device%20-%20System%20Diagnostic%20and%20Traceability%20View.png)

Logger is a cross-cutting middleware service consumed by every Application and Middleware component for diagnostic output (REQ-NF-500). Drawing every Logger consumer connection on the other views would render them unreadable; the convention is to elide Logger from those views and document its consumers here.

Logger uses RtcDriver directly to obtain timestamps, bypassing TimeProvider. This is a documented bootstrap exception: TimeProvider depends on Logger (it logs sync errors), so Logger cannot also depend on TimeProvider without creating a circular dependency.

### 5.5 System health and telemetry pipeline view

This view answers: *how do health metrics flow from producers to displays?*

![Field Device — System Health and Telemetry Pipeline](../diagrams/Field%20Device%20-%20System%20Health%20and%20Telemetry%20Pipeline.png)

HealthMonitor is a passive collector at the application level. It exposes two interfaces: `IHealthSnapshot` (read-side, consumed by LcdUi, ConsoleService, and ModbusRegisterMap which exports it via Modbus to the Gateway) and `IHealthReport` (write-side, consumed by metric producers).

Producers fall into two categories. Application-layer producers (SensorService, ModbusRegisterMap, ConfigService, etc.) push metrics directly. Middleware producers participate via the Metric Producer Pattern: ModbusSlave exposes `IModbusSlaveStats` for accumulated counters (CRC errors, timeouts, transaction counts), polled by ModbusRegisterMap and reported upward; TimeProvider, ConfigStore, and similar middleware components depend on `IHealthReport` for event-based metrics (sync state changes, persistence failures). The DIP relationship — `IHealthReport` owned by HealthMonitor (Application) but consumed by Middleware — is what preserves layering despite the bottom-up data flow.

HealthMonitor also drives the on-board LEDs to indicate device status (idle, acquiring, alarm, error).

### 5.6 Configuration and persistence view

This view answers: *how do parameter changes propagate from input to flash?*

![Field Device — Configuration and Persistence View](../diagrams/Field%20Device%20-%20Configuration%20and%20Persistence%20View.png)

ConfigService applies Interface Segregation: writers consume `IConfigManager` (LcdUi, ConsoleService, ModbusRegisterMap for incoming writes from the Gateway), readers consume `IConfigProvider` (SensorService, AlarmService, ModbusRegisterMap for outgoing register exposure). Writes are validated, applied to in-memory state, and persisted via ConfigStore. ConfigStore wraps QspiFlashDriver and handles the wear-levelling and atomic-update concerns that belong in a persistence library, not in the application.

---

## 6. Component design — Gateway

The Gateway firmware decomposes into 31 software components: ten application, seven middleware, fourteen drivers. It is more diverse than the Field Device because of its dual role as fieldbus master and cloud-facing edge.

### 6.1 Component overview

The application layer hosts the cloud-facing components (CloudPublisher, StoreAndForward), the fieldbus orchestrator (ModbusPoller), the time service (TimeService), the firmware update orchestrator (UpdateService), plus the same set of services found on the Field Device (HealthMonitor, SensorService, AlarmService, ConsoleService, ConfigService).

The middleware layer adds the cloud and time protocols (MqttClient, NtpClient), the ring-buffer log over flash (CircularFlashLog), the firmware image manager (FirmwareStore), and the Modbus master stack (ModbusMaster). Logger, TimeProvider, and ConfigStore are present as on the Field Device.

The driver layer adds WiFi (over SPI) and the WiFi module's GPIO control lines, plus a software-reset driver used by the firmware update flow.

### 6.2 Data flow view

This view answers: *how does sensor data — both gateway-local and field-device-relayed — reach the cloud?*

![Gateway — Data Flow](../diagrams/Gateway%20-%20Data%20Flow%20view.png)

CloudPublisher is the central application component. It draws sensor data from two sources — the Gateway's own SensorService and the field device's data relayed by ModbusPoller — and publishes both via MqttClient, which sits on top of the WiFi driver. When the cloud is unreachable, CloudPublisher diverts payloads to StoreAndForward, which buffers them in CircularFlashLog over QspiFlashDriver until connectivity is restored.

The Gateway's own sensor pipeline mirrors the Field Device pattern: SensorService polls the four onboard sensors via I2C and timestamps readings via TimeProvider. The Modbus master pipeline (ModbusPoller → ModbusMaster → ModbusUartDriver) parallels it, with CloudPublisher consuming both via the respective interfaces.

### 6.3 Cloud publishing and resilience view

This view answers: *how does the Gateway behave when the cloud is unreachable, and how is firmware updated?*

![Gateway — Cloud Publishing and Resilience](../diagrams/Gateway%20-%20Cloud%20Publishing%20and%20Resilience.png)

The cloud path runs from CloudPublisher through MqttClient and WifiDriver. MqttClient maintains the TLS-secured connection and exposes connection state to its consumer.

When MQTT publish fails (connection lost, broker unreachable), CloudPublisher routes payloads into StoreAndForward, which appends them to CircularFlashLog. CircularFlashLog implements a chronological append-and-consume log over flash sectors, overwriting the oldest records when full. On reconnection, StoreAndForward replays buffered records in order.

UpdateService orchestrates firmware updates per UC-18. It downloads the new image via MqttClient and delegates storage and signature verification (REQ-DM-070) to FirmwareStore, a middleware component that owns flash partition management. After verification succeeds, UpdateService commits the slot switch via FirmwareStore and triggers a reboot via ResetDriver. Separating image management (middleware) from update orchestration (application) keeps each concern testable in isolation.

### 6.4 System diagnostic and traceability view

![Gateway — System Diagnostic and Traceability](../diagrams/Gateway%20-%20System%20Diagnostic%20and%20Traceability.png)

The same Logger pattern as the Field Device, applied to the Gateway's larger component set. Every Application and Middleware component depends on `ILogger`. Logger writes to DebugUartDriver and timestamps from RtcDriver (bootstrap exception).

### 6.5 Sensor and alarm pipeline view

![Gateway — Sensor and Alarm Pipeline](../diagrams/Gateway%20-%20Sensor%20and%20Alarm%20Pipeline.png)

The Gateway evaluates alarms only on its own sensor data. Field-device alarms arrive pre-evaluated via Modbus and are relayed by ModbusPoller as state-change events to CloudPublisher; they are not re-evaluated by the Gateway's AlarmService. This avoids double evaluation and respects the field device as the authority on its own measurements.

### 6.6 System health and telemetry pipeline view

![Gateway — System Health and Telemetry Pipeline](../diagrams/Gateway%20-%20System%20Health%20and%20Telemetry%20Pipeline.png)

Same DIP and Metric Producer Pattern as on the Field Device, with more producers. ModbusMaster exposes `IModbusMasterStats` (polled by ModbusPoller); MqttClient exposes `IMqttStats` (polled by CloudPublisher). The middleware event-pushers — TimeProvider, ConfigStore, NtpClient, CircularFlashLog — depend on `IHealthReport` directly. CloudPublisher both reads `IHealthSnapshot` (to publish health telemetry) and reports its own MQTT metrics via `IHealthReport`.

### 6.7 Configuration and persistence view

![Gateway — Configuration and Persistence](../diagrams/Gateway%20-%20Configuration%20and%20Persistence.png)

The same ConfigService split as on the Field Device. Writers (ConsoleService, CloudPublisher when receiving remote configuration commands) consume `IConfigManager`. Readers (SensorService, AlarmService, ModbusPoller) consume `IConfigProvider`. Persistence flows through ConfigStore to QspiFlashDriver.

---

## 7. Architectural patterns

The design draws on a small number of patterns. Each is applied to solve a concrete problem; none is decoration.

### 7.1 Layered architecture with strict directional dependency

The system has four layers: Application → Middleware → Driver → Hardware. Each layer depends only on layers below it. This is the foundation of the design.

When bottom-up data flow is required (Middleware reporting health metrics to an Application aggregator), Dependency Inversion is applied — the lower layer depends on an abstraction owned by the upper layer, never on the upper-layer implementation.

This pattern is universal in professional embedded codebases: AUTOSAR, Zephyr, FreeRTOS-based systems, MISRA-compliant automotive firmware. It is the floor, not the ceiling.

### 7.2 Interface Segregation Principle (ISP)

When a component has consumers with different needs, the component exposes multiple narrow interfaces rather than one fat interface.

- ConfigService provides `IConfigManager` (write-side) and `IConfigProvider` (read-side). Writers and readers depend only on what they use.
- HealthMonitor provides `IHealthSnapshot` (read-side) and `IHealthReport` (write-side). Readers and producers see different surfaces.

ISP reduces coupling, simplifies testing (mock only the interface used), and makes architectural intent explicit. It is a core SOLID principle and standard in shipping commercial firmware.

### 7.3 Dependency Inversion Principle (DIP)

When an upper layer must receive data from a lower layer at runtime — for example, Middleware producers reporting metrics to an Application HealthMonitor — the upper layer defines the abstraction. The lower layer depends on the abstraction, never on the implementation.

In this project, `IHealthReport` is owned by HealthMonitor (Application) but consumed by middleware producers (TimeProvider, ConfigStore, NtpClient, CircularFlashLog). Middleware sees only the abstraction; the layering rule is preserved because dependency points at an interface, not at HealthMonitor itself.

The visual convention in the component diagrams: the abstraction ball is attached to the implementing upper-layer component; lower-layer consumers reach upward with sockets. This is standard component-diagram notation for DIP — the inversion is conceptual, not geometric.

DIP appears in Zephyr's logging backends, AUTOSAR's Diagnostic Event Manager, and C++ IoC-style frameworks throughout embedded.

### 7.4 Metric Producer Pattern

Two mechanisms are used for health metric flow, chosen by metric type:

- **Stats polling** via `IXxxStats`: for accumulated counters (CRC error counts, transaction counts, reconnection counts). Middleware exposes the stats interface; an Application consumer polls and reports through `IHealthReport`. Examples: `IModbusSlaveStats`, `IModbusMasterStats`, `IMqttStats`.
- **Direct push** via `IHealthReport`: for event-based signals (sync state change, persistence failure, NTP query failure). The producer pushes when the event occurs.

The test: *is this a counter I want to read at any sample time, or an event I need to report when it happens?* Counters → poll. Events → push.

This mixed approach matches industry convention. lwIP exposes statistics through a query API and reports events through callbacks; FreeRTOS+TCP and Zephyr's network subsystem use the same pattern.

### 7.5 Observer (event-driven subscription)

SensorService emits new-reading events. AlarmService subscribes. Each new reading triggers immediate alarm evaluation, satisfying REQ-NF-101 (one-polling-cycle alarm detection) without polling.

This avoids two failure modes of polling: alarm detection latency (waiting for the polling tick) and double polling (SensorService produces, AlarmService polls, the rates rarely align).

### 7.6 Mediator

ModbusRegisterMap mediates between ConfigService (the configuration source) and ModbusSlave (the protocol stack). When a Modbus master writes to a configuration register, the write reaches ModbusSlave first; ModbusRegisterMap dispatches it to ConfigService for validation and application. When configuration changes affect protocol behaviour (slave address, baud rate), ModbusRegisterMap reads from ConfigService and pushes to ModbusSlave.

This keeps ModbusSlave ignorant of project-specific configuration, allowing the protocol stack to remain a reusable middleware component.

### 7.7 Pull-based access

When a producer feeds multiple consumers, prefer a pull interface (consumers query for data) over push (producer notifies each consumer). Producers stay unaware of consumers.

SensorService exposes `ISensorService.get_latest()`. LcdUi, ModbusRegisterMap, ConsoleService, AlarmService all consume the same interface. SensorService adds or removes nothing when consumers change.

The exception is when a consumer's responsiveness requirement is tighter than the producer's natural rate — then event subscription (Observer) applies, as in the SensorService → AlarmService relationship.

### 7.8 Store-and-Forward

Telemetry destined for an unreliable cloud must survive connectivity loss. CloudPublisher routes payloads to MqttClient when online; when offline, it diverts to StoreAndForward, which persists records in a circular log over flash. On reconnection, the log is drained in chronological order.

This is the standard industrial-IoT resilience pattern. AWS IoT Greengrass, Azure IoT Edge, Siemens MindSphere, and HMS Networks Anybus all use this exact structure.

---

## 8. Hardware abstraction strategy

Vision §9 establishes the portability stance: no STM32 HAL above the driver layer; CMSIS-only inside drivers. This section explains how each driver realises that stance.

The drivers fall into three categories:

| Driver | Approach | Rationale |
|--------|----------|-----------|
| **DebugUartDriver** | CMSIS register-level | UART configuration is straightforward at the register level. Demonstrates competence with peripheral programming without reinventing complex flow. |
| **ModbusUartDriver** | CMSIS register-level | Same reasoning as DebugUartDriver, with additional DMA configuration for half-duplex RS-485 timing. |
| **I2cDriver** | CMSIS register-level | I2C state machine is a classic embedded driver exercise. Implementing it directly demonstrates protocol understanding. |
| **SpiDriver** *(Gateway)* | CMSIS register-level | Same reasoning as I2C. Used by WifiDriver. |
| **GpioDriver** | CMSIS register-level | GPIO is the simplest peripheral. HAL would be overhead. |
| **LedDriver** | Wraps GpioDriver | One level above GPIO; provides on/off semantics. |
| **RtcDriver** | CMSIS register-level | RTC is a self-contained backup-domain peripheral; HAL adds no value. |
| **ResetDriver** *(Gateway)* | CMSIS (NVIC_SystemReset) | One-line implementation. Wrapping `NVIC_SystemReset()` would be ceremony without substance. |
| **SdramDriver** *(Field)* | CMSIS register-level | FMC controller initialisation. Complex but well-documented in the reference manual. |
| **QspiFlashDriver** | CMSIS register-level | QSPI controller programming is non-trivial but isolated to one driver. |
| **LcdDriver** *(Field)* | CMSIS register-level + framebuffer in SDRAM | DSI controller programming. Framebuffer location handled via SdramDriver. |
| **TouchscreenDriver** *(Field)* | Wraps I2cDriver | I2C transactions to the touchscreen controller; no special HAL feature. |
| **WifiDriver** *(Gateway)* | AT-command implementation over SPI | Inventek ISM43362 module is controlled via AT commands. No HAL involvement. |
| **Sensor drivers** *(Gateway sensors)* | Wraps I2cDriver | Each sensor (Magnetometer, IMU, Barometer, Humidity/Temp) is an I2C device with a register map. The driver translates between sensor-specific protocol and the generic `IXxx` interface. |
| **Sensor drivers** *(Field Device)* | Software simulation | Per Vision §5.1.1, the Field Device simulates its sensors. The drivers expose the same interfaces (`IBarometer`, `IHumidityTemp`) as the Gateway equivalents, so the application layer is unaware of the difference. |

The result: every peripheral access in the system is direct CMSIS code. No vendor library is imported above the driver layer. The interfaces consumed by Middleware and Application are vendor-neutral by construction.

For middleware that wraps a third-party library, the same boundary discipline applies. GraphicsLibrary on the Field Device wraps LVGL — a vendor-neutral, MIT-licensed C graphics library — and exposes `IGraphics` upward. LVGL is preferred over TouchGFX because TouchGFX's tooling assumes STM32CubeMX integration, conflicting with the portability stance.

---

## 9. Forward-looking artefacts

This document will be extended as the remaining HLD artefacts are completed.

### 9.1 State machine diagrams (Phase 2.4)

Per-component state machines for components with substantive lifecycle logic. Candidates: WiFi connection management, MQTT session, Modbus master-slave protocol, alarm hysteresis, firmware update sequence.

### 9.2 Sequence diagrams (Phase 2.5)

End-to-end interaction diagrams for representative use cases. Candidates: UC-07 (acquire sensor data), UC-08 (evaluate alarms), UC-09/10 (publish telemetry, with offline buffering), UC-13 (time synchronisation), UC-18 (firmware update).

These sequence diagrams will also serve as a verification pass on the component spec — if a sequence cannot be drawn cleanly with the existing component set, that exposes a gap in the HLD which feeds back into a spec update.

### 9.3 FreeRTOS task breakdown (Phase 2.6)

Per-board task list with priorities, stack sizes, and inter-task communication mechanisms (queues, mutexes, semaphores). Mapping from components to FreeRTOS tasks is many-to-one in some cases (one task hosts multiple application components) and one-to-one in others.

### 9.4 Modbus register map (Phase 2.7)

The complete register table exposed by ModbusRegisterMap: address ranges, data types, read/write semantics, scaling factors. Source of truth for the field-device-to-gateway protocol.

### 9.5 Flash partition layout (Phase 2.8)

Memory map of the QSPI flash on each board, partitioned across firmware slots, configuration storage, circular log, and any reserved regions. Drives FirmwareStore, ConfigStore, and CircularFlashLog implementations.

---

## 10. Architectural decisions log

The following decisions were considered and resolved during the components phase. They are recorded here so an interview reviewer can see what was rejected as well as what was adopted.

### 10.1 Decisions adopted

| Decision | Rationale |
|----------|-----------|
| ConfigService split into `IConfigManager` and `IConfigProvider` | ISP — readers and writers have different needs |
| HealthMonitor split into `IHealthSnapshot` and `IHealthReport` | ISP — same reasoning, applied to health |
| `IHealthReport` consumed by Middleware via DIP | Preserves layering while permitting bottom-up data flow |
| Metric Producer Pattern: stats polling for counters, direct push for events | Matches industry convention; respects responsiveness requirements |
| ModbusRegisterMap as Mediator between ConfigService and ModbusSlave | Keeps ModbusSlave reusable as a protocol library |
| Pull-based access for sensor data | Decouples producer from consumers |
| Event-driven Observer for SensorService → AlarmService | Satisfies REQ-NF-101 alarm detection latency |
| Logger uses RtcDriver directly (not TimeProvider) | Bootstrap exception — avoids circular dependency |
| FirmwareStore as middleware separating image management from update orchestration | Signature verification belongs with image storage, not with control flow |
| Multi-view component diagrams (5 per Field Device, 6 per Gateway) | Each view answers one question; full graph stays in textual spec |

### 10.2 Decisions rejected

| Considered | Rejected because |
|------------|------------------|
| FieldService component on the Field Device for hypothetical remote restart | No use case requires it; UC-17 is gateway-only by design (clarified during components phase) |
| EventBus / Mediator helpers across the application layer | Current dependency density does not cross painful threshold (P8 in `architecture-principles.md`) |
| SignalProcessing middleware between sensor drivers and SensorService | No reusable abstraction over current scope; sensor conditioning belongs inside SensorService |
| TextFormatter middleware for ConsoleService | CLI is small enough that formatting helpers are utility functions, not a middleware component |
| Push-based sensor data distribution (SensorService writes into each consumer) | Couples SensorService to all consumers; reversed in favour of pull-based access |
| Robustness analysis (analysis-level sequence diagrams) between SRS and component diagrams | Deferred to LLD sequence diagrams, which serve dual purpose: API-level interactions plus HLD verification pass |
| Per-driver direct push to HealthMonitor (no stats interface) | Violates layering when middleware pushes to application; replaced with Metric Producer Pattern |
| Single-view "component overview" diagram showing all components and connections | Would be unreadable; replaced with multi-view approach |

---

*This document is the master HLD. It is updated as each artefact is completed. Detailed specifications live in the companion files referenced in §1.1.*
