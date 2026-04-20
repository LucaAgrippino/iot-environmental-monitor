# HLD Artefact #3 — Component Specification

This document specifies the software components of both nodes of the IoT Environmental Monitoring Gateway system. It is the textual companion to the two component diagrams produced in Visual Paradigm (`Field Device — Component Diagram` and `Gateway — Component Diagram`).

The document is organised in four sections:

1. **Bottom-up sweep** — hardware enumeration, one driver per peripheral.
2. **Top-down sweep** — use case mapping, one responsibility per application component.
3. **Final component list** — grouped by layer.
4. **Responsibility sentences and interfaces** — the contract each component honours.

Naming conventions:

- **Components:** PascalCase, acronyms treated as words (`UartDriver`, not `UARTDriver`).
- **Interfaces:** `IXxx` prefix, capability-oriented names (`IBarometer`, not `BarometerIF`).
- **Hardware-layer boundary:** `CMSIS` denotes direct register access via CMSIS device headers, per Vision §9 (vendor-neutral driver interfaces, CMSIS-only driver internals).

Cross-cutting components:

- **Logger** is consumed by every Application and Middleware component for diagnostic output (REQ-NF-500). Connections are listed in the spec but elided from the diagrams to keep them readable; a UML note on each diagram records the convention.
- **HealthMonitor** is a passive collector. Producers of health metrics push to its interface; no downward dependencies are shown in the diagram.

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
| HealthMonitor     | UC-06                                                 |
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
- ConfigStore
- ModbusSlave

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
**USES (downward):** DebugUartDriver, RtcDriver

**NAME:** ConfigStore
**LAYER:** Middleware
**RESPONSIBILITY:** Persists configuration key-value pairs across reboots and retrieves them on request.
**PROVIDES (upward):** IConfigStore
**USES (downward):** QspiFlashDriver

**NAME:** ModbusSlave
**LAYER:** Middleware
**RESPONSIBILITY:** Implements the Modbus RTU slave protocol — frame parsing, CRC validation, function code dispatch, response framing.
**PROVIDES (upward):** IModbusSlave
**USES (downward):** ModbusUartDriver, HealthMonitor

### Application layer

**NAME:** LcdUi
**LAYER:** Application
**RESPONSIBILITY:** Renders LCD screens and dispatches touchscreen input to the appropriate screen handler.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** LcdDriver, TouchscreenDriver, SensorService, AlarmService, ConfigService, Logger, HealthMonitor

**NAME:** HealthMonitor
**LAYER:** Application
**RESPONSIBILITY:** Aggregates health metrics pushed by producer components throughout the system, maintains a consolidated health snapshot, serves the snapshot to consumers (LCD, CLI, cloud), and drives the on-board LEDs to indicate device status (idle, acquiring, alarm, error).
**PROVIDES (upward):** IHealthMonitor *(other components push metrics to this interface)*
**USES (downward):** LedDriver, Logger

**NAME:** SensorService
**LAYER:** Application
**RESPONSIBILITY:** Periodically acquires and processes sensor data, and exposes the latest validated readings to consumers.
**PROVIDES (upward):** ISensorService *(latest readings + new-reading event subscription)*
**USES (downward):** BarometerDriver, HumidityTempDriver, RtcDriver, Logger, HealthMonitor

**NAME:** AlarmService
**LAYER:** Application
**RESPONSIBILITY:** Compares each new sensor reading against configured thresholds, applies hysteresis when clearing alarms, maintains per-sensor alarm state, and notifies subscribers when alarms are raised or cleared.
**PROVIDES (upward):** IAlarmService *(active alarm query + alarm event subscription)*
**USES (downward):** SensorService, ConfigService, Logger

**NAME:** ConsoleService
**LAYER:** Application
**RESPONSIBILITY:** Provides a local console for provisioning and diagnostic.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** DebugUartDriver, SensorService, ConfigService, Logger, HealthMonitor

**NAME:** ModbusRegisterMap
**LAYER:** Application
**RESPONSIBILITY:** Binds Modbus register addresses to live data sources (sensor readings, alarm state, time, configuration values), serving register reads and dispatching register writes to the appropriate handler.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** ModbusSlave, SensorService, AlarmService, ConfigService, RtcDriver, Logger

**NAME:** ConfigService
**LAYER:** Application
**RESPONSIBILITY:** Validates and applies operational and provisioning parameter changes, coordinating with ConfigStore for persistence.
**PROVIDES (upward):** IConfigService
**USES (downward):** ConfigStore, Logger

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
| HealthMonitor    | UC-06                                                                   |
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
**RESPONSIBILITY:** Sends and receives data between the MCU and the external WiFi module via AT commands.
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
**USES (downward):** DebugUartDriver, RtcDriver

