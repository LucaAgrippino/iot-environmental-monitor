# Session Report — ExtiDriver

**Date:** 2026-07-06
**Branch:** feature/phase-4-shared-exti-driver
**Companion:** docs/lld/drivers/exti-driver.md (Phase H ready, v1.0)

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| firmware/shared/drivers/exti/exti_driver.h | 137 | new |
| firmware/shared/drivers/exti/exti_driver.c | 178 | new |
| tests/shared/drivers/exti-driver/test_exti_driver_gw.c | 232 | new — STM32L475xx register layout |
| tests/shared/drivers/exti-driver/test_exti_driver_fd.c | 232 | new — STM32F469xx register layout |
| firmware/gateway/integration-tests/exti/main_test_exti.c | ~260 | new |
| firmware/field-device/integration-tests/exti/test_exti_driver_main.c | ~185 | new |

---

## Reused infrastructure

| File | Status | Symbols added |
|------|--------|----------------|
| tests/mocks/stm32f469xx.h | extended | `EXTI0_IRQn`..`EXTI4_IRQn`, `EXTI15_10_IRQn` (EXTI_TypeDef, SYSCFG_TypeDef, NVIC_SetPriority, RCC_APB2ENR_SYSCFGEN already existed from TouchscreenDriver) |
| tests/mocks/stm32l475xx.h | extended | `SYSCFG_TypeDef`, `EXTI_TypeDef` (multi-bank), `RCC_APB2ENR_SYSCFGEN`, `EXTI0_IRQn`..`EXTI4_IRQn`/`EXTI9_5_IRQn`/`EXTI15_10_IRQn`, `NVIC_SetPriority`, `g_mock_nvic_priority[]` |
| tests/mocks/stm32l475_cmsis_mock.c | extended | storage + reset for the above, `NVIC_SetPriority` implementation |
| tests/project.yml | extended | `shared/**` test path; `:test_exti_driver_fd:` / `:test_exti_driver_gw:` defines blocks; `../firmware/shared/` glob widened to `../firmware/shared/**` (was non-recursive, so it never actually reached any subdirectory of `firmware/shared/` — a latent gap this module exposed) |

LedDriver (existing, already-built ADT module) reused as-is by the Field
Device integration test main for LED signalling — no changes needed.

---

## Unit test results

| Test ID | Description | Result |
|---------|-------------|--------|
| EXTI-T01 | configure(1, PORT_E, RISING) | PASS (both boards) |
| EXTI-T02 | configure(8, PORT_C, RISING) | PASS (both boards) |
| EXTI-T03 | configure(11, PORT_C, RISING) | PASS (both boards) |
| EXTI-T04 | duplicate configure -> CONFLICT | PASS (both boards) |
| EXTI-T05 | configure(16, ...) -> INVALID_ARG | PASS (both boards) |
| EXTI-T06 | configure(5, PORT_A, FALLING) | PASS (both boards) |
| EXTI-T07 | configure(3, PORT_B, BOTH) | PASS (both boards) |
| EXTI-T08 | enable(1, 6) after configure | PASS (both boards) |
| EXTI-T09 | enable without configure -> NOT_CONFIGURED | PASS (both boards) |
| EXTI-T10 | disable after configure+enable | PASS (both boards) |
| EXTI-T11 | disable without configure -> NOT_CONFIGURED | PASS (both boards) |
| EXTI-T12 | clear_pending(8) | PASS (both boards) |
| EXTI-T13 | EXTICR field isolation | PASS (both boards) |
| EXTI-T14 | invalid port -> INVALID_ARG | PASS (both boards) |
| EXTI-T15 | invalid edge -> INVALID_ARG | PASS (both boards) |
| EXTI-T16 | reset_for_test clears bitmap | PASS (both boards) |
| EXTI-T17 | SYSCFG clock enabled on first configure | PASS (both boards) |

**Total:** 34 pass (17 x 2 boards), 0 ignored.

Full-suite regression check (`ceedling test:all`): 606 tested, 599 pass,
0 fail, 7 ignored (all pre-existing deferrals, unrelated to this module).

---

