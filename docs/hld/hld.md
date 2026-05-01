# High-Level Design — IoT Environmental Monitoring Gateway

**Version:** 0.2 (draft — state machines phase)
**Date:** May 2026
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
- `state-machines.md` — runtime state machines of both nodes, with full state lists, transition tables, and traceability.

### 1.2 How to read this document

Each architectural concern is presented through a *view* — a focused diagram answering one question. The document is layered: Section 2 sets the system context, Section 3 establishes the physical topology, Section 4 introduces the domain entities, Sections 5 and 6 decompose each node into software components, Section 7 describes the runtime behaviour through state machines, and Sections 8–9 explain the architectural patterns and hardware abstraction strategy that bind everything together.

The reader should expect to scroll through diagrams and prose in roughly equal measure. The diagrams anchor the structure; the prose carries the reasoning.

### 1.3 Methodology

The project follows a V-Model with model-based design. UML diagrams produced in Visual Paradigm are the authoritative design source; code is required to follow the model. Each artefact is traceable to the use case and requirement that motivated it.

The HLD phase consists of the following artefacts:

| # | Artefact | Status |
|---|----------|--------|
| 1 | System Deployment Diagram | Complete |
| 2 | Domain Model | Complete |
| 3 | Component Diagrams (per board, multi-view) | Complete |
| 4 | State Machine Diagrams | Complete |
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

The Field Device firmware decomposes into 25 software components across four layers: Application, Middleware, Driver, and Hardware. The full per-component specification is in `components.md`. This section presents the components through five focused views, each answering one architectural question.

### 5.1 Component overview

The Field Device hosts eight application components, five middleware components, and twelve drivers. The complete list, with use case ownership and interface contracts, is in `components.md`.

The application layer is dominated by data exposure (LcdUi for the screen, ModbusRegisterMap for the fieldbus, ConsoleService for the CLI) and data production (SensorService, AlarmService). It also includes `LifecycleController`, the explicit owner of the field-device top-level lifecycle state machine — coordinating Init sub-step sequencing, splash-screen progression, Operational ↔ EditingConfig transitions, and Faulted entry on unrecoverable conditions. The middleware layer hosts the protocol stack (ModbusSlave), the graphics library (LVGL), persistence (ConfigStore), and cross-cutting services (Logger, TimeProvider). The driver layer is direct register access via CMSIS, with two drivers — BarometerDriver and HumidityTempDriver — implemented in software per Vision §5.1.1 (the field device simulates its sensors).

### 5.2 Data flow view

This view answers: *how does sensor data reach the LCD and the Modbus register table?*

![Field Device — Data Flow](../diagrams/component-field-device-data-flow.png)

Sensor data flows upward through the SensorService, which exposes the latest validated readings via `ISensorService`. Two consumers — LcdUi (for display) and ModbusRegisterMap (for register exposure) — pull data through this interface. The reading is timestamped via TimeProvider, which encapsulates the synchronisation-state flag mandated by Vision §8. AlarmService subscribes to SensorService's new-reading events and evaluates each reading against configured thresholds; the resulting alarm state is consumed by both LcdUi and ModbusRegisterMap.

The Modbus path runs from ModbusRegisterMap down through ModbusSlave (the protocol stack) to ModbusUartDriver. The LCD path runs from LcdUi through the GraphicsLibrary middleware (LVGL) down to LcdDriver and TouchscreenDriver. The CLI path runs from ConsoleService directly to DebugUartDriver.

### 5.3 Sensor and alarm pipeline view

This view answers: *how is alarm evaluation wired and where do thresholds come from?*

![Field Device — Sensor and Alarm Pipeline](../diagrams/component-field-device-sensor-and-alarm-pipeline.png)

The sensor pipeline at the application level is event-driven. SensorService produces readings periodically, applies range validation (REQ-SA-120) and signal conditioning (REQ-SA-130, REQ-SA-140), and emits new-reading events to subscribers. AlarmService subscribes, reads thresholds and hysteresis settings from `IConfigProvider` (the read-side of ConfigService), and notifies its own subscribers when alarm state transitions occur.

