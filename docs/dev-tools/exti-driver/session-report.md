# Session Report — ExtiDriver

**Date:** 2026-07-06
**Branch:** feature/phase-4-shared-exti-driver
**Companion:** docs/lld/drivers/exti-driver.md (Phase H ready, v1.1 — Gateway-only, EXTI-O6 defers Field Device)

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| firmware/gateway/drivers/exti/exti_driver.h | 137 | new |
| firmware/gateway/drivers/exti/exti_driver.c | 178 | new |
| tests/gateway/drivers/exti_driver/test_exti_driver.c | 232 | new — STM32L475xx register layout |
| firmware/gateway/integration-tests/exti/main_test_exti.c | ~260 | new |

The module was initially built at `firmware/shared/drivers/exti/` with a
parallel Field Device unit test and integration test main, matching the
companion draft the user supplied, which specified `Board: Both`. Partway
through the session the user redirected: ExtiDriver should live under
`firmware/gateway/` only, since Field Device has no real consumer yet
(TouchscreenDriver, its only FD consumer, is unimplemented). The FD test,
FD integration main, and FD-only mock additions were removed; see
"Deviations" below.

---

## Reused infrastructure

| File | Status | Symbols added |
|------|--------|----------------|
| tests/mocks/stm32l475xx.h | extended | `SYSCFG_TypeDef`, `EXTI_TypeDef` (multi-bank), `RCC_APB2ENR_SYSCFGEN`, `EXTI0_IRQn`..`EXTI4_IRQn`/`EXTI9_5_IRQn`/`EXTI15_10_IRQn`, `NVIC_SetPriority`, `g_mock_nvic_priority[]` |
| tests/mocks/stm32l475_cmsis_mock.c | extended | storage + reset for the above, `NVIC_SetPriority` implementation |
| tests/project.yml | extended | single `:test_exti_driver:` (STM32L475xx) defines block — `gateway/drivers/**` already covers the test path, no new path entries needed |

`tests/mocks/stm32f469xx.h` was extended (new `IRQn_Type` entries) and
then reverted in the same session once ExtiDriver moved to Gateway-only
— it's back to its pre-session state (only `EXTI9_5_IRQn`, from
TouchscreenDriver).

---

## Unit test results

| Test ID | Description | Result |
|---------|-------------|--------|
| EXTI-T01 | configure(1, PORT_E, RISING) | PASS |
| EXTI-T02 | configure(8, PORT_C, RISING) | PASS |
| EXTI-T03 | configure(11, PORT_C, RISING) | PASS |
| EXTI-T04 | duplicate configure -> CONFLICT | PASS |
| EXTI-T05 | configure(16, ...) -> INVALID_ARG | PASS |
| EXTI-T06 | configure(5, PORT_A, FALLING) | PASS |
| EXTI-T07 | configure(3, PORT_B, BOTH) | PASS |
| EXTI-T08 | enable(1, 6) after configure | PASS |
| EXTI-T09 | enable without configure -> NOT_CONFIGURED | PASS |
| EXTI-T10 | disable after configure+enable | PASS |
| EXTI-T11 | disable without configure -> NOT_CONFIGURED | PASS |
| EXTI-T12 | clear_pending(8) | PASS |
| EXTI-T13 | EXTICR field isolation | PASS |
| EXTI-T14 | invalid port -> INVALID_ARG | PASS |
| EXTI-T15 | invalid edge -> INVALID_ARG | PASS |
| EXTI-T16 | reset_for_test clears bitmap | PASS |
| EXTI-T17 | SYSCFG clock enabled on first configure | PASS |

**Total:** 17 pass, 0 ignored.

Full-suite regression check (`ceedling test:all`): 589 tested, 582 pass,
0 fail, 7 ignored (all pre-existing deferrals, unrelated to this module).

---

## Integration test — expected behaviour

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | UART reports TC-HW-EXTI-001..005 all PASS | configure/enable/conflict/invalid-arg/not-configured paths on real SYSCFG/EXTI/NVIC |
| 2 | Jumper PA1 to 3V3 -> LD2 toggles, UART logs incrementing count | EXTI1_IRQHandler -> exti_clear_pending() -> ISR callback path end-to-end |

---

## Deviations from companion

**Scope reduced from dual-board to Gateway-only, by explicit user
instruction mid-session.** The companion document originally supplied
specified `Board: Both`, `firmware/shared/drivers/exti/`, and a
TouchscreenDriver (Field Device) consumer row. The user asked why the
module was placed under `firmware/shared/` and directed it to
`firmware/gateway/drivers/exti/` instead, since Field Device has no real
consumer yet. Resolution, applied in full:

- Moved `firmware/shared/drivers/exti/` -> `firmware/gateway/drivers/exti/`.
- Removed the FD unit test (`test_exti_driver_fd.c`), FD integration test
  main, and the FD-only `IRQn_Type` mock additions in `stm32f469xx.h`.
- Reverted `components.md`'s Field Device ExtiDriver entry and
  TouchscreenDriver's USES (back to `I2cDriver` only).
- Rewrote `docs/lld/drivers/exti-driver.md` in the repo — which was
  still the stale Draft v0.1 from an earlier session, not the Phase H
  v1.0 content the user actually supplied this session — to the
  Phase H content, scoped to Gateway-only, with a new open item
  (EXTI-O6) documenting exactly how to relocate to `firmware/shared/`
  and re-add the FD companion-doc updates when TouchscreenDriver is
  eventually implemented. No production logic changes were needed for
  the scope reduction — the L475/F469 register-name alias block in
  `exti_driver.c` already resolves both targets; only the physical file
  location and the doc/test/mock surface area changed.

Separately, and unrelated to the shared-vs-gateway question:
`docs/lld/drivers/wifi-driver.md` had been independently updated
(uncommitted, on disk) to WIFI-D9 "fold EXTI into GpioDriver" —
contradicting `exti-driver.md`'s EXTI-O4 instruction that WifiDriver
should consume the real ExtiDriver. The user resolved this in favour of
the real ExtiDriver; wifi-driver.md was corrected to match (USES table,
ISR sketch, two-phase init, decisions log). `components.md`'s WifiDriver
USES row was also missing `GpioDriver` (pre-existing gap) — added
alongside.

---

## Open items

- EXTI-O6 (new, in exti-driver.md): Field Device deployment is deferred
  until TouchscreenDriver is scheduled for implementation. The
  companion documents the exact relocation steps.
- WIFI-O1 (TLS strategy) and WIFI-O4 (WifiTask API surface) remain open
  in wifi-driver.md, deferred to their own companion stages as before —
  unaffected by this session's changes.

---

## PR title

feat: implement ExtiDriver for Gateway EXTI interrupt configuration

---

## PR description

## What this PR contains

- firmware/gateway/drivers/exti/{exti_driver.h,exti_driver.c} — sole owner of SYSCFG_EXTICRx, EXTI trigger/mask, and NVIC enable/disable for EXTI lines on the Gateway
- tests/gateway/drivers/exti_driver/test_exti_driver.c — 17 unit tests against the L475 mock register layout
- firmware/gateway/integration-tests/exti/main_test_exti.c — hardware bring-up main
- docs/hld/components.md — WifiDriver USES corrected to include GpioDriver
- docs/lld/drivers/wifi-driver.md — WIFI-D9 corrected: EXTI ownership belongs to ExtiDriver, not GpioDriver
- docs/lld/drivers/exti-driver.md — replaced stale Draft v0.1 with Phase H v1.1, scoped to Gateway-only (EXTI-O6 documents the deferred Field Device path)
- Extended mocks: tests/mocks/stm32l475xx.h, tests/mocks/stm32l475_cmsis_mock.c, tests/project.yml

## Design decisions

- Platform singleton, not ADT (EXTI-D6) — the MCU has exactly one EXTI peripheral and one SYSCFG block.
- Gateway-only for now (EXTI-D7) — the register-name alias block already resolves both L475 and F469, so this is a location/scope choice, not a design constraint; relocating to `firmware/shared/` later needs no logic changes (see EXTI-O6).
- Conflict-detection bitmap (EXTI-D2) catches two drivers claiming the same EXTI line at configure time rather than corrupting SYSCFG_EXTICRx silently.

## Test evidence

Unity host tests: 17/17 pass, 0 fail, 0 ignore for this module.
Full regression (`ceedling test:all`): 589 tested, 582 pass, 0 fail, 7 ignored (pre-existing, unrelated).
`scripts/test-module.ps1 -Module exti_driver`: ALL CHECKS PASSED (Ceedling + cppcheck + clang-format).
Hardware integration validated: deferred — no physical board attached this session.

## Open items carried forward

- EXTI-O6 (Field Device deployment, deferred until TouchscreenDriver exists).
- WIFI-O1, WIFI-O4 (unrelated to this module, tracked in wifi-driver.md).