## Integration test — expected behaviour

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | Gateway: UART reports TC-HW-EXTI-001..005 all PASS | configure/enable/conflict/invalid-arg/not-configured paths on real SYSCFG/EXTI/NVIC |
| 2 | Gateway: jumper PA1 to 3V3 -> LD2 toggles, UART logs incrementing count | EXTI1_IRQHandler -> exti_clear_pending() -> ISR callback path end-to-end |
| 3 | Field Device: automated checks pass silently (no fast LD3 blink) | same five checks against F469 single-bank registers |
| 4 | Field Device: press USER button (PA0) -> LD3 toggles | EXTI0_IRQHandler -> exti_clear_pending() path on the second board |

---

## Deviations from companion

None. `exti-driver.md` v1.0 (Phase H ready) was implemented as specified.

Two related documents were corrected as a prerequisite (see commit
`fde9f64`): `docs/lld/drivers/wifi-driver.md` had been independently
updated (uncommitted, on disk) to WIFI-D9 "fold EXTI into GpioDriver" —
directly contradicting `exti-driver.md`'s EXTI-O4 instruction that
WifiDriver should consume the real ExtiDriver. User resolved the
conflict in favour of the real ExtiDriver; wifi-driver.md was corrected
to match (USES table, ISR sketch, two-phase init, decisions log).
`components.md`'s WifiDriver USES row was also missing `GpioDriver`
(pre-existing gap, unrelated to ExtiDriver) — added alongside.

---

## Open items

- Field Device's CubeIDE project (`firmware/field-device/.project` /
  `.cproject`) was missing a linked `shared` resource and `sourcePath`
  entry that Gateway's project already had — added locally to unblock
  a hardware build, but both files are gitignored (they embed
  machine-specific absolute paths), so this fix is **not** part of the
  committed changes. Anyone building Field Device on a fresh checkout
  will need to add the same linked resource manually, or this should
  be captured in project setup documentation.
- WIFI-O1 (TLS strategy) and WIFI-O4 (WifiTask API surface) remain open
  in wifi-driver.md, deferred to their own companion stages as before —
  unaffected by this session's changes.

---

## PR title

feat: ExtiDriver — shared EXTI interrupt line configuration for both boards

---

## PR description

## What this PR contains

- firmware/shared/drivers/exti/{exti_driver.h,exti_driver.c} — sole owner of SYSCFG_EXTICRx, EXTI trigger/mask, and NVIC enable/disable for EXTI lines, shared between both boards
- tests/shared/drivers/exti-driver/{test_exti_driver_gw.c,test_exti_driver_fd.c} — 17 unit tests each, run against both boards' mock register layouts
- firmware/gateway/integration-tests/exti/main_test_exti.c and firmware/field-device/integration-tests/exti/test_exti_driver_main.c — hardware bring-up mains
- docs/hld/components.md — WifiDriver USES corrected to include GpioDriver
- docs/lld/drivers/wifi-driver.md — WIFI-D9 corrected: EXTI ownership belongs to ExtiDriver, not GpioDriver
- Extended stubs/mocks: tests/mocks/stm32f469xx.h, tests/mocks/stm32l475xx.h, tests/mocks/stm32l475_cmsis_mock.c, tests/project.yml

## Design decisions

- Platform singleton, not ADT (EXTI-D6) — the MCU has exactly one EXTI peripheral and one SYSCFG block.
- Register-name differences (L475 multi-bank vs F469 single-bank) resolved via compile-time macro aliases in exti_driver.c — the only platform-conditional code in the module (EXTI-D3).
- Conflict-detection bitmap (EXTI-D2) catches two drivers claiming the same EXTI line at configure time rather than corrupting SYSCFG_EXTICRx silently.

## Test evidence

Unity host tests: 34/34 pass (17 test cases x 2 boards), 0 fail, 0 ignore for this module.
Full regression (`ceedling test:all`): 606 tested, 599 pass, 0 fail, 7 ignored (pre-existing, unrelated).
`scripts/test-module.ps1 -Module exti_driver_gw` and `-Module exti_driver_fd`: ALL CHECKS PASSED (Ceedling + cppcheck + clang-format).
Hardware integration validated: deferred — no physical board attached this session.

## Open items carried forward

- Field Device CubeIDE `.project`/`.cproject` shared-folder linkage (gitignored, machine-local — see Open items above).
- WIFI-O1, WIFI-O4 (unrelated to this module, tracked in wifi-driver.md).