The driver-level acquisition path is shown on the data flow view; this view abstracts it away to keep focus on the alarm evaluation logic.

### 5.4 System diagnostic and traceability view

This view answers: *how is logging wired across the system?*

![Field Device — System Diagnostic and Traceability View](../diagrams/component-field-device-system-diagnostic-and-traceability.png)

Logger is a cross-cutting middleware service consumed by every Application and Middleware component for diagnostic output (REQ-NF-500). Drawing every Logger consumer connection on the other views would render them unreadable; the convention is to elide Logger from those views and document its consumers here.

Logger uses RtcDriver directly to obtain timestamps, bypassing TimeProvider. This is a documented bootstrap exception: TimeProvider depends on Logger (it logs sync errors), so Logger cannot also depend on TimeProvider without creating a circular dependency.

### 5.5 System health and telemetry pipeline view

This view answers: *how do health metrics flow from producers to displays?*

![Field Device — System Health and Telemetry Pipeline](../diagrams/component-field-device-system-health-and-telemetry-pipeline.png)

HealthMonitor is a passive collector at the application level. It exposes two interfaces: `IHealthSnapshot` (read-side, consumed by LcdUi, ConsoleService, and ModbusRegisterMap which exports it via Modbus to the Gateway) and `IHealthReport` (write-side, consumed by metric producers).

Producers fall into two categories. Application-layer producers (SensorService, ModbusRegisterMap, ConfigService, etc.) push metrics directly. Middleware producers participate via the Metric Producer Pattern: ModbusSlave exposes `IModbusSlaveStats` for accumulated counters (CRC errors, timeouts, transaction counts), polled by ModbusRegisterMap and reported upward; TimeProvider, ConfigStore, and similar middleware components depend on `IHealthReport` for event-based metrics (sync state changes, persistence failures). The DIP relationship — `IHealthReport` owned by HealthMonitor (Application) but consumed by Middleware — is what preserves layering despite the bottom-up data flow.

HealthMonitor also drives the on-board LEDs to indicate device status (idle, acquiring, alarm, error).

### 5.6 Configuration and persistence view

This view answers: *how do parameter changes propagate from input to flash?*

![Field Device — Configuration and Persistence View](../diagrams/component-field-device-configuration-and-persistence.png)

ConfigService applies Interface Segregation: writers consume `IConfigManager` (LcdUi, ConsoleService, ModbusRegisterMap for incoming writes from the Gateway), readers consume `IConfigProvider` (SensorService, AlarmService, ModbusRegisterMap for outgoing register exposure). Writes are validated, applied to in-memory state, and persisted via ConfigStore. ConfigStore wraps QspiFlashDriver and handles the wear-levelling and atomic-update concerns that belong in a persistence library, not in the application.

---

## 6. Component design — Gateway

The Gateway firmware decomposes into 32 software components: eleven application, seven middleware, fourteen drivers. It is more diverse than the Field Device because of its dual role as fieldbus master and cloud-facing edge.

### 6.1 Component overview

The application layer hosts the cloud-facing components (CloudPublisher, StoreAndForward), the fieldbus orchestrator (ModbusPoller), the time service (TimeService), the firmware update orchestrator (UpdateService), plus the same set of services found on the Field Device (HealthMonitor, SensorService, AlarmService, ConsoleService, ConfigService). The application layer also hosts `LifecycleController`, which owns the gateway top-level lifecycle — coordinating Init sub-steps, the restart-confirmation flow (UC-17), the firmware update handoff to UpdateService (UC-18), and Faulted entry on unrecoverable conditions.

The middleware layer adds the cloud and time protocols (MqttClient, NtpClient), the ring-buffer log over flash (CircularFlashLog), the firmware image manager (FirmwareStore), and the Modbus master stack (ModbusMaster). Logger, TimeProvider, and ConfigStore are present as on the Field Device.

