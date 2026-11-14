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
- **Remote Operator use cases** run through the cloud API (`validation/suites/10_remote_operator_api.robot`) and a real
  headless browser (`validation/suites/20_remote_operator_web.robot`), both Robot Framework; the browser part uses
  Playwright, a free open-source tool. `validation/run.sh` starts them; the account password comes from the
  environment (`VAL_PASS`) and is never written to a file.

## Use cases

| UC | Actor | How it is run | Status |
|---|---|---|---|
| UC-01 View sensor readings | Field Technician | LCD, by hand | **PASS** (V-20); E1 passes with observation **F-6** |
| UC-02 View system status | Field Technician | LCD, by hand | **PASS** (V-21), E1 included |
| UC-03 View active alarms | Field Technician | LCD, by hand | **PASS** (V-22); touch lock-up found on the way: **F-7** |
| UC-04 View device diagnostics | Field Technician | FD console | **PASS** (V-04) |
| UC-05 View telemetry data | Remote Operator | cloud API and browser | **PASS** (V-05); F-2 fixed and re-run |
| UC-06 Monitor device health | Remote Operator | cloud API and browser | **PASS** (V-06) |
| UC-07 Acquire sensor data | System | FD console, SWD fault, cloud | **PASS** (V-07); Gateway side seen in the cloud telemetry (V-10) |
| UC-08 Evaluate sensor alarms | System | FD console, cloud | **PASS** (V-08, V-09) |
| UC-09 Receive alarm notification | Remote Operator | cloud | **PASS** main flow (V-09); E1 (buffering while disconnected) in the end-of-phase session |
| UC-10 Receive sensor telemetry | AWS IoT Core | cloud | **PASS** main flow (V-10); E1 in the end-of-phase session |
| UC-11 Receive device health data | AWS IoT Core | cloud | **PASS** main flow (V-11); E1 in the end-of-phase session |
| UC-12 Receive alarm events | AWS IoT Core | cloud | **PASS** main flow (V-09); E1 in the end-of-phase session |
| UC-13 Synchronise time | System | FD console log | **PASS** (V-13) |
| UC-14 Request sensor reading | Remote Operator | cloud API and browser | **PASS** (V-14); E1/E2 (gateway disconnected) in the end-of-phase session |
| UC-15 Configure operational parameters | FT and RO | console and LCD (FT), cloud (RO) | **PARTIAL**: RO main flow and E3 invalid value pass (V-15); unknown key passes after F-4 (Gateway 1.1.1 flashed, V-18); Field Technician main flow and E4 pass (V-23) |
| UC-16 Provision device | Field Technician | console | Pending |
| UC-17 Request restart device | Remote Operator | cloud API and browser | **PASS** (V-17); a second request no longer counts as the confirmation (**F-5** fixed; V-19) |
| UC-18 Deploy firmware update | Remote Operator | cloud, OTA | Held: OTA only on the owner's go-ahead |
| UC-19 Forward remote commands | AWS IoT Core | cloud | **PASS** (E2 and routing to the Field Device, after F-3); E3 (FD unplugged) in the manual session |
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

**V-05 (UC-05).** The cloud shows the Gateway's temperature, humidity and pressure and the Field Device's three
readings with a timestamp not older than 150 s (API and overview page, screenshot `web-03-overview`). The published
payload carries accelerometer, gyroscope and magnetometer (stored in the raw telemetry). The API device view and the web
overview do not present them: UC-05 step 2 is only partly met (F-2). Browser: no access without signing in (401), a
wrong password is refused, signing in shows the device online, sign out works.

**V-06 (UC-06).** The health message carries uptime, lifecycle state, failure counters and firmware version, is not older
than 150 s, and the web overview lists all 30 health fields.

**V-09 (UC-08, UC-09, UC-12 main flow).** Lowering the Field Device threshold at its console made the cloud store an
alarm `HIGH`, source `field_device`, sensor `temperature`, and list it as active; restoring the threshold stored `CLEAR`
and emptied the active list. The whole scenario, including the two console sessions, took about 8 s each way (the alarm
latency itself, 136 ms, is REQ-NF-113 in verification). The web alarm history lists the alarms with their source.

