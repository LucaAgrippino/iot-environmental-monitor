# SRS open decisions and incoherences — resolution record

**Date:** 2026-11-06 · **SRS:** v1.3 · **Basis:** the implemented firmware on `main` @ 22734f2 (values read from source, not estimated)

## 1. Decisions taken from the real software

| Item | Was | Now | Evidence |
|---|---|---|---|
| REQ-TS-000, REQ-NF-210 | `[TBD]` sync period | 3600 s default, console-configurable (`ntp-interval-min`) | `TIME_SERVICE_NTP_INTERVAL_S_DEFAULT` in `time_service.h` |
| REQ-NF-211 | `[TBD]` FD sync period | after every NTP sync and whenever the FD link comes up | `prv_push_time_to_fd` after sync; `TS_NOTIFY_BIT_FD_LINK_UP` in `time_service.c` |
| REQ-BF-020 | `[TBD]` entries | 254 | `SAF_CAPACITY` in `Src/main.c`; `CFL_DATA_SECTOR_COUNT` in `circular_flash_log.c` |
| REQ-LI-010 | `[command list TBD]` | SRS Appendix A | console command tables, Gateway and Field Device |
| REQ-NF-213 | TBD in HLD/LLD text | 10 s (the SRS already said 10 s) | LC `lc_init` timer 10 s; Operational about 8 s after reset (`gateway-main.md`) |
| REQ-DM-020 | no timeout in SRS | 30 s | `lc_restart` timer, measured 29 990 ms (TC-HW-LC-009) |
| REQ-LI-000 | 115200 for both | FD 115200; GW 460800 (115200 when built without the console) | `Src/main.c` console comment |
| REQ-SA-090 | "10 most recent readings" | latest reading plus IIR state | SensorService keeps `prev_filtered`; history is REQ-SA-180..240 |
| SS-O1 / SS-O2 | open | closed | SRS fixes α = 0.1 and N = 1 |
| LC-O2, LC-O4, TP-O1 | open (value) | closed | as above |

**Correction.** Earlier today I called REQ-NF-213 inconsistent with a "40 s" boot. That figure (US-O2) is boot to *cloud connected*; REQ-NF-213 asks for Operational, which is about 8 s. The requirement holds.

**Not measured:** RTC drift. 3600 s is a design value (±20 ppm gives about 72 ms per hour). Re-open REQ-NF-210/211 only if a bench measurement disagrees.

## 2. Incoherences that remain (code or SRS must change; your call)

| # | SRS says | Code does | Options |
|---|---|---|---|
| I-1 | REQ-NF-109: 10 s system watchdog | Gateway `main.c` has no watchdog (LC-O8) | implement IWDG feeding from a task-health scheme, or downgrade the requirement |
| I-2 | REQ-NF-209 | **Closed, SRS v1.4:** requirement now states the implemented 2 s to 60 s backoff (CP-D13) | — |
| I-3 | REQ-NF-406/407: 3 MB log + 4 MB buffer on the Gateway | one 1 MB CircularFlashLog ring serves both (254 entries) | the limits are upper bounds, so it complies; if 4 MB of buffering is wanted, the ring must grow |
| I-4 | REQ-LD-*, REQ-SA-180..240 | display stack and history buffer not built | WP9 |
| I-5 | REQ-TS-050/060, REQ-MB-140, REQ-NF-307/308 | checked on hardware 2026-11-06: TS-060, MB-140 (log only) and NF-307 pass; **NF-308 / SA-170 have no command** (`/control` has restart, restart_confirm, reset_metrics only); TS-050 not run | add the on-demand read command or drop the requirement |
| I-6 | REQ-CC-010, REQ-BF-000 | health message lacks stack/heap/CPU/temperature items, and one health message was lost across a broker outage | see the hardware check findings 1 and 2 |

## 3. Reproduce

`python3 -I tools/gen-requirements-matrix.py` regenerates `docs/test/requirements-traceability.md`.