The driver layer adds WiFi (over SPI) and the WiFi module's GPIO control lines, plus a software-reset driver used by the firmware update flow.

### 6.2 Data flow view

This view answers: *how does sensor data — both gateway-local and field-device-relayed — reach the cloud?*

![Gateway — Data Flow](../diagrams/component-gateway-data-flow.png)

CloudPublisher is the central application component. It draws sensor data from two sources — the Gateway's own SensorService and the field device's data relayed by ModbusPoller — and publishes both via MqttClient, which sits on top of the WiFi driver. When the cloud is unreachable, CloudPublisher diverts payloads to StoreAndForward, which buffers them in CircularFlashLog over QspiFlashDriver until connectivity is restored.

The Gateway's own sensor pipeline mirrors the Field Device pattern: SensorService polls the four onboard sensors via I2C and timestamps readings via TimeProvider. The Modbus master pipeline (ModbusPoller → ModbusMaster → ModbusUartDriver) parallels it, with CloudPublisher consuming both via the respective interfaces.

### 6.3 Cloud publishing and resilience view

This view answers: *how does the Gateway behave when the cloud is unreachable, and how is firmware updated?*

![Gateway — Cloud Publishing and Resilience](../diagrams/component-gateway-cloud-publishing-and-resilience.png)

The cloud path runs from CloudPublisher through MqttClient and WifiDriver. MqttClient maintains the TLS-secured connection and exposes connection state to its consumer.

When MQTT publish fails (connection lost, broker unreachable), CloudPublisher routes payloads into StoreAndForward, which appends them to CircularFlashLog. CircularFlashLog implements a chronological append-and-consume log over flash sectors, overwriting the oldest records when full. On reconnection, StoreAndForward replays buffered records in order.

UpdateService orchestrates firmware updates per UC-18. It downloads the new image via MqttClient and delegates storage and signature verification (REQ-DM-070) to FirmwareStore, a middleware component that owns flash partition management. After verification succeeds, UpdateService commits the slot switch via FirmwareStore and triggers a reboot via ResetDriver. Separating image management (middleware) from update orchestration (application) keeps each concern testable in isolation.

### 6.4 System diagnostic and traceability view

![Gateway — System Diagnostic and Traceability](../diagrams/component-gateway-system-diagnostic-and-traceability.png)

The same Logger pattern as the Field Device, applied to the Gateway's larger component set. Every Application and Middleware component depends on `ILogger`. Logger writes to DebugUartDriver and timestamps from RtcDriver (bootstrap exception).

### 6.5 Sensor and alarm pipeline view

![Gateway — Sensor and Alarm Pipeline](../diagrams/component-gateway-sensor-and-alarm-pipeline.png)

The Gateway evaluates alarms only on its own sensor data. Field-device alarms arrive pre-evaluated via Modbus and are relayed by ModbusPoller as state-change events to CloudPublisher; they are not re-evaluated by the Gateway's AlarmService. This avoids double evaluation and respects the field device as the authority on its own measurements.

### 6.6 System health and telemetry pipeline view

![Gateway — System Health and Telemetry Pipeline](../diagrams/component-gateway-system-health-and-telemetry-pipeline.png)

Same DIP and Metric Producer Pattern as on the Field Device, with more producers. ModbusMaster exposes `IModbusMasterStats` (polled by ModbusPoller); MqttClient exposes `IMqttStats` (polled by CloudPublisher). The middleware event-pushers — TimeProvider, ConfigStore, NtpClient, CircularFlashLog — depend on `IHealthReport` directly. CloudPublisher both reads `IHealthSnapshot` (to publish health telemetry) and reports its own MQTT metrics via `IHealthReport`.

### 6.7 Configuration and persistence view

![Gateway — Configuration and Persistence](../diagrams/component-gateway-configuration-and-persistence.png)

