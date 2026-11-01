# Field Device `main()` and task set

**Document:** `docs/lld/application/field-device-main.md`
**Version:** 1.0 · **Date:** 2026-11-01 · **Board:** Field Device (STM32F469I-DISCO)
**Source:** `firmware/field-device/Src/main.c`. **Build:** `scripts/build-fd-app.sh [--flash]`
**Status:** builds and links (RAM 62.6 KB of 256 KB, flash 70 KB of 2 MB). **Not yet run on the board.**

## 1. What it is

The first real application entry point of the Field Device. It replaces the Cube stub (`for(;;)`) and
supersedes the `lifecycle_controller` bring-up harness as the image to test with. The harness stays
in `integration-tests/` because CubeIDE still links it (`.cproject` is not edited by Claude; `Src/main.c`
is excluded there). `scripts/build-fd-app.sh` links the product sources directly, with the flags of the
generated Debug makefiles, the way `scripts/build-gateway-app.sh` does for the Gateway.

## 2. Creation order

All static allocation, no heap (P5).

1. Reset cause (the flags clear when read), clock tree, GPIO, LEDs (PG6 green, PD5 red, active low), RTC,
   debug UART, I2C, QSPI, Modbus UART.
2. HealthMonitor, Logger, TimeProvider, ConfigStore.
3. ConfigService with defaults, then the stored values, so the Modbus task sees the stored slave address.
   LifecycleController loads and applies them again in its LoadingConfig step.
4. ConsoleService, ModbusRegisterMap, LifecycleController.
5. Tasks, LifecycleTask last; then the scheduler.

## 3. Tasks

| Task | Prio | Stack (words) | Notes |
|---|---|---|---|
| `SensorTask` | 5 | 768 | Calls `sensor_service_init()` and `alarm_service_init()`, then `sensor_service_run_cycle()` on every 100 ms notification. Logs alarm raised / cleared |
| `ModbusTask` | 4 | 512 | Two-phase `modbus_slave_init`, then `modbus_slave_process()` per frame notification |
| `Console` | 1 | 768 | `console_task_body` |
| `LifecycleTask` | 1 | 2048 | `lifecycle_task_body`; releases the start gate on Operational |
| `StackReport` | 1 | 384 | One shot, 30 s after boot: logs the free stack of the four tasks (closes the Field Device half of LLD O2 once run) |

`LcdUiTask` (HLD 4.2) is not built: there is no LCD module yet, and `USE_GUI` is off in LifecycleController.

## 4. Decisions

| # | Decision | Why |
|---|---|---|
| **FDM-D1** | `SensorService` is initialised by `SensorTask`, not by LifecycleController | `sensor_service_init()` stores `xTaskGetCurrentTaskHandle()` as the target of its 100 ms poll timer. The bring-up harness let LifecycleTask call it, so the timer notified LifecycleTask and no sensor cycle ever ran |
| **FDM-D2** | LifecycleController receives copies of the sensor and alarm vtables whose `init` waits (up to 2 s) for SensorTask | Its BringingUpSensors step calls `init()`; a second real init would reset the service and re-bind the timer |
| **FDM-D3** | TimeProvider and the LEDs are initialised here | The harness skipped both; the register map reads time, HealthMonitor drives the LEDs |
| **FDM-D4** | The legacy `IModbusRegisterMap` bridge (including the FC16 `write_block` path of B2) moved in unchanged | MRM-DEVIATION-001 is still open |
| **FDM-D5** | Alarm events are logged (`alarm raised HIGH: TEMPERATURE (...)`) | The Field Device otherwise says nothing when a limit is crossed, so test 2B-007 could not be timed from the log |
| **FDM-D6** | Priorities follow HLD 4.2; LifecycleTask keeps the harness's 2048-word stack | The boot sequence runs on it |

## 5. Open

| # | Item |
|---|---|
| FDM-O1 | Never run on the board. First run: flash, read the log, check `Operational`, read the registers with the dongle (2B-001) |
| FDM-O2 | Stack sizes are the HLD / harness values; the StackReport lines give the measured figures |
| FDM-O3 | No LcdUi (no module). The 4.3 inch display stays dark |
| FDM-O4 | `.cproject` still links the old harness; the CubeIDE `Debug` build is not this image |
