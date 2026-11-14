# Validation plan and record

Validation asks whether the right system was built, against the use cases (`docs/use-case-descriptions.md`), not the
SRS. It follows verification (`verification-results.md`, closed 2026-11-14). Each use case is run as its actor would
run it, on the real boards, and gets a short record here. Started 2026-11-14.

## Method

- **Cloud side: our own iotcloud on the Raspberry Pi** (Mosquitto + API + web app) stands in for AWS IoT Core. A real
  AWS IoT Core connection is a bonus check at the end of the phase, not a gate.
- **Boards: the real Gateway (signed release 1.1.0) and Field Device (v0.0.0+af4a541e)**, driven through their consoles
  (`validation/fdcmd.py`) and SWD fault injection. Nothing in the firmware is changed to make a scenario pass.
- **Needs a person (touch screen, a browser look, a physical action)** is collected into one session at the end.
- **Needs a login on the cloud API** (Remote Operator use cases): runs as soon as an account exists (see Open points).

## Use cases

| UC | Actor | How it is run | Status |
|---|---|---|---|
| UC-01 View sensor readings | Field Technician | LCD, by hand | End-of-phase manual session |
| UC-02 View system status | Field Technician | LCD, by hand | End-of-phase manual session |
| UC-03 View active alarms | Field Technician | LCD, by hand | End-of-phase manual session |
| UC-04 View device diagnostics | Field Technician | FD console | **PASS** (V-04) |
| UC-05 View telemetry data | Remote Operator | cloud API/web | Needs cloud login |
| UC-06 Monitor device health | Remote Operator | cloud API/web | Needs cloud login |
| UC-07 Acquire sensor data | System | FD console, SWD fault | **PASS** FD side (V-07); Gateway side needs cloud login |
| UC-08 Evaluate sensor alarms | System | FD console | **PASS** FD side (V-08); publish step needs cloud login |
| UC-09 Receive alarm notification | Remote Operator | cloud | Needs cloud login (E1 buffering: broker stop) |
| UC-10 Receive sensor telemetry | AWS IoT Core | cloud | Needs cloud login |
| UC-11 Receive device health data | AWS IoT Core | cloud | Needs cloud login |
| UC-12 Receive alarm events | AWS IoT Core | cloud | Needs cloud login |
| UC-13 Synchronise time | System | FD console log | **PASS** (V-13) |
| UC-14 Request sensor reading | Remote Operator | cloud | Needs cloud login |
| UC-15 Configure operational parameters | FT and RO | console (FT), cloud (RO), LCD | Partly: console path in V-15; LCD and cloud pending |
| UC-16 Provision device | Field Technician | console | Pending |
| UC-17 Request restart device | Remote Operator | cloud | Needs cloud login |
| UC-18 Deploy firmware update | Remote Operator | cloud, OTA | Held: OTA only on the owner's go-ahead |
| UC-19 Forward remote commands | AWS IoT Core | cloud | Needs cloud login |
| UC-20 Verify firmware integrity | System | OTA refusal cases | Held with UC-18 |

## Records

**V-04 (UC-04).** FD console: `version`, `serial`, `sensors`, `status`, `alarms`, `modbus status`, `selftest`,
`selftest-result` each print a diagnostic. E1: an unknown command gives `[ERR] unknown command 'bogus'`; a wrong
argument gives a usage line. Result: pass. Note: `selftest` writes the configuration (ConfigStore seq advanced by
one), harmless but visible.

**V-07 (UC-07, FD side, E1).** The barometer fault flag was set over SWD. `sensors` showed temperature and humidity
valid and `Pressure: INVALID`; `status` counted the failures (75 in 6 s); no alarm was raised for the invalid
sensor; after a reset all three readings were valid again. Result: pass. Observation: the fault produces a
`Sensor 2 driver error` warning about 14 times a second, which floods the console (see Findings).

**V-08 (UC-08, FD side).** With a threshold below the reading, `alarms` showed `Temperature: ACTIVE_HIGH` and the log
`alarm raised HIGH` within 1 s of the commit. Hysteresis: with the threshold set to 22.00 and the reading 21.49
(above 22.00 minus 1.00) the alarm stayed active; restoring 42.00 cleared it (`alarm cleared`). Result: pass.
The Gateway console header shows only the Gateway's own alarms, so the cloud leg is not visible there.

**V-13 (UC-13).** The FD log shows `CMD_SET_TIME applied` after a Gateway start and the FD readings carry the correct
UTC time (the clock matches NTP). The Gateway log shows `NTP sync ok`. Result: pass for the FD write and NTP. The
retry cases (E1, E2) were verified in verification (REQ-TS-*).

**V-15 (UC-15, Field Technician, console path).** `config set`, `config commit` asks `Apply? [y/N]`, `y` saves
(new slot and sequence number), the new thresholds apply at once, and the previous values were restored the same
way. Declining discards (seen earlier in the operator checklist OP-04). The LCD path is in the manual session.

## Findings

1. **F-1 Log flood on a sensor fault.** One failed sensor prints one `Sensor 2 driver error` warning per poll
   (about 14 per second). It hides other console output. Candidate: log the first failure and then once per
   N seconds, with a count. Not a validation failure (UC-07 E1 asks for a logged error); a usability finding.

## Open points

- A cloud API login for the validation work (needs `iotcloud-admin add-user` on the Pi, which needs sudo).
- End-of-phase manual session and the optional real AWS IoT Core check.