The same ConfigService split as on the Field Device. Writers (ConsoleService, CloudPublisher when receiving remote configuration commands) consume `IConfigManager`. Readers (SensorService, AlarmService, ModbusPoller) consume `IConfigProvider`. Persistence flows through ConfigStore to QspiFlashDriver.

---

## 7. Behavioural design — state machines

The component design in Sections 5 and 6 establishes *what exists*. This section establishes *how the system behaves over time*: the runtime modes each subsystem goes through, the events that drive transitions, and the actions performed at each step. Six state machines capture the substantive lifecycle logic in the system; each is owned by a specific component and traces back to use cases and SRS requirements.

The full state lists, transition tables, internal-transition tables, and traceability matrices are in `state-machines.md`. This section presents each machine with its diagram and a summary paragraph; the companion document is the source of truth for state-by-state detail.

### 7.1 Inventory and ownership

| #   | Machine                       | Owner component                                                                                | Board         |
|-----|-------------------------------|------------------------------------------------------------------------------------------------|---------------|
| 1   | Gateway Lifecycle             | `LifecycleController` (Application)                                                            | Gateway       |
| 2   | Cloud Connectivity            | `CloudPublisher` (Application)                                                                 | Gateway       |
| 3   | Firmware Update               | `UpdateService` (Application)                                                                  | Gateway       |
| 4   | Modbus Master                 | `ModbusPoller` (Application; protocol-level frame handling delegated to `ModbusMaster` Middleware) | Gateway       |
| 5   | Field Device Lifecycle        | `LifecycleController` (Application)                                                            | Field Device  |
| 6   | Modbus Slave                  | `ModbusSlave` (Middleware)                                                                     | Field Device  |

A seventh candidate — a per-channel alarm state machine (Clear ↔ Active with hysteresis) — is deliberately deferred to LLD. It is per-instance rather than per-system, the states are trivial (Clear, Active) with a single guarded transition pair, and including it at HLD level would clutter without adding clarity.

State machine diagrams in this project show **structural transitions only**. Internal transitions, entry / do / exit actions, and other behavioural compartments are listed in `state-machines.md`, not on the diagrams. The diagrams answer *"what states exist and how do they connect?"*; the companion document answers *"what does each state actually do?"*. Separating the two keeps the diagrams readable and the behavioural specification authoritative in one place.

### 7.2 Gateway lifecycle

![Gateway Lifecycle](../diagrams/state-machine-gateway-lifecycle.png)

The richest top-level machine in the system. Six top-level states (Init, Operational, EditingConfig, Restarting, UpdatingFirmware, Faulted) plus a five-step composite Init (CheckingIntegrity → LoadingConfig → BringingUpSensors → StartingMiddleware → SelfChecking). Twenty-one state transitions and fifteen internal transitions handle the full operational lifecycle including remote restart with confirmation (UC-17), firmware update handoff (UC-18), CLI provisioning, and unrecoverable-fault entry.

A key architectural decision is encoded here: the gateway lifecycle has no "Degraded" state, even though cloud connectivity may be lost at runtime. Per REQ-NF-200, cloud loss does not change the gateway's top-level mode — it only changes the Cloud Connectivity sub-machine's state (§7.3). Surfacing the distinction here would duplicate logic the sub-machine already owns. EditingConfig is promoted to a top-level state rather than an Operational sub-state because it carries a distinct exit timeout, snapshot/rollback semantics, and a cross-machine event (`internet_params_changed`) that triggers Cloud Connectivity to reconnect with new credentials.

### 7.3 Cloud connectivity (sub-machine)

![Cloud Connectivity](../diagrams/state-machine-cloud-connectivity.png)

Owned by `CloudPublisher`. Models the gateway's relationship with AWS IoT Core: connect, publish, lose connection, reconnect, drain buffer. Three top-level states (Connecting, Connected — composite with Draining and Publishing sub-states, Disconnected) plus a choice pseudo-state at entry to Connected that picks the initial sub-state based on buffer occupancy.

