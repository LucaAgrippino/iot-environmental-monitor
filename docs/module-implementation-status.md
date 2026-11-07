# IoT Environmental Monitor — Module Implementation Status

**As of:** 2026-11-08 · **Repo state:** `main` @ cf2fbdc. **The implementation phase is closed: see `docs/implementation-phase-closure.md`.** Sections 2 to 4 keep the per-module hardware notes from the bench work; sections 1 and 5 to 7 are current.
**Source of truth:** `firmware/{gateway,field-device}/` trees, `docs/hld/components.md`, `docs/lld/**`.
Hardware columns come from the bench results recorded during the project; where the record is silent it says so rather than guessing.

**Legend** — ✅ implemented and hardware-validated · 🟡 implemented with a caveat · ❌ not implemented · ➖ not part of that board

---

## 1. Summary

| | Gateway (B-L475E-IOT01A) | Field Device (STM32F469I-DISCO) |
|---|---|---|
| HLD modules | 36 | 25 |
| Implemented | 36, plus the bootloader (its own 16 KB image, not an HLD module) and AdcDriver, CpuDriver, WatchdogDriver (added by design notes, not HLD modules) | 25, plus AdcDriver, CpuDriver, WatchdogDriver |
| Not implemented | 0 | 0 |
| Production `main()` | `firmware/gateway/Src/main.c`, built by `scripts/build-gateway-app.sh` | `firmware/field-device/Src/main.c`, built by `scripts/build-fd-app.sh` (`GUI=1` adds the display stack) |
| Image size | RAM 97 080 of 98 304 B (98.75 %), SRAM2 11 440 of 32 768 B, flash 336 608 B of 1 MB | RAM 209 168 B of 256 KB (GUI image; 63 272 B without the display), flash 490 772 B of 2 MB (GUI image) |

**Host tests (2026-11-08, `ceedling test:all`):** default project and Gateway project both with 0 failed; counts are in the closure report. Per-module counts are in the module notes below.

**Wiring:** both boards have a production `main()` that creates the task set. A new bring-up harness must still be added to the `excluding=` lists of the CubeIDE projects (`scripts/check-build-exclusions.sh` checks it).

---

## 2. Gateway — implemented

### 2.1 Drivers

| Module | Hardware validation (per project record) | Notes |
|---|---|---|
| CpuDriver | ✅ TC-HW-CPU-007/008/011 on both boards, 010 on the Field Device (2026-11-07) | Platform singleton, not an ADT. Panic record, report and fault stubs on both boards (the Field Device has the panic subset); LLD `cpu-driver.md`. The Gateway fault stubs reported the wrong PC before this change |
| DebugUartDriver | harness exists | |
| GpioDriver | harness exists | |
| ExtiDriver | harness exists | |
| RtcDriver | harness exists | |
| SpiDriver | harness exists | |
| LedDriver | harness exists | |
| WifiDriver (ISM43362) | ✅ validated | 2.4 GHz only; DNS lookup via `D0` (`wifi_resolve`, WIFI-O18), not yet used by callers |
| QspiFlashDriver | ✅ TC-HW-QSPI-001..010 | Sector erase 81 ms, page program 3.2 ms. QSPID-O1 is closed (2026-10-26): the driver takes its own priority-inheritance mutex for each whole operation, hardware-validated |
| ModbusUartDriver | ✅ TC-HW-MBUART-001..010 | PA15 RTS/DE unverified |
| ResetDriver | ✅ TC-HW-RST-001..004 | `reset_trigger()`; reuses `cpu_hw.h`, adds `CPU_HW_DATA_BARRIER`. Committed config survived a real reset ordered via `/control` |
| InternalFlashDriver | ✅ PR #97, hardware-validated | Page erase 22.1 ms, 2 KB program 22.3 ms. An erase in the bank the code runs from stalls the CPU for those 22 ms; the other bank does not stall |
| Bootloader | ✅ PR #102, TC-HW-BL 12/12 (`scripts/bootloader_bench.py`) | 4.9 KB of 16 KB at 0x0800_0000. Verifies a 480 KB slot in 201 ms at 48 MHz; swap, trial, confirm and rollback measured. Application and bootloader both absorb an uncorrectable flash ECC NMI (BL-O8 closed on the board 2026-10-28). Open: BL-O2..O4 (rollback cap is a lifetime counter, no signature check in the bootloader, no write protection), BL-O9, BL-O10 |
| I2cDriver | 🟡 exercised through SensorService | TIMINGR value not measured; own harness not run |
| HumidityTempDriver | 🟡 exercised through SensorService | Own harness not run |
| BarometerDriver | 🟡 exercised through SensorService | Own harness not run |
| MagnetometerDriver | 🟡 exercised through SensorService | PC8/PD11 pins from memory of ST map; ODR not measured |
| ImuDriver | 🟡 exercised through SensorService | Same pin and ODR caveats |

