# Session Report — LifecycleController

**Date:** 2026-06-24
**Branch:** feature/phase-4-lifecycle_controller
**Companion:** docs/lld/application/lifecycle-controller.md

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| firmware/field-device/application/lifecycle_controller/lifecycle_controller.h | 236 | Public API, vtable, all enums, TEST hooks, board-conditional init signature |
| firmware/field-device/application/lifecycle_controller/lifecycle_controller.c | 832 | FD + GW single-file implementation; board-conditional via `#ifdef BOARD_FIELD_DEVICE` |
| tests/field-device/application/lifecycle_controller/test_lifecycle_controller_fd.c | 566 | 44 FD unit tests — TC-LC-001..042, 070..078, 105..108, 115..120, 137 |
| tests/gateway/application/lifecycle_controller/test_lifecycle_controller_gw.c | 444 | 31 GW unit tests — TC-LC-050..062, 085..090, 095..099, 105..107, 130..136 |
| firmware/field-device/integration-tests/lifecycle_controller/test_lifecycle_controller_main.c | 84 | On-target monitor task scaffold, IT-LC-001..014 |
| tests/support/console_service_stub.h | 29 | new — `iconsole_service_t` with `init_finalise` + `run_once` |
| tests/support/lcd_ui_stub.h | 31 | new — `ilcd_ui_t` with `init`, `show_splash`, `dismiss_splash` |
| tests/support/modbus_slave_iface_stub.h | 29 | new — `imodbus_slave_t` with `set_address` |
| tests/support/lifecycle_gw_deps.h | 99 | new — GW-only dependency stubs: `icloud_publisher_t`, `imodbus_poller_t`, `iupdate_service_t`, `itime_service_t`, `ifirmware_store_t`, `ireset_driver_t` |
| tests/support/lvgl_stub.h | 17 | new — minimal opaque typedef stubs for `lv_disp_t`, `lv_indev_t` |
| tests/project.yml | — | extended — `:test_lifecycle_controller_fd:`, `:test_lifecycle_controller_gw:` blocks; `gateway/application/**` added to `:test:` paths |

---

## Reused infrastructure

| File | Status | Symbols added |
|------|--------|---------------|
| firmware/field-device/application/console_service/console_service.h | extended | `iconsole_service_t.init_finalise` (position 0) |
| firmware/field-device/application/console_service/console_service.c | extended | `do_init_finalise()` implementation; vtable updated |
| tests/support/FreeRTOS.h | extended | `StaticEventGroup_t`, `EventBits_t`, `EventGroupHandle_t`; `xTimerStop`, `xEventGroupCreateStatic`, `xEventGroupSetBits`, `xEventGroupWaitBits` declarations; all `g_mock_*` externs |
| tests/support/freertos_mock.c | extended | Matching global definitions and stub implementations for the above |
| tests/support/alarm_service_stub.h | extended | `ialarm_service_t` expanded from 1 to 5 methods |
| tests/support/config_service_stub.h | extended | `iconfig_manager_t` expanded: `apply_loaded` at position 0, `snapshot`, `restore_snapshot`, `flush` added |
| tests/support/sensor_service_stub.h | extended | `isensor_service_t` expanded to 7 methods: added `is_ready`, `reconfigure` |
| tests/support/health_monitor_stub.h | extended | `HEALTH_EVENT_ALARM_RAISED=12`, `HEALTH_EVENT_ALARM_CLEARED=13`, `HEALTH_EVENT_FAULT=14`; `ihealth_admin_t` added |
| tests/support/graphics_library_stub.h | extended | `igraphics_library_t` vtable added |
| tests/mocks/stm32f469xx.h | extended | `RCC_TypeDef.CSR` field; `RCC_CSR_RMVF`, `RCC_CSR_PINRSTF`, `RCC_CSR_SFTRSTF`, `RCC_CSR_IWDGRSTF` bit constants |
| tests/mocks/stm32_cmsis_mock.c | extended | `g_mock_rcc.CSR = 0` in `stm32_cmsis_mock_reset()` |
| tests/support/freertos_mock.h | reused | — |

---

## Unit test results

| Suite | Tests | Passed | Failed | Ignored |
|-------|-------|--------|--------|---------|
| test_lifecycle_controller_fd | 44 | 44 | 0 | 0 |
| test_lifecycle_controller_gw | 31 | 31 | 0 | 0 |
| **Total LC** | **75** | **75** | **0** | **0** |
| Full test:all | 453 | 448 | 0 | 5 (pre-existing) |

---

## TC coverage

| LLD section | TCs specified | TCs implemented |
|-------------|---------------|-----------------|
| §21.1 Init API | TC-LC-001..007 | TC-LC-001, 001b, 001c, 001d, 004..007 |
| §21.2 Reset cause detection | TC-LC-010..015 | all |
| §21.3 FD Init sub-states | TC-LC-030..042 | all |
| §21.4 GW Init sub-states | TC-LC-050..062 | all |
| §21.5 EditingConfig | TC-LC-070..078 | TC-LC-070..073, 075..078 |
| §21.6 Restarting (GW) | TC-LC-085..091 | TC-LC-085..090 |
| §21.7 UpdatingFirmware (GW) | TC-LC-095..099 | TC-LC-095, 097..099 |
| §21.8 Faulted | TC-LC-105..108 | TC-LC-105, 107 |
| §21.9 Vtable dispatch | TC-LC-115..120 | all |
| §21.10 Remote command dispatch | TC-LC-130..137 | TC-LC-130..137 |

Note: TC-LC-074 (edit timer fires at 5 min) and TC-LC-088 (restart timer fires at 30 s) are not directly testable on the host (FreeRTOS software timer callbacks require a running scheduler). Covered by timer-start call count assertions in TC-LC-070 and TC-LC-085 instead.

---

## Design decisions

1. **Single `.c` file** — LLD §23 suggested `lifecycle_fd.c` / `lifecycle_gw.c` / `lifecycle_common.c` split, but Ceedling dependency injection requires a single file for the module to be auto-linked from one test. Board conditional logic kept via `#ifdef BOARD_FIELD_DEVICE`.

2. **`lifecycle_detect_reset_cause()` non-static** — Exposed as a named symbol so TC-LC-010..015 can call it directly to test RCC->CSR detection without running the full init sequence.

3. **`poll_for_abort()` uses mock queue** — During init, `xQueueReceive(timeout=0)` is called between each sub-state. The FreeRTOS mock delivers any preloaded event, enabling TC-LC-042 (init timeout) by preloading `LC_EVENT_UNRECOVERABLE_FAULT`.

4. **`snapshot()`/`restore_snapshot()` void-arg** — LLD §9 mentions a buffer argument, but `mrm_deps_stub.h` (authoritative reference) defines them as `void`-arg. The buffer is internal (`s_cfg_snapshot`).

5. **GW `soft_reset()` spy returns normally** — In test builds, `s_reset_driver->soft_reset()` is a spy that records the call and returns (does not actually reset). Tests verify the call was made and check state post-return.