The store-and-forward semantics mandated by REQ-BF-000, -010, -020 are encoded as internal transitions: Disconnected enqueues new outbound messages and drops oldest when the buffer fills; Connected.Draining publishes buffered messages chronologically; Connected.Publishing forwards live messages directly to MQTT. Reconnect attempts at 1 Hz (REQ-NF-209) are driven by a timer in Disconnected. The boundary transition from Connected to Disconnected uses `internet_params_changed` to handle credential changes from the gateway lifecycle's EditingConfig path.

### 7.4 Firmware update (sub-machine)

![Firmware Update](../diagrams/state-machine-firmware-update.png)

Owned by `UpdateService`. The most state-rich machine in the system, spanning up to two MCU reboots. Eight states (Idle, Downloading, Validating, Applying, SelfChecking, RollingBack, Committed, Failed) with the dual-bank update sequence: download to inactive bank → verify signature and integrity → set inactive bank as boot → reboot → self-check in new firmware → commit or roll back.

Reboots are not states. They are **transition actions** (`NVIC_SystemReset()`) followed by flag-driven resume on the next boot: gateway-lifecycle Init detects `pending_self_check` or `pending_rollback` and resumes this machine in the appropriate state via the entry-point pseudo-state shown on the diagram. A choice diamond branches on which flag is set, with a UML note documenting that this entry path fires only on the post-update boot — otherwise the machine remains in Idle across the reboot.

### 7.5 Modbus master (sub-machine)

![Modbus Master](../diagrams/state-machine-modbus-master.png)

Owned by `ModbusPoller` (with protocol-level frame handling delegated to the `ModbusMaster` Middleware library). Active machine: drives transitions via internal timers. Four states (Idle, Transmitting, AwaitingResponse, ProcessingResponse) with the canonical Modbus polling cycle: send request, wait for response with 200 ms timeout (REQ-MB-050), retry up to three times on timeout (REQ-MB-060), record poll outcome.

Link-state hysteresis (REQ-NF-103, NF-104, NF-215) — declaring the Modbus link offline after three consecutive failed polls and online after three consecutive successful polls — is modelled as transition actions on a `link_state` model variable, not as separate states. The yellow note on the diagram documents the hysteresis pseudocode. An alternative HSM with Online and Offline as composite states each containing the same polling sub-states was considered and rejected as visually redundant.

### 7.6 Field device lifecycle

![Field Device Lifecycle](../diagrams/state-machine-field-device-lifecycle.png)

Simpler than the gateway: no cloud, no firmware update, no remote restart. Four top-level states (Init, Operational, EditingConfig, Faulted) plus a five-step composite Init (CheckingIntegrity → LoadingConfig → BringingUpSensors → BringingUpLCD → StartingMiddleware). Note the differences from the gateway Init: BringingUpLCD replaces SelfChecking, because LCD is essential per REQ-LD-000 and there is no remote-restart self-check (UC-17 is gateway-only).

The field device's complexity lives mostly *inside* Operational rather than *across* states: ten internal transitions handle sensor polling, LCD refresh, alarm evaluation, Modbus register updates, time-push reception, on-demand reads, and CLI diagnostics. Like the gateway, EditingConfig is a top-level state rather than an Operational sub-state, with a cross-machine `modbus_address_changed` event that updates the Modbus Slave's address filter on apply.

### 7.7 Modbus slave (sub-machine)

![Modbus Slave](../diagrams/state-machine-modbus-slave.png)

Owned by `ModbusSlave` Middleware. **Reactive machine — no timers, no retries, no polling.** Frame reception drives every transition. Three states (Idle, ProcessingRequest, Responding) with the smallest transition table in the system: five state transitions, two internal transitions.

