# HLD Artefact #3 — Component Specification

This document specifies the software components of both nodes of the IoT Environmental Monitoring Gateway system. It is the textual companion to the component diagrams produced in Visual Paradigm under `02-architecture/components/`.

The document is organised in four sections:

1. **Bottom-up sweep** — hardware enumeration, one driver per peripheral.
2. **Top-down sweep** — use case mapping, one responsibility per application component.
3. **Final component list** — grouped by layer.
4. **Responsibility sentences and interfaces** — the contract each component honours.

## Naming conventions

- **Components:** PascalCase, acronyms treated as words (`UartDriver`, not `UARTDriver`).
- **Interfaces:** `IXxx` prefix, capability-oriented names (`IBarometer`, not `BarometerIF`).
- **Hardware-layer boundary:** `CMSIS` denotes direct register access via CMSIS device headers, per Vision §9 (vendor-neutral driver interfaces, CMSIS-only driver internals).

## Architectural patterns

### Interface Segregation (ISP)

Where one component is consumed by readers and writers with different needs, the component exposes two narrow interfaces rather than a single fat one.

- **`ConfigService`** provides:
  - `IConfigManager` — write/update operations, consumed by administrative components (LcdUi, ConsoleService, ModbusRegisterMap).
  - `IConfigProvider` — read-only access to current parameter values, consumed by functional components (SensorService, AlarmService, ModbusRegisterMap).
- **`HealthMonitor`** provides:
  - `IHealthSnapshot` — read-only access to the consolidated health snapshot, consumed by display/output components (LcdUi, ConsoleService; CloudPublisher on the Gateway).
  - `IHealthReport` — write-side reporting interface, consumed by metric producers across Application and Middleware layers.

### Dependency Inversion (DIP) for cross-layer reporting

Middleware components must not depend on Application-layer components. To allow Middleware to report health metrics without breaking layering, the `IHealthReport` interface is owned by `HealthMonitor` (Application) but consumed by Middleware producers as an abstraction. Middleware depends on the abstraction, not on `HealthMonitor`'s implementation.

The same principle applies to `ILogger`: Logger lives in Middleware, but its abstraction is consumed across Application and Middleware layers.

### Metric Producer Pattern

Two mechanisms are used for health metric flow, chosen by metric type:

- **Stats polling (`IXxxStats`)** — for accumulated counters (CRC error counts, transaction counts, reconnection counts). Middleware exposes the stats interface; an Application-layer consumer polls and reports through `IHealthReport`.
- **Direct push (`IHealthReport`)** — for event-based signals (sync state change, configuration write failure, critical error). The producer depends on `IHealthReport` and pushes when the event occurs.

Apply the test: *is this a counter I want to see at any sample time, or an event I need to report when it happens?* Counters → poll. Events → push.

Current assignments — Field Device:
- `ModbusSlave` exposes `IModbusSlaveStats` — polled by `ModbusRegisterMap`.
- `ConfigStore` pushes via `IHealthReport` — reports write/read failures as events.
- `TimeProvider` pushes via `IHealthReport` — reports sync state transitions as events.

Current assignments — Gateway:
- `ModbusMaster` exposes `IModbusMasterStats` — polled by `ModbusPoller`.
- `MqttClient` exposes `IMqttStats` — polled by `CloudPublisher`.
- `ConfigStore`, `TimeProvider`, `NtpClient`, `CircularFlashLog` push via `IHealthReport` — event-based.

### Cross-cutting components

- **Logger** is consumed by every Application and Middleware component for diagnostic output (REQ-NF-500). Connections are listed in this spec but elided from the component diagrams to keep them readable; a UML note on the Diagnostic and Traceability view records the convention. Logger depends directly on `RtcDriver` (not `TimeProvider`) as a documented bootstrap exception — this avoids a circular dependency, since `TimeProvider` itself logs through Logger.
- **HealthMonitor** is bidirectional through ISP-split interfaces. Producers across Application and Middleware layers push metrics through `IHealthReport`. Consumers (LcdUi, ConsoleService; CloudPublisher on the Gateway) read the consolidated snapshot through `IHealthSnapshot`. HealthMonitor also drives the on-board LEDs to indicate device status.

