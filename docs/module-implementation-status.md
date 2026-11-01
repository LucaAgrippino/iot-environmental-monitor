# IoT Environmental Monitor — Module Implementation Status

**As of:** 2026-10-28 · **Repo state:** `main` @ 5dbf9c6. Everything through PR #107 is in `main` and verified with `git merge-base --is-ancestor`: the lifecycle stack (#101), FirmwareStore, InternalFlashDriver and the QSPI mutex (#96–#100), the bootloader (#102), UpdateService (#103), the LifecycleController OTA events (#105) the CloudPublisher OTA flow (#106), the ECC NMI and `verify` heap bench results (#107) and the HLD amendments A1–A4 / B1–B4 (#104). **No PR is open.** The earlier 🔀 marker ("only on the `lifecycle_wiring` branch") no longer applies and has been removed.
**Source of truth:** `firmware/{gateway,field-device}/` trees, `docs/hld/components.md`, `docs/lld/**`.
Hardware columns come from the bench results recorded during the project; where the record is silent it says so rather than guessing.

**Legend** — ✅ implemented and hardware-validated · 🟡 implemented with a caveat · ❌ not implemented · ➖ not part of that board

---

## 1. Summary

| | Gateway (B-L475E-IOT01A) | Field Device (STM32F469I-DISCO) |
|---|---|---|
| HLD modules | 36 | 25 |
| Implemented | 36 (incl. 1 placeholder), plus the bootloader (its own 16 KB image, not an HLD module) | 20 |
| Not implemented | 0 | 5 |
| Bring-up harnesses | 37 | 15 |

**Host tests (baseline, measured 2026-10-28 on `main` @ 5dbf9c6, `ceedling test:all`):** default project 773 tested / 768 passed / 5 ignored / 0 failed; Gateway project 1312 tested / 1309 passed / 3 ignored / 0 failed. After WP2 (`test_tls_arena`, 15 tests): default 788 / 783 / 5 / 0, Gateway unchanged. Per-module counts recorded earlier: UpdateService 70, LifecycleController 114, lifecycle_wiring 55.

**The big gap on both boards:** neither has a real `main()` that creates the task set. Each firmware image is a single bring-up harness chosen through `.cproject` exclusions, so **a new harness or middleware module breaks the firmware build unless the exclusions are edited**. `lifecycle_wiring` is the first piece of production wiring, and the OTA flow is now complete up to that point (CloudPublisher → UpdateService → FirmwareStore → bootloader), but none of it is called from a `main()` (LC-O15). That is also why the **MQTT leg of OTA has never run on the board**: the download over the debug UART and every later step have.

---

## 2. Gateway — implemented

### 2.1 Drivers

| Module | Hardware validation (per project record) | Notes |
|---|---|---|
| CpuDriver | harness exists | Platform singleton, not an ADT |
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
| CircularFlashLog | ✅ TC-HW-CFL-001..010 (2026-10-11, per its LLD; harness now reads a real HealthMonitor, not yet re-run) | CFL-O7 closed 2026-10-26 (the mutex is inside the QSPI driver). Open: CFL-O8 (erase latency vs the 500 ms alarm budget, not measured) |
| ModbusMaster | ✅ validated | |
| FirmwareStore | ✅ PR #96, hardware-validated; also driven by the UpdateService bench | Staging in QSPI, install into the inactive slot, boot record and descriptor handling. `verify` (software ECDSA-P256 + SHA-256) 1.32 s for 480 KB. **FS-O4 open until measured:** `verify` now allocates from the static mbedTLS arena (WP2); 3 584 bytes of heap measured before that change with no TLS session open; the arena peak with a session open is TC-HW-MQTT-014 (REQ-NF-408) |

### 2.3 Application

| Module | Hardware validation | Notes |
|---|---|---|
| HealthMonitor | ✅ used in TimeProvider and sensor runs | LED behaviour and pin PB14 were flagged as blockers earlier |
| SensorService | ✅ TC-HW-SS-001..009 | int32 fixed point; worst cycle 5.1 ms |
| AlarmService | ✅ same run | Thresholds from ConfigService via `alarm_service_reconfigure(const config_params_t*)` (SS-O6 closed 2026-11-01, WP12 D-3..D-6): defaults are now the `config_params_t` set (temperature 40.00 / 0.00 / 1.00 degC, humidity 80/20/2.00 %RH), an active alarm keeps its state when limits move, validation stays in ConfigService. Boot-applied only until D-13. Not run on hardware |
| CloudPublisher | ✅ TC-HW-CP-001..012 (re-run 2026-10-24); CP-013; **WP1 PR-D reconciliation host-tested (95 tests), TC-HW-CP-014..017 to be run** | Links against the real StoreAndForward, HealthMonitor, ConfigService, TimeService interface, ModbusPoller Field Device query and TimeProvider; no placeholder type or function is left (`nm -A` on the harness ELF shows none of the twelve names; LC-O13, SAF-O3, TS-O6, CP-O2, CP-O4, CP-O5 closed). Schema 1.1 on all payloads (timestamp and `sync_state`, `null` for an invalid channel, nested Field Device, health with 20 metrics and the firmware version). `/config` parsed and applied all-or-nothing with `set_param_deferred`, persisted by LifecycleController (LC-D23); `/config` and `/control` now subscribed. OTA flow from #106 unchanged. Open: CP-O8 (`/control` has no result topic), CP-O9 (REQ-CC-010 metrics missing from the snapshot), CP-O10 (128-byte command cap: about five parameters). Alarm outcome of TC-HW-CP-011 is read from MQTT stats and the SAF count in the harness |
| StoreAndForward | ✅ validated 2026-10-11 (harness now reads a real HealthMonitor, not yet re-run) | Reports to the real HealthMonitor (SAF-D7, SAF-O4 closed). CloudPublisher still wired to it through a placeholder type set (SAF-O3 / LC-O13, WP1 PR-D) |
| ModbusPoller | ✅ TC-HW-MBP-001..009 (010 written, not yet run) | `modbus_poller_is_ready()` = initialised only (MBP-D7). Decodes the Field Device sensor registers into a per-slave cache and offers `modbus_poller_get_fd_reading()` (MBP-D8, CP-D18, MBP-O3 closed); 7 new host tests; verified against a real Field Device in WP7 |
| TimeService | ✅ TC-HW-TS-001..005 (2026-10-24) | Modbus bus stubbed in the harness; the FD handler exists since 2026-10-31 (TS-O5 closed), two-board check is WP7 |
| ConfigService | ✅ PR #88 hw-validated 2026-10-24 | Fixed-point as on the FD. `set_param` takes 774 ms (CFG-O7). Consumers not wired (CFG-O8). Already has snapshot/restore/flush internally |
| ConsoleService | ✅ PR #89 hw-validated 30/30 (`scripts/console_bench.py`) | Palette cells, deferred persist, Ctrl-L repaint. Open: GCS-O4..O10 |
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