The contrast with the Modbus Master diagram (§7.5) is the point: the master has timeouts, retries, and link-state hysteresis; the slave has none of these. A bad frame is silently dropped; the slave returns to Idle ready for the next frame. REQ-MB-050 (200 ms timeout), REQ-MB-060 (3-retry), REQ-NF-103 / NF-104 (link-state hysteresis) are all explicitly master-side concerns. The yellow note on the diagram makes this asymmetry explicit so that absence of timer/retry logic reads as deliberate, not as an omission.

### 7.8 Cross-machine relationships

The six machines couple at runtime through clearly named events that cross machine boundaries:

- **Gateway lifecycle ↔ Cloud Connectivity** — Init triggers Cloud Connectivity start (REQ-CC-050); thereafter independent (REQ-NF-200).
- **Gateway lifecycle ↔ Firmware Update** — `UpdatingFirmware` composite delegates via `«submachine»` stereotype; `update_done` returns control via three guarded transitions (success / rollback OK / unrecoverable). The Apply→reboot→SelfChecking and RollingBack→reboot→Failed reboot chains both cross gateway-lifecycle Init via persisted flags.
- **Gateway lifecycle ↔ Modbus Master** — Init starts the master; Modbus failures surface via `node_offline` event consumed by HealthMonitor / CloudPublisher, never changing gateway top-level state.
- **Gateway EditingConfig → Cloud Connectivity** — on apply, emits `internet_params_changed`; Cloud Connectivity force-disconnects and reconnects with new credentials.
- **Field Device lifecycle ↔ Modbus Slave** — Init brings up the slave; thereafter the slave runs autonomously. Time-push frames received by the slave (REQ-MB-020) emit `modbus_time_push_received`, consumed by Operational's RTC sync internal transition.
- **Field Device EditingConfig → Modbus Slave** — on Modbus-address change, emits `modbus_address_changed`; the slave updates its address filter without restarting.
- **Modbus Master (gateway) ↔ Modbus Slave (field device)** — RS-485 half-duplex over UART. The two machines never see each other's internal state — only frames on the bus and the cause/effect they imply.

`state-machines.md` §Cross-machine relationships tabulates these couplings in full. Sequence diagrams in HLD Artefact #5 will illustrate the same interactions concretely along the time axis.

---

## 8. Architectural patterns

The design draws on a small number of patterns. Each is applied to solve a concrete problem; none is decoration.

### 8.1 Layered architecture with strict directional dependency

The system has four layers: Application → Middleware → Driver → Hardware. Each layer depends only on layers below it. This is the foundation of the design.

When bottom-up data flow is required (Middleware reporting health metrics to an Application aggregator), Dependency Inversion is applied — the lower layer depends on an abstraction owned by the upper layer, never on the upper-layer implementation.

This pattern is universal in professional embedded codebases: AUTOSAR, Zephyr, FreeRTOS-based systems, MISRA-compliant automotive firmware. It is the floor, not the ceiling.

### 8.2 Interface Segregation Principle (ISP)

When a component has consumers with different needs, the component exposes multiple narrow interfaces rather than one fat interface.

- ConfigService provides `IConfigManager` (write-side) and `IConfigProvider` (read-side). Writers and readers depend only on what they use.
- HealthMonitor provides `IHealthSnapshot` (read-side) and `IHealthReport` (write-side). Readers and producers see different surfaces.

ISP reduces coupling, simplifies testing (mock only the interface used), and makes architectural intent explicit. It is a core SOLID principle and standard in shipping commercial firmware.

### 8.3 Dependency Inversion Principle (DIP)

When an upper layer must receive data from a lower layer at runtime — for example, Middleware producers reporting metrics to an Application HealthMonitor — the upper layer defines the abstraction. The lower layer depends on the abstraction, never on the implementation.

In this project, `IHealthReport` is owned by HealthMonitor (Application) but consumed by middleware producers (TimeProvider, ConfigStore, NtpClient, CircularFlashLog). Middleware sees only the abstraction; the layering rule is preserved because dependency points at an interface, not at HealthMonitor itself.

