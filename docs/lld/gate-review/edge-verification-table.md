# LLD Layer 2 — Edge Verification Table

**Branch:** `feature/lld-ambiguity-resolution`  
**Date:** 2026-05-20  
**Contract graph:** `layer-2-contract-graph.json` (122 edges)  
**Status:** Pass A remediation complete; ambiguity-resolution decisions D13–D17 applied.

---

## How to read this table

| Column | Meaning |
|--------|---------|
| `#` | Sequential edge number within this audit (1–122 + new D15 edge) |
| `Consumer` | Component that depends on the provider's interface |
| `Provider` | Component that exposes the interface |
| `Board` | `both`, `fd`, or `gw` |
| `Consumer LLD` | Relative path from `docs/lld/` to the consumer companion |
| `Provider LLD` | Relative path from `docs/lld/` to the provider companion |
| `Status` | `PASS` — both companions exist and edge is documented; `PASS (DNN)` — edge verified and updated in the named decision |

An edge marked `NEW` was introduced by the ambiguity-resolution pass and is not in the original 122-edge contract graph. It should be added to the contract graph before the next gate review.

---

## Both-boards edges (1–20)

| # | Consumer | Provider | Board | Consumer LLD | Provider LLD | Status |
|---|----------|----------|-------|--------------|--------------|--------|
| 1 | Logger | DebugUartDriver | both | middleware/logger.md | drivers/debug-uart-driver.md | PASS |
| 2 | Logger | RtcDriver | both | middleware/logger.md | drivers/rtc-driver.md | PASS |
| 3 | TimeProvider | RtcDriver | both | middleware/time-provider.md | drivers/rtc-driver.md | PASS (D16) |
| 4 | TimeProvider | HealthMonitor | both | middleware/time-provider.md | application/health-monitor.md | PASS |
| 5 | TimeProvider | Logger | both | middleware/time-provider.md | middleware/logger.md | PASS |
| 6 | ConfigStore | QspiFlashDriver | both | middleware/config-store.md | drivers/qspi-flash-driver.md | PASS |
| 7 | ConfigStore | HealthMonitor | both | middleware/config-store.md | application/health-monitor.md | PASS |
| 8 | ConfigStore | Logger | both | middleware/config-store.md | middleware/logger.md | PASS |
| 9 | LedDriver | GpioDriver | both | drivers/led-driver.md | drivers/gpio-driver.md | PASS |
| 10 | HealthMonitor | LedDriver | both | application/health-monitor.md | drivers/led-driver.md | PASS |
| 11 | HealthMonitor | Logger | both | application/health-monitor.md | middleware/logger.md | PASS |
| 12 | SensorService | TimeProvider | both | application/sensor-alarm-service.md | middleware/time-provider.md | PASS |
| 13 | SensorService | ConfigService | both | application/sensor-alarm-service.md | application/config-service.md | PASS |
| 14 | SensorService | HealthMonitor | both | application/sensor-alarm-service.md | application/health-monitor.md | PASS |
| 15 | SensorService | Logger | both | application/sensor-alarm-service.md | middleware/logger.md | PASS |
| 16 | AlarmService | SensorService | both | application/sensor-alarm-service.md | application/sensor-alarm-service.md | PASS |
| 17 | AlarmService | ConfigService | both | application/sensor-alarm-service.md | application/config-service.md | PASS |
| 18 | AlarmService | Logger | both | application/sensor-alarm-service.md | middleware/logger.md | PASS |
| 19 | ConfigService | ConfigStore | both | application/config-service.md | middleware/config-store.md | PASS |
| 20 | ConfigService | Logger | both | application/config-service.md | middleware/logger.md | PASS |

---

## Field-Device-only edges (21–55)

