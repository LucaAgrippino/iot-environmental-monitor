# Implementation phase: closure report

**Date:** 2026-11-08 · **Repository state:** `main` after PR #175 (closure branch `chore/close-implementation-phase`)
**Scope:** both boards of the IoT Environmental Monitor (Gateway B-L475E-IOT01A, Field Device STM32F469I-DISCO).
**Result:** the implementation phase is complete. What remains is verification and validation, listed in section 6.

## 1. Criteria for closing the phase

| # | Criterion | Result |
|---|---|---|
| C1 | Every HLD module has an implementation and a host test suite | Met: Gateway 36 of 36, Field Device 25 of 25 (`module-implementation-status.md`) |
| C2 | Each board has a production `main()` that creates the task set | Met: `firmware/gateway/Src/main.c`, `firmware/field-device/Src/main.c` |
| C3 | Every SRS requirement is traced to an LLD or to code, and the gaps found were closed or recorded | Met: 207 of 207 with a reference (section 3, section 4) |
| C4 | All host tests pass and CI is green | Met: section 2 |
| C5 | Both production images link within the chip's memory | Met: section 2 |
| C6 | Every open item is closed, deferred with a reason, or moved to verification | Met: section 5 |
| C7 | Documentation matches the code (LLD companions, status table, traceability matrix) | Met: LcdUi LLD v0.4, status table, coverage addendum, regenerated matrix |

## 2. Results

| Measure | Value |
|---|---|
| Host tests, default project (Field Device and shared) | 890 tested, 885 passed, 0 failed, 5 ignored |
| Host tests, Gateway project | 1568 tested, 1565 passed, 0 failed, 3 ignored |
| Gateway image | RAM 97 080 of 98 304 B (98.75 %, **1 224 B spare**), SRAM2 11 440 of 32 768 B, flash 336 608 B of 1 MB |
| Field Device image, product (no display) | RAM 63 272 B of 256 KB, flash within 2 MB |
| Field Device image, with the display (`GUI=1`) | RAM 209 168 B of 256 KB (79.8 %), flash 490 772 B of 2 MB (23.4 %) |
| Requirements traced | 207 of 207 (79 LLD and code, 126 LLD, 2 code) |
| CI checks | build configurations (one `main()`), firmware builds for both boards, GUI image and simulator, clang-format 17, cppcheck, Markdown lint, host tests, HAL encapsulation (added at closure) |

Hardware evidence recorded during the phase (per module in `module-implementation-status.md` and the LLD result
tables): the Gateway chain from drivers to OTA (OTA 001..005 pass), the two-board time push, the CPU panic report and the
watchdog on both boards, the Field Device display (every screen, touch checked by hand), and `TC-HW-ADC-001`/`002`.

## 3. Work done at closure

The audit compared the SRS with the code and the matrix with the tree. It found three gaps that were real, not missing labels:

| Finding | Fix |
|---|---|
| **REQ-NF-114:** the polling interval was accepted up to 60 s; the SRS allows 100 ms to 10 s | Range set to 100 ms .. 10 s in the Field Device and Gateway config services, the register map and the LCD field; boundary tests updated |
| **REQ-SA-220:** a history entry had no acquisition timestamp | `ui_history` stores one per channel and sample; `ui_history_get_time()`; tests |
| **REQ-SA-240:** a stale reading was recorded as valid | A sample is recorded invalid when the latest reading is older than the 5 s interval; TC-LCDUI-051 |

Also: `docs/lld/requirement-coverage-addendum.md` maps the requirements the matrix could not link (31 with no reference at all, 11 found only in the HLD; they were
implemented but never named) to their mechanism and evidence; the matrix was regenerated; `check-hal-encapsulation.sh`
was a false-positive generator (comments, build output, `.cproject`) and is now restricted to real `#include` lines and
runs in CI; the status table was brought to the real state.

## 4. Deviations from the SRS and the design

