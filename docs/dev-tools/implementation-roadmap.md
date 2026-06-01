# Implementation Roadmap — Phase 4

**Status as of:** LedDriver v1.0 merged, HealthMonitor in progress
**Deadline:** 18 August 2026
**Layers 1–5 non-negotiable. Layers 6–7 cut first if behind schedule.**

---

## Completed

| # | Component | Board | Layer | PR |
|---|---|---|---|---|
| 1 | GpioDriver | FD | Driver | #24 |
| 2 | DebugUartDriver | FD | Driver | #25 |
| 3 | RtcDriver | FD | Driver | #26 |
| 4 | Logger | FD | Middleware | logger-v1.0 |
| 5 | LedDriver | FD + GW | Driver | led-driver-v1.0 |
| 6 | HealthMonitor | FD + GW | Application | in progress |

---

## Remaining — topological order

The ordering follows the boot-order dependency graph: a component only
appears after all its dependencies are done. Mandatory (Layers 1–5) and
optional (Layers 6–7) are separated.

---

### MANDATORY — Field Device

#### Layer 1 — Sensor Acquisition

| # | Component | Layer | USES | Notes |
|---|---|---|---|---|
| 7 | TimeProvider | Middleware | RtcDriver ✓, IHealthReport ✓, ILogger ✓ | First middleware with FreeRTOS task |
| 8 | I2cDriver | Driver | CMSIS | Needed by sensor drivers |
| 9 | BarometerDriver | Driver | *(simulation — no I2C)* | Simulated per Vision §5.1.1 |
| 10 | HumidityTempDriver | Driver | *(simulation — no I2C)* | Simulated per Vision §5.1.1 |
| 11 | SensorService | Application | IBarometer, IHumidityTemp, ITimeProvider, IHealthReport, ILogger | Hosts SensorTask (priority 5) |

#### Layer 2 — Processing / State Machines

| # | Component | Layer | USES | Notes |
|---|---|---|---|---|
| 12 | AlarmService | Application | ISensorService, IHealthReport, ILogger | Co-located in SensorTask |
| 13 | ConfigStore | Middleware | QspiFlashDriver, IHealthReport, ILogger | Needs QspiFlashDriver first |
| 14 | QspiFlashDriver | Driver | CMSIS | Dependency of ConfigStore |
| 15 | ConfigService | Application | IConfigStore, IConfigManager, IConfigProvider, ILogger | Hosts ConsoleTask part |

#### Layer 3 — Modbus RTU (Field Device Slave)

| # | Component | Layer | USES | Notes |
|---|---|---|---|---|
| 16 | ModbusUartDriver | Driver | CMSIS | UART for RS-485 |
| 17 | ModbusSlave | Middleware | IModbusUart, ILogger | Frame parse, CRC, FC dispatch |
| 18 | ModbusRegisterMap | Application | IModbusSlave, IConfigProvider, ISensorService, ILogger | Hosts ModbusSlaveTask (priority 4) |

#### Layer 4 — LCD Display

| # | Component | Layer | USES | Notes |
|---|---|---|---|---|
| 19 | SdramDriver | Driver | CMSIS | Framebuffer in external SDRAM |
| 20 | LcdDriver | Driver | SdramDriver, CMSIS | DSI LCD controller |
| 21 | TouchscreenDriver | Driver | I2cDriver ✓ | FT6206 touch controller |
| 22 | GraphicsLibrary | Middleware | ILcd, ITouchscreen, ILogger | LVGL wrapper |
| 23 | LcdUi | Application | IGraphics, ISensorService, IConfigProvider, IHealthSnapshot, ILogger | Hosts LcdUiTask (priority 2) |

#### Lifecycle and Console

| # | Component | Layer | USES | Notes |
|---|---|---|---|---|
| 24 | ConsoleService | Application | IDebugUart ✓, IConfigManager, IHealthSnapshot, ILogger | Hosts ConsoleTask (priority 1) |
| 25 | LifecycleController | Application | IConfigStore, ILed ✓, ILogger | Hosts LifecycleTask (priority 1) |

---

### MANDATORY — Gateway

The Gateway shares DebugUartDriver, RtcDriver, LedDriver, Logger,
HealthMonitor, TimeProvider, ConfigStore, ConfigService, ConsoleService,
and LifecycleController logic — L475 ports of these land alongside or
just after the FD versions.

#### Layer 1 — Sensor Acquisition (Gateway onboard sensors, real hardware)