## 5. Field Device — not implemented

| Module | LLD status | Notes |
|---|---|---|
| **LcdDriver** | Draft | Gates the whole display stack |
| **TouchscreenDriver** | LLD exists | Needs the LCD stack |
| **SdramDriver** | LLD exists | Framebuffer memory for the LCD |
| **GraphicsLibrary** | LLD exists | LVGL vendoring/simulator branch exists on the remote |
| **LcdUi** | LLD exists | Depends on all four above |

The FD side now handles the `CMD_SET_TIME_HI/LO` registers (0x0210/0x0211) that the Gateway's TimeService writes (TS-O5 closed 2026-10-31, `ModbusRegisterMap`, LLD-D16): host tests TC-MRM-028..034 pass; the harness phase that sets the RTC from simulated register writes is written and compiles, but was **not run on a board**.

---

## 6. Cross-cutting open items

| Area | Item |
|---|---|
| Merge state | Resolved: #91–#95 landed through #101; #103, #105, #106, #107 and #104 were merged on 2026-10-28 (stacked PRs retargeted to `main`, `main` merged into each branch to start CI, ancestry verified). No PR is open. The merged feature branches have not been deleted (WP0 hands over the delete commands). The Visual Paradigm model and PNG diagrams are not updated for A1–A4 / B1–B4 |
| Wiring | **Gateway: `firmware/gateway/Src/main.c` exists (WP4 session 2, 2026-10-31)** and `scripts/build-gateway-app.sh` links it into a complete image (all modules, console included) that fits the chip. It is still excluded from the CubeIDE build (`.cproject`, not edited by Claude). Boots on the board to Operational: join on the first attempt, the broker name resolved (D-7), every module started. The cloud session is unstable once connected (send errors `err=6`, GM-O20), so end-to-end publishing is not yet validated. Field Device: still no real `main()` |
| OTA, not yet exercised | MQTT on the board (needs `main()`); `verify` arena peak with TLS open (FS-O4: measured, the verify adds 0 to the 46 280-byte handshake peak); real download rate (US-O1); US-O11. Bootloader: BL-O2 (lifetime rollback counter), BL-O3 (CRC only, no signature check), BL-O4 (no write protection) |
| Lifecycle | LC-O10: FD file still has dead `BOARD_GATEWAY` code. LC-O14: blank vs corrupt ConfigStore indistinguishable |
| Config | `IConfigProvider` still not consumed by TimeService (TS-O4) and ConfigService consumers are unwired (CFG-O8); SensorService half of SS-O6 closed |
| Time | `trigger_retry()` on reconnect is wired in CloudPublisher (TS-O6 closed, CP-D23); TimeService is still never instantiated in production wiring (LC-O15) |
| Telemetry | Invalid sensor channels are sent as 0 and listed in `invalid_channels`; cloud schema confirmation pending (CP-O4) |
| RAM | WP4 option F, measured on the production image (2026-10-31): main bank 97 224 of 98 304 B (1 080 spare), SRAM2 30 928 of 32 768 B (1 840 spare), flash 323 KB of 1 MB. The link fails if the image does not fit (`build-gateway-app.sh`). MqttClient's pool lives in SRAM2 (MQTT-D19, `.bss.sram2`) |
| Storage | CFL-O8 erase spin vs the 500 ms alarm budget (the QSPI mutex itself is merged and closed) |
| Sensors | I2C TIMINGR, DRDY pins PC8/PD11 and real ODRs unmeasured; own harnesses for i2c and groups A/B never run |
| Design | ConsoleService CS-O4..CS-O8 open under a "Pass H complete" status |
| Two-board | Gateway-to-FD time push and register handling to be verified at the end of the project |

## 7. Next steps

The ordered plan is `remaining-work-plan.md` (WP0 to WP13). WP0 (this baseline) is done on branch `docs/status-baseline-2026-10`. The critical path is WP1 (CloudPublisher reconciliation) → WP4 (Gateway `main()`) → WP5 (OTA over MQTT on the board) → WP7 (two boards).