### 2.2 Middleware

| Module | Hardware validation | Notes |
|---|---|---|
| Logger | harness exists | |
| WifiTask | ✅ validated | Alarm-latency decision CP-O6 largely resolved |
| MqttClient | ✅ validated | TLS mutual auth, non-blocking publish. WP2: mbedTLS now allocates only from the static `tls_arena` (28 KB since WP4 D-1 (MQTT-D19), mutex-guarded; `tls_arena_init()` must run before the scheduler). Arena peak with a TLS session open (TC-HW-MQTT-014): **46 280 bytes** with the default 16 KB record buffers (2026-10-29; the old 44 KB arena was 1 224 bytes too small), **20 920 bytes** with the 5 120 / 2 048 buffers against AWS IoT Core (2026-10-30). `tls_server_name` (MQTT-D18) separates the certificate name from the connect address |
| NtpClient | ✅ validated | IPv4 servers only |
| TimeProvider | ✅ TC-HW-TP-001..011 | |
| ConfigStore | ✅ validated | Diverges from the FD copy (CS-O5) |
| CertStore | ✅ hardware-checked 2026-11-08 | WiFi credentials and the X.509 identity from the console; 20 host tests; `lld/middleware/cert-store.md` |
| CircularFlashLog | ✅ TC-HW-CFL-001..010 (2026-10-11, per its LLD; harness now reads a real HealthMonitor, not yet re-run) | CFL-O7 closed 2026-10-26 (the mutex is inside the QSPI driver). Open: CFL-O8 (erase latency vs the 500 ms alarm budget, not measured) |
| ModbusMaster | ✅ validated | |
| FirmwareStore | ✅ PR #96, hardware-validated; also driven by the UpdateService bench | Staging in QSPI, install into the inactive slot, boot record and descriptor handling. `verify` (software ECDSA-P256 + SHA-256) 1.32 s for 480 KB. **FS-O4 open until measured:** `verify` now allocates from the static mbedTLS arena (WP2); 3 584 bytes of heap measured before that change with no TLS session open; the arena peak with a session open is TC-HW-MQTT-014 (REQ-NF-408) |

### 2.3 Application