**V-10 (UC-10).** Since the last start no telemetry gap is longer than the 60 s period. A read request adds an
extra message, so a gap may be shorter.

**V-11 (UC-11).** At least five health messages arrive in five minutes (10 s period in this build).

**V-14 (UC-14).** `read` is acknowledged and a telemetry message newer than the request arrives within 30 s. In the
browser, Read now shows the command with status accepted.

**V-15 (UC-15, Remote Operator).** `telemetry_interval_ms = 30000` is applied (result `applied`) and the following
telemetry gaps are 25 to 36 s; the default 60000 was restored. A value below the range (10) is `rejected` with reason
`invalid_value`, and the previous value stays in force.

**V-17 (UC-17).** One `restart` is acknowledged and nothing happens: after 45 s the uptime kept counting (E2, cancel
without confirmation). `restart` followed by `restart_confirm` restarted the Gateway; it published again 16 s later with
uptime under 200 s and state operational. The browser shows a separate confirm button after Restart…
(screenshot `web-09-restart-confirm`).

## Corrections after the first run (2026-11-14)

- **F-2 fixed** (cloud): the API device view, the telemetry endpoint and the CSV export carry the nine motion and magnetic
  values (read from the stored payload, so the history has them too); the overview shows three cards and the charts tab
  three charts. Re-run: API 3/3, browser 10/10 (screenshot `web-03-overview`).
- **F-3 closed**: the cloud on the Pi was brought to this branch (bundle build, `install.sh`, database kept; a copy
  `iotcloud.sqlite3.bak-pre-deploy` was taken first). `fd_ack_alarm` is now accepted by the cloud, routed by the Gateway
  and acknowledged. The browser suite passes 10/10.
- **F-4 fixed** (Gateway firmware): a rejected configuration command now still reports its request id (test
  CP-T99b). The Gateway on the bench runs release 1.1.0 without this change; the scenario
  `UC-15 E3 A setting the device does not know...` stays failing until a new release is flashed or sent over the air.

**V-20 (UC-01, 2026-11-14, with Luca at the board).** The Sensors screen shows the three values with units and they
refresh. E1: barometer fault injected over SWD: temperature and humidity stay normal, the pressure field shows a dash
in red; after the fault is cleared the value returns. Result: pass. Observations: see F-6.

**V-21 (UC-02).** The Status screen shows the sensor, system, Modbus sections. E1 (system in error, pressure fault): a
banner "Sensor error - last valid reading shown", the failure row in red with an error and the time of the last valid
reading; the Modbus CRC row is yellow and shows 1, equal to the console `status`. After the fault is cleared the banner
disappears and the pill says RUNNING. Result: pass.

**V-22 (UC-03).** With `temp-alarm-high` set to 20.00 over the console the Alarms screen lists the temperature alarm
HIGH, the header pill blinks ALARM, the Sensors screen shows the value with the blinking label. Clearing the alarm
(V-23) returns the pill to RUNNING. Result: pass. While the pressure sensor was invalid the Alarms screen said no
active alarm (correct) while the pill blinked ALARM (F-6).

**V-23 (UC-15, Field Technician).** Main flow on the display: CONFIG, temperature high 20.0 changed to 42.0, APPLY
(confirmation asked), CONFIRM: console `config list` showed 4200 and the alarm cleared. E4: value changed to 30.0, APPLY,
CANCEL: the field shows 42.0 again and the console still shows 4200. Result: pass.

## Findings

1. **F-1 (fixed; V-19) Log flood on a sensor fault.** One failed sensor prints one `Sensor 2 driver error` warning per poll
   (about 14 per second). It hides other console output. Candidate: log the first failure and then once per
   N seconds, with a count. Not a validation failure (UC-07 E1 asks for a logged error); a usability finding.
2. **F-2 (fixed) The operator cannot see accelerometer, gyroscope and magnetometer** (UC-05 step 2, REQ-CC-000). The Gateway
   publishes them and the cloud stores them in the raw payload, but the API device view, the telemetry endpoint and the
   web overview and charts show only temperature, humidity and pressure. Verification passed REQ-CC-000 because the
   message on the broker is complete; validation fails the use case. Fix: expose and show the nine values (cloud only).
