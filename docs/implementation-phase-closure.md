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
| D-1 | REQ-CC-030/040 said the telemetry and health intervals are adjustable "via the LCD configuration screen". They are Gateway parameters and the Gateway has no LCD | **Resolved (SRS v1.7):** the local adjustment is by the serial console |
| D-2 | REQ-LD-100 listed "display settings" among the configurable parameters. The Config screen has the polling rate and the six alarm thresholds; no display setting exists | **Resolved (SRS v1.7):** "display settings" dropped |
| D-3 | The keypad has a `+/-` key that the design mockup lacks (negative temperature thresholds) | Accepted (LcdUi LLD LCD-O2) |
| D-4 | Free RAM on the Field Device is a static figure (top of RAM minus `.bss`), not a live heap measurement: there is no heap | Accepted (LCD-O7) |
| D-5 | The Field Device CubeIDE `Release` configuration was stale (no entry point) | **Resolved:** repaired into the no-display production image, built and checked in CI; the display variant is built by `scripts/build-fd-app.sh GUI=1` |
| D-6 | The Gateway has 1 224 B of spare main RAM | Accepted, with a rule: every new static buffer needs a budget check (`build-gateway-app.sh` fails the link when the image does not fit) |
| D-8 | REQ-LI-040, -060, -080 and -160 were not met (found at verification planning): the console accepted WiFi credentials, certificates and Modbus line settings but nothing stored or applied them, and the self-test result was not persisted. This report first said no deferred item affected a requirement; that was wrong | **Resolved 2026-11-08:** CertStore (`lld/middleware/cert-store.md`), `modbus_uart_configure`, stored self-test flags; verified on the boards |
| D-7 | `.cproject` files were edited to keep harnesses out of the Debug builds (ADC, GUI exclusions) | Disclosed; checked by `check-build-exclusions` |

## 5. Open items: triage

| Group | Items | Disposition |
|---|---|---|
| Wiring and integration | WP4 main() both boards, WP1 CloudPublisher reconciliation, WP2 arena, WP6 CMD_SET_TIME, TS-O5, TS-O6, LC-O13, LC-O15 | **Closed** |
| Display and health | LCD-O1 (splash), LCD-O3, GUI bring-up, resource fields, link state | **Closed** (LcdUi LLD v0.4) |
| Two-board and system behaviour | WP7 (RS-485 integration), REQ-NF-113 end-to-end latency, soak runs | **Verification phase** |
| Bench measurements | I2C TIMINGR, DRDY pins PC8/PD11 and real ODRs, own harnesses for I2C and sensor groups, CFL-O8 (erase latency against the 500 ms alarm budget), TC-HW-CP-014..017, MBP-010 | **Verification phase** (they are measurements) |
| Bootloader hardening | BL-O2 (rollback counter now counts consecutive rollbacks), BL-O3 (the application authenticates, the bootloader checks integrity), BL-O4 (write-protect pages 0 to 7: `scripts/gateway-bootloader-wrp.sh`, measured on the board, mass erase is refused while set) | **Closed** (bootloader LLD). The protection is left off during bench work; `set` at the final flash |
| Code hygiene | LC-O6 (unused `USE_GUI` hooks removed from the Field Device lifecycle), LC-O10, LC-O14, and the open-item rows that were done but not marked (LC-O8, LC-O15, CP-O9, CS-O7 FD, GCS-O10, ADC-O1); AdcDriver, CpuDriver and WatchdogDriver added to `components.md` | **Closed** |
| Feature-sized items left | CFG-O8 and TS-O4 (TimeService does not read `IConfigProvider`; it uses the defaults, which equal the configuration defaults; no requirement asks for a configurable NTP interval), CS-O5/O8, GCS-O9 and O13 | **Deferred**: no SRS requirement needs them |
| **Console provisioning gaps** | GCS-O4 (WiFi credentials have no back-end), CS-O6/GCS-O5 (certificate provisioning, Modbus line settings), GCS-O6 (self-test result not persisted) | **Closed 2026-11-08** (see D-8). Not deferrable as first written. Found at verification planning (2026-11-08):** these leave REQ-LI-040, -060, -080 and -160 unmet. Moved to D-8 |
| Open design notes | CP-O4 (cloud schema confirmation), COMP-DIAG-01 (Visual Paradigm component diagram), LCD-O8 (check that the bench sensors are real) | **Verification phase** / documentation |
| Documentation checks | Doxygen run (REQ-NF-505), full BARR-C review (REQ-NF-502) | **Verification phase** |

## 6. Handoff to verification

Suggested order, from the existing plans (`docs/test/*`, the work packages WP7, WP10, WP13):

1. (Done: D-1, D-2 and D-5 were decided and applied, SRS v1.7.)
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