| # | Consumer | Provider | Board | Consumer LLD | Provider LLD | Status |
|---|----------|----------|-------|--------------|--------------|--------|
| 21 | LcdDriver | SdramDriver | fd | drivers/lcd-driver.md | drivers/sdram-driver.md | PASS |
| 22 | TouchscreenDriver | I2cDriver | fd | drivers/touchscreen-driver.md | drivers/i2c-driver.md | PASS |
| 23 | TouchscreenDriver | ExtiDriver | fd | drivers/touchscreen-driver.md | drivers/exti-driver.md | PASS |
| 24 | ModbusSlave | ModbusUartDriver | fd | middleware/modbus-slave.md | drivers/modbus-uart-driver.md | PASS |
| 25 | ModbusSlave | Logger | fd | middleware/modbus-slave.md | middleware/logger.md | PASS |
| 26 | GraphicsLibrary | LcdDriver | fd | middleware/graphics-library.md | drivers/lcd-driver.md | PASS |
| 27 | GraphicsLibrary | TouchscreenDriver | fd | middleware/graphics-library.md | drivers/touchscreen-driver.md | PASS |
| 28 | GraphicsLibrary | Logger | fd | middleware/graphics-library.md | middleware/logger.md | PASS |
| 29 | SensorService | BarometerDriver | fd | application/sensor-alarm-service.md | drivers/humidity-temp-barometer-drivers.md | PASS |
| 30 | SensorService | HumidityTempDriver | fd | application/sensor-alarm-service.md | drivers/humidity-temp-barometer-drivers.md | PASS |
| 31 | LifecycleController | SensorService | fd | application/lifecycle-controller.md | application/sensor-alarm-service.md | PASS |
| 32 | LifecycleController | GraphicsLibrary | fd | application/lifecycle-controller.md | middleware/graphics-library.md | PASS |
| 33 | LifecycleController | AlarmService | fd | application/lifecycle-controller.md | application/sensor-alarm-service.md | PASS |
| 34 | LifecycleController | ConsoleService | fd | application/lifecycle-controller.md | application/console-service-lld.md | PASS |
| 35 | LifecycleController | ConfigService | fd | application/lifecycle-controller.md | application/config-service.md | PASS |
| 36 | LifecycleController | ConfigStore | fd | application/lifecycle-controller.md | middleware/config-store.md | PASS |
| 37 | LifecycleController | Logger | fd | application/lifecycle-controller.md | middleware/logger.md | PASS |
| 38 | LcdUi | GraphicsLibrary | fd | application/lcd-ui-lld.md | middleware/graphics-library.md | PASS |
| 39 | LcdUi | SensorService | fd | application/lcd-ui-lld.md | application/sensor-alarm-service.md | PASS |
| 40 | LcdUi | AlarmService | fd | application/lcd-ui-lld.md | application/sensor-alarm-service.md | PASS |
| 41 | LcdUi | ConfigService | fd | application/lcd-ui-lld.md | application/config-service.md | PASS |
| 42 | LcdUi | HealthMonitor | fd | application/lcd-ui-lld.md | application/health-monitor.md | PASS |
| 43 | LcdUi | Logger | fd | application/lcd-ui-lld.md | middleware/logger.md | PASS |
| 44 | ConsoleService | DebugUartDriver | fd | application/console-service-lld.md | drivers/debug-uart-driver.md | PASS |
| 45 | ConsoleService | SensorService | fd | application/console-service-lld.md | application/sensor-alarm-service.md | PASS |
| 46 | ConsoleService | ConfigService | fd | application/console-service-lld.md | application/config-service.md | PASS |
| 47 | ConsoleService | HealthMonitor | fd | application/console-service-lld.md | application/health-monitor.md | PASS |
| 48 | ConsoleService | Logger | fd | application/console-service-lld.md | middleware/logger.md | PASS |
| 49 | ModbusRegisterMap | ModbusSlave | fd | application/modbus-register-map-lld.md | middleware/modbus-slave.md | PASS |
| 50 | ModbusRegisterMap | SensorService | fd | application/modbus-register-map-lld.md | application/sensor-alarm-service.md | PASS |
| 51 | ModbusRegisterMap | AlarmService | fd | application/modbus-register-map-lld.md | application/sensor-alarm-service.md | PASS (D14) |
| 52 | ModbusRegisterMap | ConfigService | fd | application/modbus-register-map-lld.md | application/config-service.md | PASS |
| 53 | ModbusRegisterMap | HealthMonitor | fd | application/modbus-register-map-lld.md | application/health-monitor.md | PASS (D15) |
| 54 | ModbusRegisterMap | TimeProvider | fd | application/modbus-register-map-lld.md | middleware/time-provider.md | PASS |
| 55 | ModbusRegisterMap | Logger | fd | application/modbus-register-map-lld.md | middleware/logger.md | PASS |