| ID | Deviation | Decision |
|---|---|---|
| D-1 | REQ-CC-030/040 say the telemetry and health intervals are adjustable "via the LCD configuration screen". They are Gateway parameters and the Gateway has no LCD. They are settable from the console and by cloud command | Moved to verification as an SRS wording issue: change the SRS (LCD only for the Field Device's polling rate) or add the fields to the Field Device screen |
| D-2 | REQ-LD-100 lists "display settings" among the configurable parameters. The Config screen has the polling rate and the six alarm thresholds; there is no display setting (no backlight control exists) | Same: either drop "display settings" from the SRS or add a setting |
| D-3 | The keypad has a `+/-` key that the design mockup lacks (negative temperature thresholds) | Accepted (LcdUi LLD LCD-O2) |
| D-4 | Free RAM on the Field Device is a static figure (top of RAM minus `.bss`), not a live heap measurement: there is no heap | Accepted (LCD-O7) |
| D-5 | The Field Device CubeIDE `Release` configuration is stale (no entry point); the production image is built by `scripts/build-fd-app.sh` | Decision for the project owner (`dev-tools/build-configurations.md` section 7) |
| D-6 | The Gateway has 1 224 B of spare main RAM | Accepted, with a rule: every new static buffer needs a budget check (`build-gateway-app.sh` fails the link when the image does not fit) |
| D-7 | `.cproject` files were edited to keep harnesses out of the Debug builds (ADC, GUI exclusions) | Disclosed; checked by `check-build-exclusions` |

## 5. Open items: triage

| Group | Items | Disposition |
|---|---|---|
| Wiring and integration | WP4 main() both boards, WP1 CloudPublisher reconciliation, WP2 arena, WP6 CMD_SET_TIME, TS-O5, TS-O6, LC-O13, LC-O15 | **Closed** |
| Display and health | LCD-O1 (splash), LCD-O3, GUI bring-up, resource fields, link state | **Closed** (LcdUi LLD v0.4) |
| Two-board and system behaviour | WP7 (RS-485 integration), REQ-NF-113 end-to-end latency, soak runs | **Verification phase** |
| Bench measurements | I2C TIMINGR, DRDY pins PC8/PD11 and real ODRs, own harnesses for I2C and sensor groups, CFL-O8 (erase latency against the 500 ms alarm budget), TC-HW-CP-014..017, MBP-010 | **Verification phase** (they are measurements) |
| Bootloader hardening | BL-O2 (lifetime rollback counter), BL-O3, BL-O4 | **Deferred**: hardening beyond the SRS; the update, rollback and power-cut behaviour required by REQ-DM-070..074 is implemented and was tested (25 power-cut points) |
| Code hygiene | LC-O6 (unused `USE_GUI` hooks in the lifecycle), LC-O10 (dead `BOARD_GATEWAY` code in the Field Device lifecycle file), LC-O14 (blank against corrupt ConfigStore), CFG-O8, TS-O4 (consumers of `IConfigProvider`), CS-O4..O8 | **Deferred**: none affects an SRS requirement; each is recorded in its LLD |
| Open design notes | CP-O4 (cloud schema confirmation), COMP-DIAG-01 (Visual Paradigm component diagram), LCD-O8 (check that the bench sensors are real) | **Verification phase** / documentation |
| Documentation checks | Doxygen run (REQ-NF-505), full BARR-C review (REQ-NF-502) | **Verification phase** |
| D-1, D-2, D-5 | see section 4 | Decisions for the project owner |

## 6. Handoff to verification

Suggested order, from the existing plans (`docs/test/*`, the work packages WP7, WP10, WP13):

1. Decide D-1, D-2 and D-5 (SRS wording and the Release configuration).
2. Run the bench measurements of section 5, then the Field Device stand-alone checks and the two-board system test.
3. Run the requirement-by-requirement verification from `docs/test/requirements-traceability.md`, marking each
   requirement with the test case and result; the `Verify` entries of the coverage addendum are the first candidates.
4. Run the Doxygen and BARR-C checks.
5. Optional: an operator console on the Raspberry Pi as test tooling, and a one-off connection to AWS IoT Core as a
   conformance check (neither is an SRS deliverable).

## 7. Sign-off

| Role | Name | Date | Decision |
|---|---|---|---|
| Project owner | | | Implementation phase closed: yes / no |