### Layering rule

Each layer depends only on the layers below it. Drivers never depend on Middleware or Application components. Middleware never depends on Application components — when bottom-up data flow is required (health reporting), DIP is applied. Driver-layer state (for example WiFi RSSI, sensor error counters) is queried by the Middleware consumer and pushed upward through the appropriate stats or report interface.

### Behavioural patterns

- **Observer (event-driven):** `SensorService` emits new-reading events; `AlarmService` subscribes. Satisfies REQ-NF-101 (one-polling-cycle alarm detection) without polling.
- **Pull-based access:** `SensorService` exposes `ISensorService.get_latest()` for consumers that only need current values. Consumers do not depend on each other, only on `SensorService`.
- **Mediator:** `ModbusRegisterMap` mediates between `ConfigService` (configuration source) and `ModbusSlave` (protocol stack), keeping the protocol stack reusable and free of project-specific configuration logic.
- **Blackboard (LLD level):** latest readings inside `SensorService` are shared state protected by mutex; readers read whatever is currently posted, without blocking for new data.

---

# Field Device

## 1. Bottom-up sweep (hardware → drivers)

### Hardware used

| Peripheral / module                        | Interface                  |
|--------------------------------------------|----------------------------|
| USB Mini-B ST-Link virtual COM port        | UART1 (debug console)      |
| Simulated barometer                        | Software simulation        |
| Simulated humidity and temperature sensor  | Software simulation        |
| 4-inch DSI LCD                             | MIPI DSI                   |
| LCD touchscreen controller                 | I2C1                       |
| 128 Mbit SDRAM                             | FMC                        |
| 128 Mbit QSPI NOR flash                    | QSPI                       |
| RS-485 transceiver for Modbus              | UARTx (RS-485 half-duplex) |
| On-board LEDs                              | GPIO                       |
| RTC (backup-domain)                        | RTC peripheral             |

### Driver components derived

- DebugUartDriver
- BarometerDriver *(simulated — no downward hardware dependency)*
- HumidityTempDriver *(simulated — no downward hardware dependency)*
- LcdDriver
- TouchscreenDriver
- SdramDriver
- QspiFlashDriver
- ModbusUartDriver
- LedDriver
- RtcDriver
- I2cDriver
- GpioDriver

## 2. Top-down sweep (use cases → application components)

| Component         | Use cases covered                                     |
|-------------------|-------------------------------------------------------|
| LcdUi             | UC-01, UC-02, UC-03, UC-08 *(display)*, UC-15         |
| HealthMonitor     | UC-02, UC-04, UC-06                                   |
| SensorService     | UC-07                                                 |
| AlarmService      | UC-08                                                 |
| ConsoleService    | UC-04, UC-16                                          |
| ConfigStore       | UC-15, UC-16 *(persistence tier)*                     |
| ModbusRegisterMap | UC-06, UC-07, UC-13, UC-15                            |
| ConfigService     | UC-15, UC-16                                          |

## 3. Final component list

### Application layer
- LcdUi
- HealthMonitor
- SensorService
- AlarmService
- ConsoleService
- ModbusRegisterMap
- ConfigService

### Middleware layer
- Logger
- TimeProvider
- ConfigStore
- ModbusSlave
- GraphicsLibrary

### Driver layer
- DebugUartDriver
- BarometerDriver
- HumidityTempDriver
- LcdDriver
- TouchscreenDriver
- SdramDriver
- QspiFlashDriver
- ModbusUartDriver
- LedDriver
- RtcDriver
- I2cDriver
- GpioDriver

## 4. Responsibility sentences and interfaces — Field Device

### Driver layer

**NAME:** DebugUartDriver
**LAYER:** Driver
**RESPONSIBILITY:** Sends and receives byte streams over the UART peripheral connected to the debug console.
**PROVIDES (upward):** IDebugUart
**USES (downward):** CMSIS

**NAME:** BarometerDriver
**LAYER:** Driver
**RESPONSIBILITY:** Provides ambient atmospheric pressure readings to the layer above. *(Simulated on this node per Vision §5.1.1.)*
**PROVIDES (upward):** IBarometer
**USES (downward):** *(none — simulation internal to the driver)*