| Module | Hardware validation | Notes |
|---|---|---|
| HealthMonitor | ✅ used in TimeProvider and sensor runs | LED behaviour and pin PB14 were flagged as blockers earlier |
| SensorService | ✅ TC-HW-SS-001..009 | int32 fixed point; worst cycle 5.1 ms |
| AlarmService | ✅ same run | Thresholds from ConfigService via `alarm_service_reconfigure(const config_params_t*)` (SS-O6 closed 2026-11-01, WP12 D-3..D-6): defaults are now the `config_params_t` set (temperature 40.00 / 0.00 / 1.00 degC, humidity 80/20/2.00 %RH), an active alarm keeps its state when limits move, validation stays in ConfigService. Boot-applied only until D-13. Not run on hardware. **Field Device (2026-11-03, PR #155):** `alarm_service_set_thresholds` fed from ConfigService at start and on every change; hardware-verified (a raised and a cleared temperature alarm reached the broker). Luca confirmed the 40.00 / 1.00 degC default on 2026-11-03 |
| CloudPublisher | ✅ TC-HW-CP-001..012 (re-run 2026-10-24); CP-013; **WP1 PR-D reconciliation host-tested (95 tests), TC-HW-CP-014..017 to be run** | Links against the real StoreAndForward, HealthMonitor, ConfigService, TimeService interface, ModbusPoller Field Device query and TimeProvider; no placeholder type or function is left (`nm -A` on the harness ELF shows none of the twelve names; LC-O13, SAF-O3, TS-O6, CP-O2, CP-O4, CP-O5 closed). Schema 1.1 on all payloads (timestamp and `sync_state`, `null` for an invalid channel, nested Field Device, health with 20 metrics and the firmware version). `/config` parsed and applied all-or-nothing with `set_param_deferred`, persisted by LifecycleController (LC-D23); `/config` and `/control` now subscribed. OTA flow from #106 unchanged. Open: CP-O8 (`/control` has no result topic), CP-O9 (REQ-CC-010 metrics missing from the snapshot), CP-O10 (128-byte command cap: about five parameters). Alarm outcome of TC-HW-CP-011 is read from MQTT stats and the SAF count in the harness |
| StoreAndForward | ✅ validated 2026-10-11 (harness now reads a real HealthMonitor, not yet re-run) | Reports to the real HealthMonitor (SAF-D7, SAF-O4 closed). CloudPublisher still wired to it through a placeholder type set (SAF-O3 / LC-O13, WP1 PR-D) |
| ModbusPoller | ✅ TC-HW-MBP-001..009 (010 written, not yet run) | `modbus_poller_is_ready()` = initialised only (MBP-D7). Decodes the Field Device sensor registers into a per-slave cache and offers `modbus_poller_get_fd_reading()` (MBP-D8, CP-D18, MBP-O3 closed); 7 new host tests; verified against a real Field Device in WP7 |
| TimeService | ✅ TC-HW-TS-001..005 (2026-10-24) | Modbus bus stubbed in the harness; the FD handler exists since 2026-10-31 (TS-O5 closed), two-board check is WP7 |
| ConfigService | ✅ PR #88 hw-validated 2026-10-24 | Fixed-point as on the FD. `set_param` takes 774 ms (CFG-O7). Consumers not wired (CFG-O8). Already has snapshot/restore/flush internally |
| ConsoleService | ✅ three-panel redesign (v2.0, branch `feature/phase-4-gw-console_three_panel`): host tests + simulator pass, **hardware bench not yet run** (`scripts/console_bench.py` PN-001..074) | Header / log / command panel on scroll regions, Logger sink, `term`. The cockpit TUI of PR #89 is gone. Open: GCS-O4..O10 |
| LifecycleController | ✅ PR #90 hw-validated TC-HW-LC-001..014, 114 host tests | Local `lc_*_if_t` interfaces. PR #105 (LCC-2..4, LC-D19..D22): `OTA_FINISHED` → Operational, `OTA_RESET_REQUESTED` → flush, 2 s delay, soft reset; rollback path without the delay; the real UpdateService and FirmwareStore are bound when the wiring gets all four OTA members. LC-D23 (WP1 PR-C): also writes deferred configuration changes to flash on its idle tick and before every reset (`persist_pending`). Open: LC-O10, LC-O11, LC-O13..O15 |
| LifecycleWiring (not an HLD module) | ✅ TC-HW-LW-001..009, 55 host tests | LC-D15..D18. Real controller over ConfigStore on QSPI, ConfigService, SensorTask, AlarmService, ModbusPoller, HealthMonitor, ResetDriver. Blank ConfigStore is treated as a first boot (corrupt vs blank indistinguishable, LC-O14). `/control` payload `{"command":"restart"\|"restart_confirm"\|"reset_metrics"}` is my own definition (LC-D17) |
| UpdateService | ✅ PR #103; TC-HW-US-001..011 on the board 2026-10-27/28 | Real FreeRTOS, FirmwareStore, bootloader; images over the debug UART. 480 KB image in 90.2 s (UART-bound, not an MQTT figure), 25 power-cut points never left the device unbootable, record write atomic at about 22 ms, longest interrupt gap 22.5 ms from Slot A (limit 30 ms), rollback reported 0.23 s after a failed self-check (REQ-NF-204: 10 s), stack 586 of 1024 words free. `update_service_get_instance()` (US-D15, TC-US-003b) hands out the singleton handle before `update_service_create()`, so `cloud_publisher_config_t::update` can be filled before the transport exists (US-O12 closed; WP4 D-2). **Not covered:** MQTT on the board, `verify` heap with a TLS session open (FS-O4), US-O1 (real download rate), US-O11 |
| DeviceProfileRegistry | 🟡 placeholder | Enough for ModbusPoller; the real registry is not built |

SensorService additionally gained `sensor_service_reconfigure(const config_params_t*)` (SS-D20; alpha per-mille, ranges, polling period to Group A; TC-HW-SS-010), which closes the SensorService half of SS-O6. Sensor drivers must still init **before** the scheduler (SS-D9), so Lifecycle's sensors/alarms/cloud/console adapters only verify.

---

## 3. Gateway — not implemented

None of the 36 HLD modules is missing any more. FirmwareStore and UpdateService, the last two, are implemented and hardware-validated (section 2). What remains on the Gateway is wiring, not modules: see section 6.

---

## 4. Field Device — implemented

| Layer | Modules |
|---|---|
| Drivers | DebugUart, Gpio, Led, Rtc, I2c, HumidityTemp, Barometer, ModbusUart, QspiFlash |
| Middleware | Logger, TimeProvider, ConfigStore, ModbusSlave |
| Application | HealthMonitor, SensorService, AlarmService, ConfigService, ConsoleService, LifecycleController, ModbusRegisterMap |

Hardware status of the Field Device modules is **not tracked in my project notes** (the bench work during this phase was all Gateway). The FD has 15 bring-up harnesses, including one for simulated sensors.

Known deliberate details: the FD codebase contains planted bugs for the interview-exercise material (for example the single-byte `write_read` ACK in `i2c_driver_f4.c`). They must not be fixed or copied into Gateway code.

## 5. Field Device: display stack (implemented since 2026-11)

| Module | Status | Notes |
|---|---|---|
| SdramDriver, LcdDriver, TouchscreenDriver | ✅ hardware-validated | framebuffer cleared before the panel starts; touch axes mapped for landscape (LcdUi LLD §19.5) |
| GraphicsLibrary | ✅ | LVGL v8.3.11, vendor sources built at `-O2` by the script |
| LcdUi | ✅ LLD v0.4 | five screens, splash, Config with keypad and confirm modal, Trend history; touch checked by hand on the board |
| AdcDriver, CpuDriver, WatchdogDriver | ✅ | `TC-HW-ADC-001` (FD) and `-002` (GW) pass; CPU panic report and watchdog hardware-validated |

The Field Device handles the `CMD_SET_TIME` registers (TS-O5 closed). The display modules are excluded from CubeIDE's Debug configuration and built with `GUI=1 scripts/build-fd-app.sh`.

---

## 6. Open items at the close of the implementation phase

All open items are triaged in `docs/implementation-phase-closure.md` section 5 (closed, deferred with a reason, or moved to the verification phase). The list that stood here on 2026-10-28 was obsolete: the production `main()` of both boards, the OTA flow on the board, the CloudPublisher reconciliation, TimeService wiring, the Field Device display stack and the two-board time push are all done.

## 7. Next steps

The implementation phase is closed. Next: the verification phase (system test on two boards, requirement-by-requirement verification, operator console), see `docs/implementation-phase-closure.md` section 6.