3. **F-3 (closed) The cloud installed on the Raspberry Pi is older than `main`.** It accepts four control commands
   (`read`, `restart`, `restart_confirm`, `reset_metrics`); `fd_ack_alarm`, `fd_reset_metrics`, `profile_add` and
   `profile_remove` (PR #213) answer 400. The routing of a command to the Field Device (UC-19 step 3) cannot be
   validated until the Pi runs the current bundle (`cloud/deploy/build-bundle.sh` and `install.sh` as the cloud
   README describes). Needs the owner's decision: it changes the running production service.
4. **F-4 (fixed; Gateway 1.1.1 on the board) A rejected unknown setting is not reported to the operator.** The Gateway answers `unknown_key` without the
   request id, so the cloud cannot match the answer: the command stays pending and then shows `timeout` (UC-15 E3
   asks the operator to learn that the value was not applied). The invalid value case works (`invalid_value`).
   Fix: echo `req` in the `unknown_key` answer (Gateway) or show unmatched config results in the command list (cloud).
5. **F-5 (fixed; V-19) A second `restart` inside the 30 s window is the confirmation.** Was by design (LLD section 10), but an operator
   who presses Restart… twice, or an automation that retries, reboots the Gateway without the explicit confirm. In
   addition a late `restart_confirm` is answered `accepted` although nothing is pending. Consider ignoring a repeated
   `restart` and answering a late confirm with a rejection.
   Fixed: a repeated `restart` is ignored and refused (`rejected`, reason `bad_state`); a `restart_confirm` with nothing
   pending is refused the same way; the LLD section 10 and TC-LC-089 are changed (host tests LC-089, LW-071b/c, CP-T109b).
6. **F-6 A sensor fault is shown as ALARM although no alarm exists** (UC-01 E1, UC-03). The header pill blinks ALARM
   for any Field Device fault (`compute_pill`, `fault_text`) while the Alarms screen and the console list no alarm, so
   the technician sees ALARM and then "no active alarms". Also the banner says "last valid reading shown" but the Sensors
   card shows a dash, not the old value. Proposal: a distinct pill word (FAULT) or a line on the Alarms screen naming
   the sensor error. Design decision for the owner; not changed.
7. **F-7 Touch input stops permanently after an I2C1 bus error** (found at UC-03, 2026-11-14 about 16:22). The display
   kept updating but no touch reached the firmware (no `touch raw` log in a 45 s capture while the screen was touched
   many times). Over SWD: I2C1 CR1 = 0x601 (STOP pending), SR1 = 0x100 (BERR), SR2 = 0x3 (MSL, BUSY), identical on six
   reads; the sensors (another bus) kept reading. `transfer_write`/`transfer_read` return `I2C_ERR_BUS_BUSY` at once when
   BUSY is set and run the recovery (`i2c_bus_recovery`, SWRST) only after a timeout, and the wait loops do not check
   BERR/ARLO, so the state never clears. A reset of the Field Device fixed it. The trigger is not established: the
   bus was clean before and after the same console `config commit`, and after a reset it stayed clean for the rest of
   the session. Proposal: recover (SWRST + bus clear) when BUSY is found on entry and when BERR/ARLO is flagged.

## Open points

- Done 2026-11-14: Gateway 1.1.1 (signed release, built from 7b0dced, version string v0.0.0+7b0dceda) flashed with the owner's go-ahead; identity kept, cloud reconnected; UC-05 x3, UC-14 and UC-15 E3 x2 pass (6/6), including the unknown-key scenario (V-18).
- Done 2026-11-14 (V-19): F-1 and F-5 on the boards with the owner's go-ahead: Gateway 1.1.2 and the Field Device, both built from 09d5588
  (v0.0.0+09d5588f). API scenarios UC-05 x3, UC-14, UC-15 x3 and the new UC-17 E3 pass (8/8). Barometer fault injected for 21 s:
  213 failed reads gave 3 warning lines (reads 1, 100, 200) and one recovery line, instead of about 213 lines. Field Device settings unchanged.
- End-of-phase manual session: LCD use cases (UC-01..03, UC-15 Field Technician), UC-16, the disconnected cases (E1/E2
  of UC-09..UC-14, UC-19 E3), and UC-18/UC-20 (OTA, only with the owner's go-ahead).
- Optional last: a real AWS IoT Core connection.