**NAME:** HumidityTempDriver
**LAYER:** Driver
**RESPONSIBILITY:** Provides ambient humidity and temperature readings to the layer above. *(Simulated on this node per Vision §5.1.1.)*
**PROVIDES (upward):** IHumidityTemp
**USES (downward):** *(none — simulation internal to the driver)*

**NAME:** LcdDriver
**LAYER:** Driver
**RESPONSIBILITY:** Renders application-supplied frames to the LCD, owning the framebuffer located in external SDRAM.
**PROVIDES (upward):** ILcd
**USES (downward):** SdramDriver, CMSIS

**NAME:** TouchscreenDriver
**LAYER:** Driver
**RESPONSIBILITY:** Reads touch coordinate events from the touchscreen controller.
**PROVIDES (upward):** ITouchscreen
**USES (downward):** I2cDriver

**NAME:** SdramDriver
**LAYER:** Driver
**RESPONSIBILITY:** Initialises the FMC controller and exposes the external SDRAM as memory-mapped space.
**PROVIDES (upward):** ISdram
**USES (downward):** CMSIS

**NAME:** QspiFlashDriver
**LAYER:** Driver
**RESPONSIBILITY:** Reads, writes, and erases sectors of the external QSPI flash memory.
**PROVIDES (upward):** IQspiFlash
**USES (downward):** CMSIS

**NAME:** ModbusUartDriver
**LAYER:** Driver
**RESPONSIBILITY:** Sends and receives byte streams over the UART peripheral configured for the Modbus RS-485 link.
**PROVIDES (upward):** IModbusUart
**USES (downward):** CMSIS

**NAME:** LedDriver
**LAYER:** Driver
**RESPONSIBILITY:** Controls the on/off state of the board LEDs.
**PROVIDES (upward):** ILed
**USES (downward):** GpioDriver

**NAME:** RtcDriver
**LAYER:** Driver
**RESPONSIBILITY:** Keeps wall-clock time across reboots using the backup-domain RTC.
**PROVIDES (upward):** IRtc
**USES (downward):** CMSIS

**NAME:** I2cDriver
**LAYER:** Driver
**RESPONSIBILITY:** Serialises I2C bus transactions across multiple sensor drivers.
**PROVIDES (upward):** II2c
**USES (downward):** CMSIS

**NAME:** GpioDriver
**LAYER:** Driver
**RESPONSIBILITY:** Configures GPIO pins and provides read/write access to single-pin digital I/O.
**PROVIDES (upward):** IGpio
**USES (downward):** CMSIS

### Middleware layer

**NAME:** Logger
**LAYER:** Middleware
**RESPONSIBILITY:** Formats severity-tagged log entries with timestamps and source module identifiers, and dispatches them to the configured output sinks.
**PROVIDES (upward):** ILogger
**USES (downward):** DebugUartDriver, RtcDriver *(bootstrap exception — see preamble)*

**NAME:** TimeProvider
**LAYER:** Middleware
**RESPONSIBILITY:** Provides wall-clock time readings with a synchronisation-state flag per Vision §8. Falls back to uptime-based timestamps when the RTC has not been synchronised. Reports sync state transitions as events.
**PROVIDES (upward):** ITimeProvider
**USES (downward):** RtcDriver, IHealthReport, ILogger

**NAME:** ConfigStore
**LAYER:** Middleware
**RESPONSIBILITY:** Persists configuration key-value pairs across reboots and retrieves them on request. Reports persistence failures as events.
**PROVIDES (upward):** IConfigStore
**USES (downward):** QspiFlashDriver, IHealthReport, ILogger

**NAME:** ModbusSlave
**LAYER:** Middleware
**RESPONSIBILITY:** Implements the Modbus RTU slave protocol — frame parsing, CRC validation, function code dispatch, response framing. Maintains per-transaction reliability counters exposed via stats interface.
**PROVIDES (upward):** IModbusSlave, IModbusSlaveStats
**USES (downward):** ModbusUartDriver, ILogger

**NAME:** GraphicsLibrary
**LAYER:** Middleware
**RESPONSIBILITY:** Renders widgets to the framebuffer and dispatches touch input events to registered widgets (LVGL).
**PROVIDES (upward):** IGraphics
**USES (downward):** LcdDriver, TouchscreenDriver, ILogger