The visual convention in the component diagrams: the abstraction ball is attached to the implementing upper-layer component; lower-layer consumers reach upward with sockets. This is standard component-diagram notation for DIP — the inversion is conceptual, not geometric.

DIP appears in Zephyr's logging backends, AUTOSAR's Diagnostic Event Manager, and C++ IoC-style frameworks throughout embedded.

### 8.4 Metric Producer Pattern

Two mechanisms are used for health metric flow, chosen by metric type:

- **Stats polling** via `IXxxStats`: for accumulated counters (CRC error counts, transaction counts, reconnection counts). Middleware exposes the stats interface; an Application consumer polls and reports through `IHealthReport`. Examples: `IModbusSlaveStats`, `IModbusMasterStats`, `IMqttStats`.
- **Direct push** via `IHealthReport`: for event-based signals (sync state change, persistence failure, NTP query failure). The producer pushes when the event occurs.

The test: *is this a counter I want to read at any sample time, or an event I need to report when it happens?* Counters → poll. Events → push.

This mixed approach matches industry convention. lwIP exposes statistics through a query API and reports events through callbacks; FreeRTOS+TCP and Zephyr's network subsystem use the same pattern.

### 8.5 Observer (event-driven subscription)

SensorService emits new-reading events. AlarmService subscribes. Each new reading triggers immediate alarm evaluation, satisfying REQ-NF-101 (one-polling-cycle alarm detection) without polling.

This avoids two failure modes of polling: alarm detection latency (waiting for the polling tick) and double polling (SensorService produces, AlarmService polls, the rates rarely align).

### 8.6 Mediator

ModbusRegisterMap mediates between ConfigService (the configuration source) and ModbusSlave (the protocol stack). When a Modbus master writes to a configuration register, the write reaches ModbusSlave first; ModbusRegisterMap dispatches it to ConfigService for validation and application. When configuration changes affect protocol behaviour (slave address, baud rate), ModbusRegisterMap reads from ConfigService and pushes to ModbusSlave.

This keeps ModbusSlave ignorant of project-specific configuration, allowing the protocol stack to remain a reusable middleware component.

### 8.7 Pull-based access

When a producer feeds multiple consumers, prefer a pull interface (consumers query for data) over push (producer notifies each consumer). Producers stay unaware of consumers.

SensorService exposes `ISensorService.get_latest()`. LcdUi, ModbusRegisterMap, ConsoleService, AlarmService all consume the same interface. SensorService adds or removes nothing when consumers change.

The exception is when a consumer's responsiveness requirement is tighter than the producer's natural rate — then event subscription (Observer) applies, as in the SensorService → AlarmService relationship.

### 8.8 Store-and-Forward

Telemetry destined for an unreliable cloud must survive connectivity loss. CloudPublisher routes payloads to MqttClient when online; when offline, it diverts to StoreAndForward, which persists records in a circular log over flash. On reconnection, the log is drained in chronological order.

This is the standard industrial-IoT resilience pattern. AWS IoT Greengrass, Azure IoT Edge, Siemens MindSphere, and HMS Networks Anybus all use this exact structure.

### 8.9 State machines as behavioural backbone

Per Section 7, six explicit state machines own the substantive lifecycle logic in the system. The pattern is consistent: each machine has exactly one owner component (visible in `components.md`), each state and transition traces to ≥ 1 SRS requirement or use case, and behavioural detail (entry / do / exit, internal transitions) is specified textually in `state-machines.md` rather than crammed onto the diagram.

The pattern of a single `LifecycleController` Application component owning each board's top-level lifecycle is deliberate: lifecycle is a coordination concern distinct from the functional decomposition of Sections 5 and 6, and an explicit owner makes it traceable in the same way SensorService is the owner of sensor acquisition. Without an explicit owner, the lifecycle would be implicit in `main.c` startup code — defensible but harder to defend in interview review.

---

## 9. Hardware abstraction strategy

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

