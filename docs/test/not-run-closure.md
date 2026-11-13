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