**NAME:** MqttClient
**LAYER:** Middleware
**RESPONSIBILITY:** Implements the MQTT client protocol over a TLS-secured connection.
**PROVIDES (upward):** IMqttClient
**USES (downward):** WifiDriver, HealthMonitor

**NAME:** ModbusMaster
**LAYER:** Middleware
**RESPONSIBILITY:** Implements the Modbus RTU master protocol — request framing, transmission, response parsing, timeout handling.
**PROVIDES (upward):** IModbusMaster
**USES (downward):** ModbusUartDriver, HealthMonitor

**NAME:** CircularFlashLog
**LAYER:** Middleware
**RESPONSIBILITY:** Implements a circular append-and-consume log over flash sectors, preserving chronological order and overwriting oldest records when full.
**PROVIDES (upward):** ICircularFlashLog
**USES (downward):** QspiFlashDriver

**NAME:** NtpClient
**LAYER:** Middleware
**RESPONSIBILITY:** Queries NTP servers and returns time references.
**PROVIDES (upward):** INtpClient
**USES (downward):** WifiDriver

**NAME:** ConfigStore
**LAYER:** Middleware
**RESPONSIBILITY:** Persists configuration key-value pairs across reboots and retrieves them on request.
**PROVIDES (upward):** IConfigStore
**USES (downward):** QspiFlashDriver

### Application layer

**NAME:** HealthMonitor
**LAYER:** Application
**RESPONSIBILITY:** Aggregates health metrics pushed by producer components throughout the system, maintains a consolidated health snapshot, serves the snapshot to consumers (LCD, CLI, cloud), and drives the on-board LEDs to indicate device status (idle, acquiring, alarm, error).
**PROVIDES (upward):** IHealthMonitor *(other components push metrics to this interface)*
**USES (downward):** LedDriver, Logger

**NAME:** SensorService
**LAYER:** Application
**RESPONSIBILITY:** Periodically acquires and processes sensor data, and exposes the latest validated readings to consumers.
**PROVIDES (upward):** ISensorService *(latest readings + new-reading event subscription)*
**USES (downward):** MagnetometerDriver, ImuDriver, BarometerDriver, HumidityTempDriver, RtcDriver, Logger, HealthMonitor

**NAME:** AlarmService
**LAYER:** Application
**RESPONSIBILITY:** Compares each new gateway sensor reading against configured thresholds, applies hysteresis when clearing alarms, maintains per-sensor alarm state, and notifies subscribers when alarms are raised or cleared. Field device alarms arrive pre-evaluated via Modbus and are not re-evaluated here.
**PROVIDES (upward):** IAlarmService
**USES (downward):** SensorService, ConfigService, Logger

**NAME:** CloudPublisher
**LAYER:** Application
**RESPONSIBILITY:** Serialises and deserialises MQTT payloads according to the project's message schemas, publishes telemetry, alarm and health messages, and routes inbound commands to the appropriate handler.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** MqttClient, SensorService, AlarmService, ModbusPoller, StoreAndForward, Logger, HealthMonitor

**NAME:** TimeService
**LAYER:** Application
**RESPONSIBILITY:** Synchronises the local RTC with an external NTP source on boot and periodically thereafter, then writes the current time to the field device via Modbus.
**PROVIDES (upward):** ITimeService
**USES (downward):** RtcDriver, NtpClient, ModbusPoller, Logger

**NAME:** UpdateService
**LAYER:** Application
**RESPONSIBILITY:** Orchestrates the firmware update flow: download, verify, partition switch, reboot.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** MqttClient, QspiFlashDriver, ResetDriver, Logger

**NAME:** ConsoleService
**LAYER:** Application
**RESPONSIBILITY:** Provides a local console for provisioning and diagnostic.
**PROVIDES (upward):** *(none — top of the stack)*
**USES (downward):** DebugUartDriver, SensorService, ConfigService, Logger, HealthMonitor

**NAME:** ModbusPoller
**LAYER:** Application
**RESPONSIBILITY:** Polls configured slave devices on schedule, tracks per-slave register state transitions to detect new events (such as field device alarms), routes Modbus transactions on behalf of higher-layer components, and tracks per-slave error statistics.
**PROVIDES (upward):** IModbusPoller *(transaction routing + state-change event subscription)*
**USES (downward):** ModbusMaster, Logger, HealthMonitor

**NAME:** ConfigService
**LAYER:** Application
**RESPONSIBILITY:** Validates and applies operational and provisioning parameter changes, coordinating with ConfigStore for persistence.
**PROVIDES (upward):** IConfigService
**USES (downward):** ConfigStore, Logger

**NAME:** StoreAndForward
**LAYER:** Application
**RESPONSIBILITY:** Stores telemetry in flash when the cloud is unreachable, and forwards it in chronological order when connectivity is restored.
**PROVIDES (upward):** IStoreAndForward
**USES (downward):** CircularFlashLog, Logger, HealthMonitor