### Application layer

**NAME:** LcdUi
**LAYER:** Application
**RESPONSIBILITY:** Renders LCD screens and dispatches touchscreen input to the appropriate screen handler. Consumes sensor, alarm, and health data for display; initiates configuration changes via ConfigService.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** GraphicsLibrary, ISensorService, IAlarmService, IConfigManager, IHealthSnapshot, ILogger

**NAME:** HealthMonitor
**LAYER:** Application
**RESPONSIBILITY:** Aggregates health metrics pushed by producer components throughout the system, maintains a consolidated health snapshot, serves the snapshot to consumers (LCD, CLI), and drives the on-board LEDs to indicate device status (idle, acquiring, alarm, error).
**PROVIDES (upward):** IHealthSnapshot *(read-side)*, IHealthReport *(write-side)*
**USES (downward):** LedDriver, ILogger

**NAME:** SensorService
**LAYER:** Application
**RESPONSIBILITY:** Periodically acquires sensor data, validates values against physical sensor ranges (REQ-SA-120), applies signal conditioning and low-pass filtering (REQ-SA-130, REQ-SA-140), exposes the latest validated readings to consumers, and emits new-reading events to subscribers. Reports sensor failure events via IHealthReport.
**PROVIDES (upward):** ISensorService *(latest readings + new-reading event subscription)*
**USES (downward):** BarometerDriver, HumidityTempDriver, TimeProvider, IConfigProvider, IHealthReport, ILogger

**NAME:** AlarmService
**LAYER:** Application
**RESPONSIBILITY:** Subscribes to SensorService new-reading events, compares each reading against configured thresholds, applies hysteresis when clearing alarms, maintains per-sensor alarm state, and notifies subscribers when alarms are raised or cleared.
**PROVIDES (upward):** IAlarmService *(active alarm query + alarm event subscription)*
**USES (downward):** ISensorService, IConfigProvider, ILogger

**NAME:** ConsoleService
**LAYER:** Application
**RESPONSIBILITY:** Provides a local console for provisioning and diagnostic, exposing sensor, configuration, and health data through CLI commands.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** DebugUartDriver, ISensorService, IConfigManager, IHealthSnapshot, ILogger

**NAME:** ModbusRegisterMap
**LAYER:** Application
**RESPONSIBILITY:** Binds Modbus register addresses to live data sources (sensor readings, alarm state, time, configuration values), serving register reads and dispatching register writes to the appropriate handler. Mediates between ConfigService and ModbusSlave for protocol configuration. Polls IModbusSlaveStats and reports Modbus protocol metrics via IHealthReport. Exposes IHealthSnapshot data via Modbus registers so the Gateway can aggregate field-device health.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** ModbusSlave, ISensorService, IAlarmService, IConfigProvider, IConfigManager, IHealthSnapshot, IHealthReport, TimeProvider, ILogger

**NAME:** ConfigService
**LAYER:** Application
**RESPONSIBILITY:** Validates and applies operational and provisioning parameter changes, coordinating with ConfigStore for persistence. Provides ISP-split interfaces for read and write access.
**PROVIDES (upward):** IConfigManager *(write-side)*, IConfigProvider *(read-side)*
**USES (downward):** ConfigStore, ILogger

---

# Gateway

## 1. Bottom-up sweep (hardware → drivers)

### Hardware used

| Peripheral / module                          | Interface                  |
|----------------------------------------------|----------------------------|
| USB Micro-B ST-Link virtual COM port         | UART1 (debug console)      |
| WiFi module Inventek ISM43362-M3G-L44        | SPI3 + GPIO *(per CON-001)*|
| Magnetometer LIS3MDL                         | I2C2 + GPIO                |
| IMU LSM6DSL (accelerometer + gyroscope)      | I2C2 + GPIO                |
| Barometer LPS22HB                            | I2C2 + GPIO                |
| Humidity and Temperature HTS221              | I2C2 + GPIO                |
| 64 Mbit QSPI NOR flash MX25R6435F            | QSPI                       |
| RS-485 transceiver for Modbus                | UARTx (RS-485 half-duplex) |
| On-board LEDs                                | GPIO                       |
| RTC (backup-domain)                          | RTC peripheral             |
| System reset                                 | NVIC software reset        |