| # | Component | Layer | USES | Notes |
|---|---|---|---|---|
| G1 | I2cDriver | Driver | CMSIS (L475) | All onboard sensors on I2C |
| G2 | BarometerDriver | Driver | II2c, IGpio | LPS22HB — real I2C |
| G3 | HumidityTempDriver | Driver | II2c, IGpio | HTS221 — real I2C |
| G4 | ImuDriver | Driver | II2c, IGpio | LSM6DSL accel+gyro |
| G5 | MagnetometerDriver | Driver | II2c, IGpio | LIS3MDL |
| G6 | SensorService (GW) | Application | Above drivers, ITimeProvider, IHealthReport | Hosts SensorTask (priority 5) |
| G7 | AlarmService (GW) | Application | ISensorService | Co-located in SensorTask |

#### Layer 3 — Modbus RTU (Gateway Master)

| # | Component | Layer | USES | Notes |
|---|---|---|---|---|
| G8 | ModbusUartDriver (GW) | Driver | CMSIS (L475) | UART for RS-485 |
| G9 | ModbusMaster | Middleware | IModbusUart, ILogger | Request frame, response parse |
| G10 | ModbusPoller | Application | IModbusMaster, IHealthReport, ILogger | Hosts ModbusPollerTask (priority 4) |
| G11 | DeviceProfileRegistry | Application | IConfigProvider | Per-slave profile binding |

#### Layer 5 — WiFi / MQTT / Cloud

| # | Component | Layer | USES | Notes |
|---|---|---|---|---|
| G12 | SpiDriver | Driver | CMSIS | ISM43362 WiFi module via SPI |
| G13 | WifiDriver | Driver | ISpi, IGpio | ISM43362 AT command layer |
| G14 | MqttClient | Middleware | IWifi, ILogger | MQTT over TLS |
| G15 | NtpClient | Middleware | IWifi, IHealthReport, ILogger | NTP queries |
| G16 | TimeService | Application | INtpClient, ITimeProvider, ILogger | Hosts TimeServiceTask (priority 2) |
| G17 | CircularFlashLog | Middleware | IQspiFlash, IHealthReport, ILogger | Store-and-forward buffer |
| G18 | StoreAndForward | Application | ICircularFlashLog, ILogger | Offline buffering |
| G19 | CloudPublisher | Application | IMqttClient, ISensorService, IHealthSnapshot, ILogger | Hosts CloudPublisherTask (priority 2) |

---

### OPTIONAL — Layers 6–7

| # | Component | Layer | Notes | Cut order |
|---|---|---|---|---|
| O1 | ResetDriver | Driver | Software reset for watchdog recovery | Cut last |
| O2 | FirmwareStore | Middleware | OTA firmware verification | Cut first |
| O3 | UpdateService | Application | OTA orchestration | Cut first |
| O4 | WifiTask refactor | Application | Dedicated WifiTask as sole SPI owner (D29) | Cut if short on time |
| O5 | CI/CD extensions | Tooling | Hardware-in-loop, clang-tidy, coverage | Cut first |

---

## Priority summary

| Priority | Components | Why |
|---|---|---|
| **Now** | HealthMonitor (close out) | Unblocks TimeProvider |
| **Next** | TimeProvider | Unblocks SensorService, AlarmService, CloudPublisher |
| **Then — FD** | I2cDriver → BarometerDriver/HumidityTempDriver (simulated) → SensorService → AlarmService | Layer 1 FD complete |
| **Then — GW** | I2cDriver(GW) → all sensor drivers → SensorService(GW) | Layer 1 GW complete |
| **Then** | QspiFlashDriver → ConfigStore → ConfigService | Layer 2 |
| **Then — FD** | ModbusUartDriver → ModbusSlave → ModbusRegisterMap | Layer 3 FD |
| **Then — GW** | ModbusUartDriver(GW) → ModbusMaster → ModbusPoller | Layer 3 GW |
| **Then — FD** | SdramDriver → LcdDriver → TouchscreenDriver → GraphicsLibrary → LcdUi | Layer 4 |
| **Then — GW** | SpiDriver → WifiDriver → MqttClient → NtpClient → TimeService → CircularFlashLog → StoreAndForward → CloudPublisher | Layer 5 |
| **Last** | ConsoleService → LifecycleController (both boards) | Ties everything together |
| **Optional** | ResetDriver, FirmwareStore, UpdateService, CI extras | Cut if behind |

---

## Cross-cutting notes

- **DebugUartDriver L475 port** — deferred from PR #25, still open.
  Needed before Logger runs on GW hardware.
- **GW ports** of RtcDriver, Logger, LedDriver, HealthMonitor — each
  driver already has GW conditionals or pin table injection in place.
  The L475 firmware build CI check validates they compile; hardware
  validation needs a separate integration test per component.
- **Simulated sensors** (FD BarometerDriver, HumidityTempDriver) — same
  interface as real ones; implementation is a deterministic value
  generator. Zero hardware dependency; quick to implement.
- **LVGL** (GraphicsLibrary) — the most complex single component.
  Budget 2–3x the time of a typical driver. Already vendored as MIT
  library; wrap `IGraphics` around its render/touch API.
