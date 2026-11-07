# Verification plan

**Version:** 0.1 (draft for review) · **Date:** 2026-11-08 · **Baseline:** git tag `phase-4-implementation-complete`
**Scope:** the IoT Environmental Monitor, Gateway (B-L475E-IOT01A) and Field Device (STM32F469I-DISCO), against SRS v1.7 (207 requirements).

## 1. Purpose

This plan says how the system is verified against its requirements: which method for each requirement, on which
level, with which tools and environment, when the work may start and when it is finished, and how defects are
handled. It follows the V-model: each design level has a verification level on the right side.

| Design level | Verification level | Status at the baseline |
|---|---|---|
| Detailed design (LLD) | Unit: host tests (Ceedling, Unity) | Done. About 2 450 tests run in CI. Their results are re-run and credited as evidence |
| Architecture (HLD) | Integration: modules together, board with board | Bring-up harnesses (TC-HW-*) exist per module. The two-board cases are written (`two-board-integration.md`) and not yet run |
| Requirements (SRS) | System verification | **This plan** |
| Stakeholder needs (use cases) | Validation (acceptance) | Planned after verification, section 11 |

Verification asks whether the system was built right, against the specification. Validation asks whether the
right system was built, against what the user needs. They are reported separately.

## 2. References

`docs/SRS.md` (v1.7) · `docs/hld/*` · `docs/lld/**` · `docs/test/requirements-traceability.md` (where each
requirement is designed and implemented) · `docs/lld/requirement-coverage-addendum.md` · `docs/implementation-phase-closure.md` ·
`docs/test/verification-method-table.md` (generated, the method for each requirement) ·
`docs/test/two-board-integration.md` · `docs/test/hardware-check-2026-11-06.md`.

## 3. Verification methods and levels

Each requirement has one of four methods, chosen from its wording and not from the code:

| Method | Meaning | Example |
|---|---|---|
| Test (T) | Run the system or a module and observe | alarm latency at most 500 ms |
| Inspection (I) | Read code, documents or tool output | the register map equals the document |
| Analysis (A) | Derive from a measurement or a calculation | the image fits in 1 MB, from the linker output |
| Demonstration (D) | Operate and observe without a measured limit | navigation between the screens |

`verification-method-table.md` assigns method, level (unit, integration, system), board, automation class and the
way of verifying to every requirement; its statistics are generated. 187 requirements are verified by test, 9 by
analysis, 8 by inspection, 3 by demonstration. 22 are verified at unit level by existing host tests that are
credited as evidence: the plan re-runs them at the baseline and records the result against the requirement.

## 4. Test environment

| Item | Choice |
|---|---|
| Boards | Gateway B-L475E-IOT01A (ST-LINK serial in `scripts/gateway-bootloader-wrp.sh`), Field Device STM32F469I-DISCO, wired over RS-485 |
| Broker | Mosquitto (Docker) with the bench TLS certificates, on the laptop or the Raspberry Pi |
| Raspberry Pi | RS-485 probe and alarm-latency dongle (`scripts/rpi_rs485_probe.py`, `alarm_latency_bench.py`) |
| Debug access | openocd over SWD: reset, flash, memory read, framebuffer capture, GPIO read |
| Consoles | Gateway console at 460 800 baud, Field Device console at 115 200 baud |
| Network | 2.4 GHz WiFi only (the module does not see 5 GHz) |
| Runner | Robot Framework with Python libraries (section 6), run from `verification/` |
| Baseline | A build from the tag `phase-4-implementation-complete`; a code change after the tag means a new baseline and a new run of the affected suites |

The test environment is recorded in each run: git commit, firmware version string of each board, openocd and tool
versions, the date, and the operator.

## 5. Test design

- **Source of the cases:** the requirement wording. A case is written to check what the requirement says, so a
  defect in the code cannot hide in the test.
- **Techniques:** boundary values (100 ms and 10 s polling, 254 buffered entries, 3 and 4 transmissions), state
  transitions (lifecycle, Modbus link up and down), negative and fault-injection cases (CRC errors, forced sensor
  fault, power cut, wrong signature), and endurance runs.
- **Identifiers:** `TC-VER-<area>-<nnn>` (areas: SA, AM, LD, LI, MB, CC, TS, DM, BF, NF). Each case lists the
  requirements it covers. A requirement may have several cases and a case may cover several requirements.
- **Case fields:** ID, requirements, preconditions, steps, expected result, pass criterion, method, automation class.
- **Traceability:** both directions. A requirement with no case, or a case with no requirement, is a finding.

## 6. Tooling and structure

Robot Framework is the executable test specification. Python keyword libraries do the board work.

```text
verification/
  suites/             one .robot file per area: sa, am, ld, li, mb, cc, ts, dm, bf, nf, system
  resources/          shared keywords (boot a board, wait for the console, expect a payload, ...)
  libraries/          Python: BoardControl (openocd flash, reset, memory), Console, FramebufferGolden,
                      MqttProbe, Rs485Probe, LinkBudget (linker output), PowerControl (optional)
  golden/             reference images from the simulator for the display cases
  results/            output.xml, log.html, report.html per run (git-ignored)
tools/                gen-verification-methods.py, a merge script that adds the results to the matrix
```