---

## Gateway-only edges (56–122)

| # | Consumer | Provider | Board | Consumer LLD | Provider LLD | Status |
|---|----------|----------|-------|--------------|--------------|--------|
| 56 | WifiDriver | SpiDriver | gw | drivers/wifi-driver.md | drivers/spi-driver.md | PASS |
| 57 | WifiDriver | ExtiDriver | gw | drivers/wifi-driver.md | drivers/exti-driver.md | PASS |
| 58 | MagnetometerDriver | I2cDriver | gw | drivers/magnetometer-imu-drivers.md | drivers/i2c-driver.md | PASS |
| 59 | MagnetometerDriver | ExtiDriver | gw | drivers/magnetometer-imu-drivers.md | drivers/exti-driver.md | PASS |
| 60 | ImuDriver | I2cDriver | gw | drivers/magnetometer-imu-drivers.md | drivers/i2c-driver.md | PASS |
| 61 | ImuDriver | ExtiDriver | gw | drivers/magnetometer-imu-drivers.md | drivers/exti-driver.md | PASS |
| 62 | BarometerDriver | I2cDriver | gw | drivers/humidity-temp-barometer-drivers.md | drivers/i2c-driver.md | PASS |
| 63 | BarometerDriver | GpioDriver | gw | drivers/humidity-temp-barometer-drivers.md | drivers/gpio-driver.md | PASS |
| 64 | HumidityTempDriver | I2cDriver | gw | drivers/humidity-temp-barometer-drivers.md | drivers/i2c-driver.md | PASS |
| 65 | HumidityTempDriver | GpioDriver | gw | drivers/humidity-temp-barometer-drivers.md | drivers/gpio-driver.md | PASS |
| 66 | MqttClient | WifiDriver | gw | middleware/mqtt-client.md | drivers/wifi-driver.md | PASS (D13) |
| 67 | MqttClient | Logger | gw | middleware/mqtt-client.md | middleware/logger.md | PASS |
| 68 | ModbusMaster | ModbusUartDriver | gw | middleware/modbus-master-poller.md | drivers/modbus-uart-driver.md | PASS |
| 69 | ModbusMaster | Logger | gw | middleware/modbus-master-poller.md | middleware/logger.md | PASS |
| 70 | CircularFlashLog | QspiFlashDriver | gw | middleware/circular-flash-log.md | drivers/qspi-flash-driver.md | PASS |
| 71 | CircularFlashLog | HealthMonitor | gw | middleware/circular-flash-log.md | application/health-monitor.md | PASS |
| 72 | CircularFlashLog | Logger | gw | middleware/circular-flash-log.md | middleware/logger.md | PASS |
| 73 | NtpClient | WifiDriver | gw | middleware/ntp-client.md | drivers/wifi-driver.md | PASS (D13) |
| 74 | NtpClient | HealthMonitor | gw | middleware/ntp-client.md | application/health-monitor.md | PASS |
| 75 | NtpClient | Logger | gw | middleware/ntp-client.md | middleware/logger.md | PASS |
| 76 | FirmwareStore | QspiFlashDriver | gw | middleware/firmware-store.md | drivers/qspi-flash-driver.md | PASS |
| 77 | FirmwareStore | Logger | gw | middleware/firmware-store.md | middleware/logger.md | PASS |
| 78 | SensorService | BarometerDriver | gw | application/sensor-alarm-service.md | drivers/humidity-temp-barometer-drivers.md | PASS |
| 79 | SensorService | HumidityTempDriver | gw | application/sensor-alarm-service.md | drivers/humidity-temp-barometer-drivers.md | PASS |
| 80 | SensorService | MagnetometerDriver | gw | application/sensor-alarm-service.md | drivers/magnetometer-imu-drivers.md | PASS |
| 81 | SensorService | ImuDriver | gw | application/sensor-alarm-service.md | drivers/magnetometer-imu-drivers.md | PASS |
| 82 | LifecycleController | SensorService | gw | application/lifecycle-controller.md | application/sensor-alarm-service.md | PASS |
| 83 | LifecycleController | AlarmService | gw | application/lifecycle-controller.md | application/sensor-alarm-service.md | PASS |
| 84 | LifecycleController | CloudPublisher | gw | application/lifecycle-controller.md | application/cloud-publisher-lld.md | PASS |
| 85 | LifecycleController | ModbusPoller | gw | application/lifecycle-controller.md | middleware/modbus-master-poller.md | PASS |
| 86 | LifecycleController | UpdateService | gw | application/lifecycle-controller.md | application/update-service-lld.md | PASS |
| 87 | LifecycleController | TimeService | gw | application/lifecycle-controller.md | application/time-service-lld.md | PASS |
| 88 | LifecycleController | ConsoleService | gw | application/lifecycle-controller.md | application/console-service-lld.md | PASS |
| 89 | LifecycleController | ConfigService | gw | application/lifecycle-controller.md | application/config-service.md | PASS |
| 90 | LifecycleController | ConfigStore | gw | application/lifecycle-controller.md | middleware/config-store.md | PASS |
| 91 | LifecycleController | ResetDriver | gw | application/lifecycle-controller.md | drivers/reset-driver.md | PASS |
| 92 | LifecycleController | Logger | gw | application/lifecycle-controller.md | middleware/logger.md | PASS |
| 93 | CloudPublisher | MqttClient | gw | application/cloud-publisher-lld.md | middleware/mqtt-client.md | PASS |
| 94 | CloudPublisher | SensorService | gw | application/cloud-publisher-lld.md | application/sensor-alarm-service.md | PASS |
| 95 | CloudPublisher | AlarmService | gw | application/cloud-publisher-lld.md | application/sensor-alarm-service.md | PASS |
| 96 | CloudPublisher | ModbusPoller | gw | application/cloud-publisher-lld.md | middleware/modbus-master-poller.md | PASS |
| 97 | CloudPublisher | StoreAndForward | gw | application/cloud-publisher-lld.md | application/store-and-forward-lld.md | PASS |
| 98 | CloudPublisher | HealthMonitor | gw | application/cloud-publisher-lld.md | application/health-monitor.md | PASS |
| 99 | CloudPublisher | Logger | gw | application/cloud-publisher-lld.md | middleware/logger.md | PASS |
| 100 | TimeService | TimeProvider | gw | application/time-service-lld.md | middleware/time-provider.md | PASS (D16) |
| 101 | TimeService | NtpClient | gw | application/time-service-lld.md | middleware/ntp-client.md | PASS (D13) |
| 102 | TimeService | ModbusPoller | gw | application/time-service-lld.md | middleware/modbus-master-poller.md | PASS |
| 103 | TimeService | Logger | gw | application/time-service-lld.md | middleware/logger.md | PASS |
| 104 | UpdateService | MqttClient | gw | application/update-service-lld.md | middleware/mqtt-client.md | PASS |
| 105 | UpdateService | FirmwareStore | gw | application/update-service-lld.md | middleware/firmware-store.md | PASS |
| 106 | UpdateService | ResetDriver | gw | application/update-service-lld.md | drivers/reset-driver.md | PASS |
| 107 | UpdateService | Logger | gw | application/update-service-lld.md | middleware/logger.md | PASS |
| 108 | ConsoleService | DebugUartDriver | gw | application/console-service-lld.md | drivers/debug-uart-driver.md | PASS |
| 109 | ConsoleService | SensorService | gw | application/console-service-lld.md | application/sensor-alarm-service.md | PASS |
| 110 | ConsoleService | ConfigService | gw | application/console-service-lld.md | application/config-service.md | PASS |
| 111 | ConsoleService | DeviceProfileRegistry | gw | application/console-service-lld.md | application/device-profile-registry-lld.md | PASS |
| 112 | ConsoleService | HealthMonitor | gw | application/console-service-lld.md | application/health-monitor.md | PASS |
| 113 | ConsoleService | Logger | gw | application/console-service-lld.md | middleware/logger.md | PASS |
| 114 | ModbusPoller | ModbusMaster | gw | middleware/modbus-master-poller.md | middleware/modbus-master-poller.md | PASS |
| 115 | ModbusPoller | DeviceProfileRegistry | gw | middleware/modbus-master-poller.md | application/device-profile-registry-lld.md | PASS |
| 116 | ModbusPoller | HealthMonitor | gw | middleware/modbus-master-poller.md | application/health-monitor.md | PASS |
| 117 | ModbusPoller | Logger | gw | middleware/modbus-master-poller.md | middleware/logger.md | PASS |
| 118 | DeviceProfileRegistry | ConfigStore | gw | application/device-profile-registry-lld.md | middleware/config-store.md | PASS |
| 119 | DeviceProfileRegistry | Logger | gw | application/device-profile-registry-lld.md | middleware/logger.md | PASS |
| 120 | StoreAndForward | CircularFlashLog | gw | application/store-and-forward-lld.md | middleware/circular-flash-log.md | PASS |
| 121 | StoreAndForward | HealthMonitor | gw | application/store-and-forward-lld.md | application/health-monitor.md | PASS |
| 122 | StoreAndForward | Logger | gw | application/store-and-forward-lld.md | middleware/logger.md | PASS |