### Driver components derived

- DebugUartDriver
- WifiDriver
- MagnetometerDriver
- ImuDriver *(combined accelerometer + gyroscope per LSM6DSL datasheet)*
- BarometerDriver
- HumidityTempDriver
- QspiFlashDriver
- ModbusUartDriver
- LedDriver
- RtcDriver
- ResetDriver
- I2cDriver
- SpiDriver
- GpioDriver

## 2. Top-down sweep (use cases → application components)

| Component        | Use cases covered                                                       |
|------------------|-------------------------------------------------------------------------|
| HealthMonitor    | UC-04, UC-06                                                            |
| SensorService    | UC-07                                                                   |
| AlarmService     | UC-08 *(gateway sensors only — field-device alarms arrive pre-evaluated via Modbus)* |
| CloudPublisher   | UC-05, UC-09, UC-10, UC-11, UC-12, UC-14, UC-17, UC-18, UC-19           |
| TimeService      | UC-13                                                                   |
| ConsoleService   | UC-04, UC-16                                                            |
| UpdateService    | UC-18, UC-20                                                            |
| ConfigStore      | UC-15, UC-16 *(persistence tier)*                                       |
| ModbusPoller     | UC-07, UC-13, UC-14, UC-15, UC-19                                       |
| ConfigService    | UC-15, UC-16                                                            |
| StoreAndForward  | UC-09, UC-10, UC-11, UC-12, UC-14 *(exception flows)*                   |

## 3. Final component list

### Application layer
- HealthMonitor
- SensorService
- AlarmService
- CloudPublisher
- TimeService
- UpdateService
- ConsoleService
- ModbusPoller
- ConfigService
- StoreAndForward

### Middleware layer
- Logger
- TimeProvider
- MqttClient
- ModbusMaster
- CircularFlashLog
- NtpClient
- ConfigStore

### Driver layer
- DebugUartDriver
- WifiDriver
- MagnetometerDriver
- ImuDriver
- BarometerDriver
- HumidityTempDriver
- QspiFlashDriver
- ModbusUartDriver
- LedDriver
- RtcDriver
- ResetDriver
- I2cDriver
- SpiDriver
- GpioDriver

## 4. Responsibility sentences and interfaces — Gateway

### Driver layer

**NAME:** DebugUartDriver
**LAYER:** Driver
**RESPONSIBILITY:** Sends and receives byte streams over the UART peripheral connected to the debug console.
**PROVIDES (upward):** IDebugUart
**USES (downward):** CMSIS

**NAME:** WifiDriver
**LAYER:** Driver
**RESPONSIBILITY:** Sends and receives data between the MCU and the external WiFi module via AT commands. Exposes link-level state (RSSI, connection status) to its consumer.
**PROVIDES (upward):** IWifi
**USES (downward):** SpiDriver, GpioDriver

**NAME:** MagnetometerDriver
**LAYER:** Driver
**RESPONSIBILITY:** Provides 3-axis magnetic field readings to the layer above.
**PROVIDES (upward):** IMagnetometer
**USES (downward):** I2cDriver, GpioDriver

**NAME:** ImuDriver
**LAYER:** Driver
**RESPONSIBILITY:** Provides 3-axis accelerometer and 3-axis gyroscope readings to the layer above.
**PROVIDES (upward):** IImu
**USES (downward):** I2cDriver, GpioDriver

**NAME:** BarometerDriver
**LAYER:** Driver
**RESPONSIBILITY:** Provides ambient atmospheric pressure readings to the layer above.
**PROVIDES (upward):** IBarometer
**USES (downward):** I2cDriver, GpioDriver

**NAME:** HumidityTempDriver
**LAYER:** Driver
**RESPONSIBILITY:** Provides ambient humidity and temperature readings to the layer above.
**PROVIDES (upward):** IHumidityTemp
**USES (downward):** I2cDriver, GpioDriver

**NAME:** QspiFlashDriver
**LAYER:** Driver
**RESPONSIBILITY:** Reads, writes, and erases sectors of the external QSPI flash memory.
**PROVIDES (upward):** IQspiFlash
**USES (downward):** CMSIS