The Modbus protocol stack on both boards is implemented from scratch, not wrapped from FreeMODBUS or a vendor SDK. This decision is recorded in §11.1 with rationale.

---

## 10. Forward-looking artefacts

This document will be extended as the remaining HLD artefacts are completed.

### 10.1 Sequence diagrams (Phase 2.5)

End-to-end interaction diagrams for representative use cases. Candidates: UC-07 (acquire sensor data), UC-08 (evaluate alarms), UC-09/10 (publish telemetry, with offline buffering), UC-13 (time synchronisation), UC-18 (firmware update).

These sequence diagrams will also serve as a verification pass on the component spec — if a sequence cannot be drawn cleanly with the existing component set, that exposes a gap in the HLD which feeds back into a spec update.

### 10.2 FreeRTOS task breakdown (Phase 2.6)

Per-board task list with priorities, stack sizes, and inter-task communication mechanisms (queues, mutexes, semaphores). Mapping from components to FreeRTOS tasks is many-to-one in some cases (one task hosts multiple application components) and one-to-one in others.

### 10.3 Modbus register map (Phase 2.7)

The complete register table exposed by ModbusRegisterMap: address ranges, data types, read/write semantics, scaling factors. Source of truth for the field-device-to-gateway protocol.

### 10.4 Flash partition layout (Phase 2.8)

Memory map of the QSPI flash on each board, partitioned across firmware slots, configuration storage, circular log, and any reserved regions. Drives FirmwareStore, ConfigStore, and CircularFlashLog implementations.

---

## 11. Architectural decisions log

The following decisions were considered and resolved during the components and state machines phases. They are recorded here so an interview reviewer can see what was rejected as well as what was adopted.

### 11.1 Decisions adopted

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
| `LifecycleController` introduced as Application component owning each board's top-level lifecycle | Lifecycle is a coordination concern distinct from functional decomposition; explicit owner makes the state machines traceable to a real component rather than implicit in `main.c` |
| No "Degraded" top-level state on the gateway lifecycle | REQ-NF-200 mandates that cloud loss does not change the gateway's top-level mode; cloud-down behaviour is owned by the Cloud Connectivity sub-machine. Promoting Degraded would duplicate sub-machine logic |
| `EditingConfig` as a top-level state on both boards | Distinct exit timeout, snapshot/rollback semantics, and cross-machine credential-change events justify top-level promotion over Operational sub-state |
| Modbus link-state hysteresis as model variable + transition actions, not separate states | Avoids an HSM with Online/Offline composites that would duplicate the polling cycle visually |
| Reboots in Firmware Update are transition actions, not states | The machine resumes via persisted flags detected by gateway-lifecycle Init; entry-point pseudo-state + choice diamond render this on the diagram |
| State machine diagrams show structural transitions only; behavioural compartments live in `state-machines.md` | Keeps diagrams readable; companion document is the authoritative behavioural specification |
| Custom Modbus RTU implementation, not FreeMODBUS | Tightly scoped to project needs (function codes 03/04/06/16; minimal exception responses). Demonstrates protocol-level competence and respects the no-HAL-above-driver portability stance. Decision can still flip to FreeMODBUS after LLD design pass without losing the design work |

### 11.2 Decisions rejected

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
| Per-channel alarm state machine at HLD level | Per-instance rather than per-system; trivial states (Clear, Active) with single guarded transition pair; deferred to LLD alongside per-channel data structures |
| Modbus Master HSM with Online/Offline as composite states each containing the polling cycle | Visually duplicative of the polling cycle with no behavioural difference inside each composite; replaced with model-variable + transition-action hysteresis |
| `Degraded` top-level state on the gateway lifecycle | Cloud loss is owned by the Cloud Connectivity sub-machine; surfacing it at gateway top level would duplicate logic and contradict REQ-NF-200 |

---

*This document is the master HLD. It is updated as each artefact is completed. Detailed specifications live in the companion files referenced in §1.1.*
