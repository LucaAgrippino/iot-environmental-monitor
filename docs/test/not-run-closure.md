# Closing the 51 requirements without a result (proposal, 2026-11-13)

**Status: proposal for the project owner. Nothing here is a verdict.** Source: `verification-results.md` (51 requirements
"Not run yet" after PR #210), `verification-methods.csv`, the suites under `verification/suites/`, plan section 8
rule 1 (every requirement ends as pass, fail, waived or not applicable) and the table in section 12 (a waiver needs a
reason and the owner's signature).

Outcomes: **OP** operator checklist (wave 7, last, with the owner); **TAG** an existing passing case already measures it,
add the tag after reading the case; **CASE** write and run a Robot case; **GDB** needs the boards on the PC (framebuffer
and symbol reads through gdb-multiarch, which the Pi lacks); **WAIVE** proposed waiver, owner decides; **N/A** the
requirement text points elsewhere.

| Requirement | Outcome | Note |
|---|---|---|
| REQ-LD-110, LD-120, LD-130, LD-155, LD-150 | OP | OP-01..05 (touch) |
| REQ-LD-040, LD-190 | OP | OP-06/07 (forced sensor fault) |
| REQ-NF-214 | OP | OP-08 (power cut during a config save) |
| REQ-MB-140 | TAG + small change | TC-VER-CL-009 proves `bad_format` / `unknown_command`; it does not check the log line, so extend it first |
| REQ-DM-021 | TAG (to confirm) | TC-VER-CL-010: a restart that is never confirmed does nothing for 40 s. Check that "declines" in the requirement means "no confirmation" |
| REQ-DM-030 | CASE | TC-VER-CL-011 shows the Gateway comes back; it does not read a "restart success" report. Add the check |
| REQ-DM-071, REQ-NF-204 | TAG (to confirm) | OTA suite, `selfcheckfail` case: trial boot, self-check fails, rollback. Needs the run time of the rollback against 10 s |
| REQ-DM-052, DM-053 | CASE | OTA suite `resume` case covers a broker outage; add a "3 failures discard the partial image" check and the log line |
| REQ-DM-054, DM-056, DM-060, DM-062 | CASE | second update command while one runs; update from a wrong source; bad size, format, header; log line on validation failure. The `damaged` case checks the signature path (REQ-DM-080), not these |
| REQ-DM-100 | CASE | profile update by console and by remote configuration |
| REQ-TS-020, TS-050, NF-210, NF-211 | CASE | RTC across a reset; time retry after the network returns; `ntp-interval-min`; Field Device time written after each sync |
| REQ-NF-209 | CASE | reconnect after 2 s, doubling to 60 s: block the WiFi or the broker and read the console log |
| REQ-NF-106, NF-100 | CASE | on-board timestamps (health or console), not the wire |
| REQ-NF-110 | TAG (to confirm) | the passing probe case PR-003 sees the default one-burst-per-second polling |
| REQ-MB-000, MB-080, MB-090, SA-110, SA-160, LI-070 | CASE | register changes with a new measurement; remote command to the holding registers and its result back to the cloud; same filter for periodic and on-demand readings; error flag on an invalid channel (needs a forced fault, so this one may join OP-06); `prov set modbus-addr` |
| REQ-NF-305 | CASE | a plain-TCP connection to the broker is refused (host side check against the bench broker) |
| REQ-NF-500 | CASE or N/A | log line format (level, time, module): read it from the console |
| REQ-SA-010, SA-030, LD-020, LD-030, LD-060, LD-180, LD-195, LD-270, NF-108 | GDB | the Field Device display and boot cases; run together once the boards sit on the PC |
| REQ-BF-020 | WAIVE | 254 entries need about 4 h of telemetry; the ring's capacity and overwrite are covered by the host tests and the hardware cases in `circular-flash-log.md` |
| REQ-MB-060 | WAIVE | four sends of a failed request cannot be told apart on the wire here; the host test of the poller is the evidence |
| REQ-NF-107 | WAIVE | "starts within 500 ms" is not measurable from the reply (TC-VER-CL-008 logs the latency of the acknowledgement) |
| REQ-NF-202 | WAIVE | openocd freezes the IWDG, so a watchdog restart cannot be forced from the bench; the stall build resets at 31-35 s and the boot time is covered by REQ-NF-213 |
| REQ-MB-130 | WAIVE (or CASE) | the wording "log the offending identifier and slave address" is unclear; decide the expected log line first |
| REQ-NF-212 | N/A | the text says "See REQ-TS-040": the requirement is carried by TS-040 |

Counts: OP 8, TAG 5 (to confirm), CASE 23, GDB 9, WAIVE 5, N/A 1 = 51 (some cases cover several requirements, so the
number of cases is smaller).

## Decisions for the owner

1. Approve or change the five waivers (BF-020, MB-060, NF-107, NF-202, MB-130) and the N/A (NF-212).
2. Boards on the PC for the nine GDB requirements: yes or no (or waive the framebuffer ones that the operator checklist
   already shows by eye).
3. DEF-012: fix before the baseline tag, or accept as a known limitation (see `defects.md`).

## Progress

- **2026-11-13, owner decisions:** waivers (BF-020, MB-060, NF-107, NF-202, MB-130) and the N/A (NF-212) approved; the
  display cases run on the Raspberry Pi (no gdb: `scripts/gen-verify-symbols.py` resolves the one symbol needed on the PC);
  DEF-012 is fixed (PR #211).
- **Display group, first batch, run on the Pi (TC-VER-FD-014..018):** REQ-LD-030, LD-020, LD-180, LD-195, SA-030 pass.
  LD-195: the design keeps the "accumulating" badge until the 5 min window is full, longer than the SRS wording; both agree
  right after a boot. SA-030: the boot log has one line for the SensorService, the three channels show in `sensors`.
- **Display group, still open:** LD-060 (the "waiting" overlay is brief; timing to capture), LD-270 (needs the Error
  state), NF-108 (refresh rate), SA-010 (a failed configuration read).

## Owner decisions, 2026-11-13 (second session)

- **REQ-NF-110 (poll the field node at a default 1 Hz): the default Modbus poll period stays 200 ms.** The Gateway polls the Field Device
  every 200 ms (`DEFAULT_MODBUS_POLL_PERIOD_MS`, `config_service.c`) so that an alarm raised on the Field Device reaches the cloud within
  500 ms (REQ-NF-113, measured median 136 ms). At 1 Hz the alarm latency would be about 1.2 s. The two requirements cannot both hold with
  one poll period; the owner chose REQ-NF-113. REQ-NF-110 is therefore **waived**. The Field Device samples its sensors at 1 Hz by default
  (`polling-interval-ms`, REQ-SA-010), which is the rate that REQ-SA-140 refers to. The SRS wording should be amended to "the Field Device
  samples at 1 Hz by default; the Gateway polls it every 200 ms".
- **OTA closure suite (suite 11) approved to run.** It uploads to the Gateway but never reaches activation, so the firmware version stays.
- **REQ-SA-010** (default polling interval when the configuration cannot be read): the Field Device keeps its configuration in external QSPI
  flash, which the debugger cannot corrupt, so the failure branch cannot be forced on the bench. The evidence is at unit level: the host tests
  TC-CSVC-001 and TC-CSVC-004 now assert exactly 1000 ms when nothing, or something invalid, is stored. Pending the owner's acceptance.
- **REQ-MB-060** is no longer waived: it is verified on the RS-485 line (TC-VER-PR-004) after DEF-013.

## Found and changed during the closure work

- **DEF-013** (Modbus retries were 1, the requirement says 3) and **DEF-014** (publish periods saved over a restart were ignored): see `defects.md`.
- **REQ-MB-080 / MB-090** had no cloud entry point: `fd_ack_alarm` and `fd_reset_metrics` now write the Field Device's command register and
  report its answer (cloud-publisher-lld.md, CP-D39). **REQ-DM-100** had no remote path: `profile_add` and `profile_remove` (CP-D40).
- **REQ-NF-108**: the Sensors screen redrew its values on every UI tick (about 14 Hz measured, TC-VER-FD-019); it is now gated to the 200 ms
  `LCD_REFRESH_MS` (5 Hz) with a drift-free schedule; the UI task still ticks every 40 ms for the touch response and the blink phases.
- **Measurable timing** was added for REQ-NF-100 / NF-106 (the longest sensor cycle of each board, and the Gateway's request-to-queued log).

## Status at the end of the closure session (2026-11-14)

Report: 98 of 98 test cases pass; of 207 requirements 179 are verified by test, 10 by hand (inspection or earlier bench results), 5 waived by the
owner (BF-020, MB-130, NF-107, NF-202, NF-110) and 1 not applicable (NF-212). 12 are still without a result:

| Requirement | Why | Next |
|---|---|---|
| LD-040, LD-110, LD-120, LD-130, LD-150, LD-155, LD-190, NF-214 | operator checklist (touch, forced sensor fault, power cut) | wave 7, with the owner, last |
| LD-060 ("waiting for data" until the first reading) | the Field Device always has a reading within a second of the scheduler start, and the splash covers that time; the state cannot be held on the bench | unit test TC-LCDUI-004 (the cards start in the WAITING state); owner to accept |
| SA-010 (default 1 s interval if the configuration cannot be read) | the Field Device's configuration is in external QSPI flash; the failure cannot be forced | unit tests TC-CSVC-001/004 assert exactly 1000 ms; owner to accept |
| NF-204 (rollback within 10 s) | needs a timed run of the self-check-fail update (suite 9, OTA-001), which changes the Gateway's firmware version | owner's go-ahead for that run |
| NF-209 (reconnect after 2 s, doubling to 60 s) | MQTT layer: TC-VER-CL-024 (run result below) | the Wi-Fi layer is not forced on the bench |

Closure runs of 2026-11-13/14: suites 10 (time, remote commands, Modbus retries, profiles, log format, TLS only, timing), 11 (OTA refusals,
stall, busy, authentication) and the FD display cases. The merged report is built from an explicit list of valid runs (the killed runs of 10 and
12 November and the abandoned development runs of 10 November are left out); the two test ids that were reused by mistake (GW-016 and GW-017)
were relabelled in copies of the affected result files (GW-018 and GW-019) before merging, the original result files are untouched.