**NAME:** ModbusUartDriver
**LAYER:** Driver
**RESPONSIBILITY:** Sends and receives byte streams over the UART peripheral configured for the Modbus RS-485 link.
**PROVIDES (upward):** IModbusUart
**USES (downward):** CMSIS

**NAME:** LedDriver
**LAYER:** Driver
**RESPONSIBILITY:** Controls the on/off state of the board LEDs.
**PROVIDES (upward):** ILed
**USES (downward):** GpioDriver

**NAME:** RtcDriver
**LAYER:** Driver
**RESPONSIBILITY:** Keeps wall-clock time across reboots using the backup-domain RTC.
**PROVIDES (upward):** IRtc
**USES (downward):** CMSIS

**NAME:** ResetDriver
**LAYER:** Driver
**RESPONSIBILITY:** Triggers a full software reset of the MCU.
**PROVIDES (upward):** IReset
**USES (downward):** CMSIS

**NAME:** I2cDriver
**LAYER:** Driver
**RESPONSIBILITY:** Serialises I2C bus transactions across multiple sensor drivers.
**PROVIDES (upward):** II2c
**USES (downward):** CMSIS

**NAME:** SpiDriver
**LAYER:** Driver
**RESPONSIBILITY:** Transfers data between the MCU and SPI-connected peripherals.
**PROVIDES (upward):** ISpi
**USES (downward):** CMSIS

**NAME:** GpioDriver
**LAYER:** Driver
**RESPONSIBILITY:** Configures GPIO pins and provides read/write access to single-pin digital I/O.
**PROVIDES (upward):** IGpio
**USES (downward):** CMSIS

### Middleware layer

**NAME:** Logger
**LAYER:** Middleware
**RESPONSIBILITY:** Formats severity-tagged log entries with timestamps and source module identifiers, and dispatches them to the configured output sinks.
**PROVIDES (upward):** ILogger
**USES (downward):** DebugUartDriver, RtcDriver *(bootstrap exception — see preamble)*

**NAME:** TimeProvider
**LAYER:** Middleware
**RESPONSIBILITY:** Provides wall-clock time readings with a synchronisation-state flag per Vision §8. Falls back to uptime-based timestamps when the RTC has not been synchronised. Reports sync state transitions as events.
**PROVIDES (upward):** ITimeProvider
**USES (downward):** RtcDriver, IHealthReport, ILogger

**NAME:** MqttClient
**LAYER:** Middleware
**RESPONSIBILITY:** Implements the MQTT client protocol over a TLS-secured connection. Maintains connection state and publish/subscribe reliability counters exposed via stats interface.
**PROVIDES (upward):** IMqttClient, IMqttStats
**USES (downward):** WifiDriver, ILogger

**NAME:** ModbusMaster
**LAYER:** Middleware
**RESPONSIBILITY:** Implements the Modbus RTU master protocol — request framing, transmission, response parsing, timeout handling. Maintains per-transaction reliability counters exposed via stats interface.
**PROVIDES (upward):** IModbusMaster, IModbusMasterStats
**USES (downward):** ModbusUartDriver, ILogger

**NAME:** CircularFlashLog
**LAYER:** Middleware
**RESPONSIBILITY:** Implements a circular append-and-consume log over flash sectors, preserving chronological order and overwriting oldest records when full. Reports buffer overflow events.
**PROVIDES (upward):** ICircularFlashLog
**USES (downward):** QspiFlashDriver, IHealthReport, ILogger

**NAME:** NtpClient
**LAYER:** Middleware
**RESPONSIBILITY:** Queries NTP servers and returns time references. Reports NTP query failure events.
**PROVIDES (upward):** INtpClient
**USES (downward):** WifiDriver, IHealthReport, ILogger

**NAME:** ConfigStore
**LAYER:** Middleware
**RESPONSIBILITY:** Persists configuration key-value pairs across reboots and retrieves them on request. Reports persistence failures as events.
**PROVIDES (upward):** IConfigStore
**USES (downward):** QspiFlashDriver, IHealthReport, ILogger

### Application layer