Each case carries the tags `REQ-xx-nnn` and `TC-VER-...`. `output.xml` is the execution record. A script merges the
results into the traceability matrix (a "Verified" column with result, run date and build), and the report is
generated from the same data. Cases that need an operator (touch, a forced fault, a power cut) are tagged `semi`
and the runner stops with a prompt, so the operator's answer is part of the record.

## 7. Entry criteria

1. The baseline tag exists and the build is reproducible (CI green on the tagged commit).
2. This plan and the method table are reviewed and accepted by the project owner.
3. Both boards are flashed with the baseline images, and the environment of section 4 works (a boot smoke test passes).
4. Any gap found before the run (section 10) has a decision: implement, waive, or change the SRS.

## 8. Exit criteria

1. All 207 requirements have a recorded result: pass, fail, waived or not applicable, none "not run".
2. Every requirement verified by test has at least one passing case, or a written, accepted waiver.
3. No open defect of severity 1 or 2. Defects of severity 3 or 4 are listed with a decision.
4. Every fix after a failure was followed by a regression run of the affected suites and of the smoke suite.
5. The soak runs finished (section 9, wave 6) without an unexplained reset.
6. The verification report is issued and signed off by the project owner.

## 9. Execution order

| Wave | Content | Needs |
|---|---|---|
| 0 | Re-run the host tests and the CI checks on the baseline, credit the 22 unit-level requirements, run the analysis and inspection items (linker output, HAL check, symbol check) | host only |
| 1 | Smoke: boot, versions, console, both boards (also the gate for every later run) | boards |
| 2 | Field Device alone: display against golden images, configuration, persistence, LEDs, splash | FD, SWD |
| 3 | Gateway alone: sensors, console and provisioning, time, buffering, profiles | GW |
| 4 | Two boards: Modbus link, timeouts and retries, time push, remote commands | GW, FD, RS-485 probe |
| 5 | Cloud: telemetry, health, alarms, configuration and control commands, QoS, TLS, reconnect, OTA, rollback, signatures | GW, broker, FD |
| 6 | Endurance: overnight run with both boards and the broker, memory and error counters, reset watch | all |
| 7 | Operator checklist: touch, anything that needs hands, then the remaining `semi` cases | operator |

## 10. Known gaps and risks found while planning

Four requirements (REQ-LI-040, -060, -080, -160) were found to be unmet while planning: the console accepted
WiFi credentials, certificates and Modbus line settings but stored or applied none of them, and the self-test result
was not kept. **They were implemented on 2026-11-08** (CertStore, `modbus_uart_configure`, the stored self-test
flags) and checked on the boards: a wrong stored passphrase keeps the Gateway offline, the stored identity
connects to the broker over mutual TLS after a reset, the UART registers follow the provisioned baud rate and parity, and the
self-test result survives a reset. The cases for these four requirements are planned as normal cases. No requirement
is known to fail at the start of the verification.

Risks the method table marks: LCD-O8 (are the Field Device sensors real or simulated; this decides what the
sensor cases prove), forced sensor faults needed for four cases, timing-sensitive splash captures, the 200 ms against
40 ms refresh question of REQ-NF-108, a controlled power cut for REQ-NF-214, the Gateway's 1 224 bytes of spare RAM
(REQ-NF-400), a BARR-C review not yet done (REQ-NF-502), and a Doxygen run never done (REQ-NF-505).

## 11. Validation (acceptance), after verification

Scenario runs per use case (UC-01 to UC-20) with the Remote Operator and Field Technician actors, using the system
as they would, with a short record per use case. Optional: a one-off connection to a real AWS IoT Core endpoint as a
conformance check of the topics, the client rules and TLS.

## 12. Defect management

| Item | Rule |
|---|---|
| Identifier | `DEF-nnn`, one entry per defect in `docs/test/defects.md`: description, case and requirement, build, severity, status, fix commit |
| Severity | 1 safety or data loss, or a requirement fails with no workaround · 2 a requirement fails, with a workaround · 3 minor deviation, cosmetic · 4 documentation or improvement |
| Life cycle | open, analysed, fixed on a branch, retested, closed. A fix goes through a pull request and CI |
| Regression | After a fix the failed case, its suite, the smoke suite and the units around the changed module are re-run |
| Waiver | A requirement that cannot be met is waived with a reason and the owner's signature, never silently dropped |

## 13. Roles and independence

Project owner: approves the plan, takes the decisions of section 10, signs off. Test author and operator:
writes and runs the cases. Independence rests on the written case: it derives from the requirement and is reviewed
against the method table before it runs.

## 14. Deliverables

`verification-plan.md` (this), `verification-method-table.md` and `.csv` (generated), the Robot suites in
`verification/`, run records, the updated traceability matrix with results, `defects.md`, the verification report
(generated), and the validation record.