---

## New edges introduced by ambiguity-resolution decisions

The following edge was not present in the original 122-edge contract graph. It is documented here for traceability and must be added to `layer-2-contract-graph.json` before the next gate review.

| # | Consumer | Provider | Board | Consumer LLD | Provider LLD | Status |
|---|----------|----------|-------|--------------|--------------|--------|
| 123 | LifecycleController | HealthMonitor | both | application/lifecycle-controller.md | application/health-monitor.md | NEW (D15) |

**Rationale:** LLD-D15 routes CMD_RESET_METRICS via `lifecycle_controller->handle_remote_command()` → `health_admin->reset_metrics()`. This creates a direct LifecycleController→HealthMonitor (IHealthAdmin) dependency on both boards that was absent from the contract graph. The HLD has been updated (components.md) and the LLD companion USES headers have been updated; the JSON graph update is deferred to a follow-up commit.

---

## Summary

| Category | Count | All PASS |
|----------|-------|---------|
| Both-boards edges | 20 | Yes |
| FD-only edges | 35 | Yes |
| GW-only edges | 67 | Yes |
| **Total (graph)** | **122** | **Yes** |
| New edges (D15) | 1 | NEW — to be added to graph |

**Decisions applied in this pass:** LLD-D13 (edges 3, 66, 73, 101), LLD-D14 (edge 51), LLD-D15 (edges 53, 123), LLD-D16 (edges 3, 100), LLD-D17 (no edge change — storage model only).

All 122 edges from `layer-2-contract-graph.json` have a PASS status. Both LLD companion files exist for every edge. No broken edges. No missing provider companions. No missing consumer companions.