**NAME:** HealthMonitor
**LAYER:** Application
**RESPONSIBILITY:** Aggregates health metrics pushed by producer components throughout the system, maintains a consolidated health snapshot, serves the snapshot to consumers (CLI, cloud), and drives the on-board LEDs to indicate device status (idle, acquiring, alarm, error).
**PROVIDES (upward):** IHealthSnapshot *(read-side)*, IHealthReport *(write-side)*
**USES (downward):** LedDriver, ILogger

**NAME:** SensorService
**LAYER:** Application
**RESPONSIBILITY:** Periodically acquires sensor data, validates values against physical sensor ranges (REQ-SA-120), applies signal conditioning and low-pass filtering (REQ-SA-130, REQ-SA-140), exposes the latest validated readings to consumers, and emits new-reading events to subscribers. Reports sensor failure events via IHealthReport.
**PROVIDES (upward):** ISensorService *(latest readings + new-reading event subscription)*
**USES (downward):** MagnetometerDriver, ImuDriver, BarometerDriver, HumidityTempDriver, TimeProvider, IConfigProvider, IHealthReport, ILogger

**NAME:** AlarmService
**LAYER:** Application
**RESPONSIBILITY:** Subscribes to SensorService new-reading events, compares each reading against configured thresholds, applies hysteresis when clearing alarms, maintains per-sensor alarm state, and notifies subscribers when alarms are raised or cleared. Field device alarms arrive pre-evaluated via Modbus and are not re-evaluated here.
**PROVIDES (upward):** IAlarmService *(active alarm query + alarm event subscription)*
**USES (downward):** ISensorService, IConfigProvider, ILogger

**NAME:** CloudPublisher
**LAYER:** Application
**RESPONSIBILITY:** Serialises and deserialises MQTT payloads according to the project's message schemas, publishes telemetry, alarm and health messages, and routes inbound commands to the appropriate handler. Polls IMqttStats and reports connectivity metrics via IHealthReport.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** MqttClient, ISensorService, IAlarmService, IModbusPoller, StoreAndForward, IHealthSnapshot, IHealthReport, ILogger

**NAME:** TimeService
**LAYER:** Application
**RESPONSIBILITY:** Synchronises the local RTC with an external NTP source on boot and periodically thereafter, then writes the current time to the field device via Modbus.
**PROVIDES (upward):** ITimeService
**USES (downward):** TimeProvider, NtpClient, IModbusPoller, ILogger

**NAME:** UpdateService
**LAYER:** Application
**RESPONSIBILITY:** Orchestrates the firmware update flow: download, verify, partition switch, reboot.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** MqttClient, QspiFlashDriver, ResetDriver, ILogger

**NAME:** ConsoleService
**LAYER:** Application
**RESPONSIBILITY:** Provides a local console for provisioning and diagnostic, exposing sensor, configuration, and health data through CLI commands.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** DebugUartDriver, ISensorService, IConfigManager, IHealthSnapshot, ILogger

**NAME:** ModbusPoller
**LAYER:** Application
**RESPONSIBILITY:** Polls configured slave devices on schedule, tracks per-slave register state transitions to detect new events (such as field device alarms), routes Modbus transactions on behalf of higher-layer components, and tracks per-slave error statistics. Polls IModbusMasterStats and reports Modbus protocol metrics via IHealthReport.
**PROVIDES (upward):** IModbusPoller *(transaction routing + state-change event subscription)*
**USES (downward):** ModbusMaster, IHealthReport, ILogger

**NAME:** ConfigService
**LAYER:** Application
**RESPONSIBILITY:** Validates and applies operational and provisioning parameter changes, coordinating with ConfigStore for persistence. Provides ISP-split interfaces for read and write access.
**PROVIDES (upward):** IConfigManager *(write-side)*, IConfigProvider *(read-side)*
**USES (downward):** ConfigStore, ILogger

**NAME:** StoreAndForward
**LAYER:** Application
**RESPONSIBILITY:** Stores telemetry in flash when the cloud is unreachable, and forwards it in chronological order when connectivity is restored. Reports buffer occupancy via IHealthReport.
**PROVIDES (upward):** IStoreAndForward
**USES (downward):** CircularFlashLog, IHealthReport, ILogger