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
| UC-09 Receive alarm notification | Remote Operator | cloud | **PASS** main flow (V-09); E1 alarms buffered and delivered in order (V-25) |
| UC-10 Receive sensor telemetry | AWS IoT Core | cloud | **PASS** main flow (V-10); E1 **PASS** after F-9 (V-25, V-26, V-27) |
| UC-11 Receive device health data | AWS IoT Core | cloud | **PASS** main flow (V-11); E1 **PASS** after F-9 (V-27) |
| UC-12 Receive alarm events | AWS IoT Core | cloud | **PASS** main flow (V-09); E1 passes (V-25) |
| UC-13 Synchronise time | System | FD console log | **PASS** (V-13) |
| UC-14 Request sensor reading | Remote Operator | cloud API and browser | **PASS** (V-14); E1 passes: refused at once (F-10 fixed, V-27); E2 not stageable by hand |
| UC-15 Configure operational parameters | FT and RO | console and LCD (FT), cloud (RO) | **PARTIAL**: RO main flow and E3 invalid value pass (V-15); unknown key passes after F-4 (Gateway 1.1.1 flashed, V-18); Field Technician main flow and E4 pass (V-23) |
| UC-16 Provision device | Field Technician | console | **PASS** with finding **F-8** (V-24) |
| UC-17 Request restart device | Remote Operator | cloud API and browser | **PASS** (V-17); a second request no longer counts as the confirmation (**F-5** fixed; V-19) |
| UC-18 Deploy firmware update | Remote Operator | cloud, OTA | **PASS** (V-29, V-30): the update, E2 (download through a lost connection), E3 (rollback) and E1 (offline device refused) after the fixes F-12 and F-13; E4 not stageable |
| UC-19 Forward remote commands | AWS IoT Core | cloud | **PASS** (E2 and routing to the Field Device, after F-3); E3 passes (V-25); E4 not stageable by hand |
| UC-20 Verify firmware integrity | System | OTA refusal cases | **PASS** (V-29): a tampered image is refused with `hash_mismatch`, an image signed with another key with `sig_invalid`; E1 (read error) not stageable |

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

**V-24 (UC-16, 2026-11-14).** Field Device console: `prov set modbus-addr 0` is refused (`invalid value, 1..247`); staging 5, `prov commit`
and answering `n` gives `Discarded` and `config list` still shows address 1 (E2); staging 1, `prov commit`, `y` gives
`Provisioning applied and persisted` and a save to flash (main flow). Wi-Fi, baud and parity are refused on the Field
Device (`not yet supported (CS-O10)`). Gateway console: an endpoint with a space is refused, a valid one is staged, `n`
gives `Discarded` and a second `prov commit` says `Nothing staged`; the stored identity (CA, certificate, key) and the
Wi-Fi setting are intact and the Gateway stayed connected. Not run: E1 (an apply failure) and a real change of the
Wi-Fi credentials or certificates on the production Gateway (covered in verification). Result: pass.

**V-25 (disconnected cases, 2026-11-14 16:36 to 16:42, script `validation/outage.py`).** *UC-19 E3:* with the Field Device
CPU halted over SWD the Gateway answered a `fd_ack_alarm` as failed and the cloud showed `rejected` (suite scenario
`UC-19 E3`); the FD was resumed and is healthy. *Outage:* the broker on the Pi was stopped for 245 s (16:36:02 to 16:40:07).
UC-09/UC-12 E1: the alarm raised at 16:37:38 and cleared at 16:38:08 were both received after the restart (16:40:33), in
order (HIGH, then CLEAR): pass. UC-10/UC-11 E1: telemetry (period 60 s) and health (period 10 s) show nothing between the
stop and 16:38:45 (telemetry) or 16:38:52 (health): the two telemetry messages (16:36:45, 16:37:45) and about 17 health
messages of that time never reached the cloud; from 16:38:45 on the messages were buffered and arrived after the restart
(16:40:34), then live data resumed: partial, **F-9**. The first buffered telemetry (16:39:44, received 16:40:38) arrived after
a live one (16:40:34, received 16:40:35), so the arrival order is not strictly chronological (REQ-BF-010). UC-14 E1: the
read request sent during the outage was accepted by the API (202) and stayed `pending`; the cloud presents `timeout`
after 60 s and the row became `accepted` at 16:40:35 when the Gateway answered after the reconnect: **F-10**.
Not staged: E2 of UC-14 and E4 of UC-19 (a result produced while disconnected needs a command that arrives just before the
loss; the buffering of results is covered by unit tests CP-T*).

**V-26 (F-9 investigation, 2026-11-14 16:49 to 16:56 Gateway time).** Second broker outage of 240 s with the Gateway console captured
over the serial port. The Gateway logged `Connection lost` in the same second the broker stopped. The cloud received 6 of 6
telemetry messages and every health message of the outage after the Gateway reset (arrival in chronological order), see F-9 and
F-11. The Gateway's own health messages gave the buffer fill: 22 to 31 entries and 0 after the replay in the first outage with no
overflow, 1 to 40 and 0 in the second.

**V-27 (deployment of the fixes and repeat, 2026-11-14 about 17:00 to 17:15).** Field Device flashed with F-6, F-7, F-8 (v0.0.0+66f7b136),
cloud bundle installed (F-10), broker configuration `queue_qos0_messages true` installed (F-9); the Gateway reconnected by itself after the
broker restart (39 s). *F-8:* `prov set modbus-addr 300` and `257` are refused, `prov commit` says `Nothing staged`. *F-6:* with the
pressure fault injected and no alarm the pill shows a yellow, steady `FAULT` (checked by Luca), touch works, after the fault is cleared the
sensors read again. *F-9 and F-10, repeat of the V-25 outage (246 s, `validation/outage.py`):* the read request during the outage is
refused at once with `503 not connected to the broker: the command was not sent`; after the restart the cloud holds 4 of 4 expected
telemetry messages (largest gap 60 s), 25 of about 24 expected health messages (largest gap 10 s) and both alarm events (raised
17:10:13, cleared 17:10:42, received after the restart), all in chronological order; the Gateway reconnected by itself in about 35 s
(F-11 did not recur). The `409` answer for a device that has not reported for 150 s is covered by unit tests only (it
needs the Gateway silent for 2.5 minutes with the broker up). API scenarios UC-05 x3, UC-14, UC-15, UC-19 pass on the deployed cloud (6/6).

**V-28 (F-11 on the board, 2026-11-14 17:33 to 17:44, Gateway 1.1.3 = v0.0.0+68b81953, script `/tmp/f11_cycles.sh` on the Pi).** Six broker
outages of 20, 45, 90, 30, 60 and 45 s with the Gateway console captured. The Gateway reconnected every time, 15 to 30 s after the broker
was back (25, 30, 15, 16, 15, 20 s). The log shows the ladder (CP-D41) at the planned counts in every cycle: at the 3rd failed attempt
`joining the access point again`, at the 6th `resetting the Wi-Fi module`, then a join that works and `MQTT (re)connected`. Every failed
open reported AT step 5 (`P6`, the connect itself): with the broker down the module accepts the socket set-up and only the TCP connect
fails, so the new log line cannot tell a refused connection from a confused module socket either, as expected. Observations: (a) the
rejoin step answers `-> 3` (`WIFI_ERR_MODULE`) each time, because the module refuses a join while it is still associated, so only the module reset
does the work, about 11 s long; (b) an outage longer than about 20 s therefore costs a module reset and 5 to 10 s more before the Gateway is back;
(c) the publisher task blocks during a rejoin or reset, so one health message (10 s period) was late or missing in 2 of the 6
cycles. Cloud data over the 10 minutes: telemetry complete (largest gap 61 s), health 66 of about 65 expected, largest gap 18 s. *What this
does not show:* the wedge of V-26 (1 in 4 outages before the fix) did not occur in these 6, so the cure itself is not demonstrated; the
proof is that the ladder runs, that the module reset and join work on the board, and that nothing regressed. A further series of outages, or
the next time the wedge occurs, will show the cure.

**V-29 (UC-18 and UC-20 over the air, 2026-11-14 17:52 to 18:50, suite `validation/suites/30_ota.robot`, with the owner's go-ahead).** Gateway 1.1.3 updated to
1.1.4 over the cloud API (`PUT /api/images`, `POST /api/devices/{id}/ota`), the Gateway console captured on the Pi. Images: 1.1.4 and 1.1.5 built from main
(f96bac1) and signed with the release key (1.1.5 with the trial self-check forced to fail), a copy of 1.1.4 with one body byte flipped after signing, and
1.1.4 signed with a throw-away key. *UC-20:* the tampered image was downloaded completely (351 132 bytes) and refused with `hash_mismatch` (code 13, console ERROR line,
result `failed`); the wrong-key image with `sig_invalid`; the firmware stayed 1.1.3 with 0 rollbacks both times: pass. *UC-18 main flow with E2:* the broker was stopped for
25 s after 103 424 bytes; the download continued when the connection returned and finished; the Gateway verified, activated, rebooted, passed its self-check and
sent `success` (version 1.1.4, rollbacks 0) at 18:05:12, about 3 min after the job was created; the Gateway runs 1.1.4. *UC-18 E3:* 1.1.5 (self-check forced
to fail) was downloaded; the Gateway rebooted into it and the bootloader returned to 1.1.4; it sent `rolled_back` (reason `rollback`, version 1.1.4, rollbacks 1) at
18:25:32. *But:* both outcome messages carry no job id (`req` null), so the cloud kept jobs 4 and 5 as `running`: **F-12**. *E1:* with the Gateway CPU halted for 170 s (online
window 150 s) and the broker up the API accepted an update (202, job `sent`, nobody to run it): **F-13**. The first run of the suite had two mistakes of
its own (an arithmetic error that skipped the broker interruption, and a job left running that made the next case fail with 409); both were fixed and those cases
repeated. Not staged: UC-18 E4 (a flash write failing while deleting an image) and UC-20 E1 (a read error during the calculation). The suite's cases for the main flow,
E3 and E1 fail as long as F-12 and F-13 stand; they pass when the cloud attributes the result and refuses the offline device. State afterwards: Gateway 1.1.4 (built from main f96bac1,
the code of 1.1.3 plus the merge commits), Field Device unchanged, no job active, the validation images removed from the store.

**V-30 (F-12 and F-13 fixed, deployed and repeated, 2026-11-14 18:46 to 18:57).** Cloud service redeployed (database backup with the SQLite backup API,
`iotcloud.sqlite3.bak-pre-f12`); the API regression passed 6 of 6. Repeated on the Gateway (running 1.1.4): *UC-18 E3:* 1.1.5 with the self-check forced to fail was downloaded, the
bootloader rolled back to 1.1.4 and the cloud closed the job as `rolled_back` (reason `rollback`, version 1.1.4, rollbacks 1) at the Gateway's id-less result. *UC-18 main
flow with E2:* an update to 1.1.6 (same code, new version) with the broker stopped for 25 s after 101 376 bytes: the download continued, the job closed as `success`
(version 1.1.6, rollbacks 0) 3 min 28 s after it was created, and the Gateway runs 1.1.6. *UC-18 E1:* with the Gateway CPU halted for 170 s the API answered `409 device offline (last heard 171 s
ago): the command was not sent` and created no job. The whole suite `validation/suites/30_ota.robot` now passes (UC-20 in V-29, the rest in V-30). State afterwards: Gateway 1.1.6, no job
active, the validation images removed from the store.

**V-31 (the F-11 follow-ups on the board, 2026-11-14 19:57 to 20:08, Gateway 1.1.7 = v0.0.0+bac3d9ce, flashed from main after PR #224).** Six broker outages of 20, 45, 60,
90, 30 and 75 s with the Gateway console captured. The Gateway reconnected every time, 15 to 40 s after the broker was back (16, 30, 15, 40, 35, 15 s). *The rejoin now works:* the step logs
`leave the access point -> 0` and `join the access point -> 0` in every outage, where it used to answer `-> 3` (V-28). *The module reset comes earlier than I had written:* at the 8th failed
attempt, which is about 46 s after the connection was lost (not about 70 s, each attempt takes a few seconds on top of the backoff); every outage from 30 s up triggered one (it came before or just
after the broker returned), the 20 s outage none; a reset costs about 12 s but the reconnect after it was in the same 15 to 40 s range as waiting for the next backoff attempt, so the value 8 is kept and
the documentation corrected. Cloud data over the run: telemetry complete (12 rows, largest gap 60 s), health 71 of about 72 (gaps of 20 s and 30 s: a sample or two lost around a recovery step, the
publisher is blocked for a few seconds by it). The wedge of V-26 did not occur in these six either (12 outages without it since the fix, 1 in 4 before), so the cure is still not shown by a real case.

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
6. **F-6 (fixed, verified on the board, V-27) A sensor fault is shown as ALARM although no alarm exists** (UC-01 E1, UC-03). The header
   pill blinked ALARM for any Field Device fault (`compute_pill`, `fault_text`) while the Alarms screen and the console listed
   no alarm. Fix: a new pill state `FAULT` (amber, steady) for a fault with no active alarm (invalid sensor, Modbus link lost,
   flash write failed); an active alarm still shows the red blinking ALARM. The banner on the Status screen names the fault.
   The design pack (style guide, status pill table) and tests TC-LCDUI-007/021/009b are changed.
7. **F-7 (fixed, on the board since V-27, the trigger was never reproduced) Touch input stops permanently after an I2C1 bus error** (found at UC-03, 2026-11-14 about
   16:22). The display kept updating but no touch reached the firmware (no `touch raw` log in a 45 s capture while the screen
   was touched many times). Over SWD: I2C1 CR1 = 0x601 (STOP pending), SR1 = 0x100 (BERR), SR2 = 0x3 (MSL, BUSY), identical
   on six reads; the sensors (another bus) kept reading. The driver returned `I2C_ERR_BUS_BUSY` at once when BUSY was set and ran
   the recovery only after a timeout, and the wait loops did not check BERR/ARLO. A reset of the Field Device fixed it. The
   trigger was not established (the bus stayed clean afterwards). Fix: the recovery does a software reset of the peripheral, a BUSY bus
   at entry is recovered once, and BERR/ARLO end a transfer with the new `I2C_ERR_BUS_FAULT` after a recovery (LLD 3.5
   amended, tests TC-I2C-F4-008b..d).
8. **F-8 (fixed, verified on the board, V-27) `prov set modbus-addr 300` was accepted on the Field Device** (UC-16, range 1..247). `cmd_prov`
   cast the parsed number to `uint8_t` before the range check, so 300 became 44 (257 became 1). A commit would have set address 44
   and cut the Modbus link. `config set` was not affected. Fix: check the range on the parsed `long` (test TC-CS-087b).
9. **F-9 (fixed and verified, V-27) Telemetry and health sent while the cloud was catching up after a broker restart were lost**
   (UC-10/UC-11 E1, REQ-BF-000, REQ-BF-010). In the outage of V-25 the oldest 2 min 43 s of telemetry and health were missing
   although the alarms of the same time arrived. Investigation (V-26): the Gateway noticed the loss at once and buffered
   everything (its health message showed `buffer_entry_count` 22 to 31 and 0 after the replay, `buffer_overflow_count` 0); a
   second outage, replayed after a Gateway reset, arrived complete and in order (6 of 6 telemetry, every 10 s health sample).
   The loss therefore happens after the broker is back: the Gateway reconnects and replays at once, the cloud service (a persistent
   session) is not yet subscribed, and Mosquitto discards QoS 0 for an offline session, while QoS 1 is queued. Telemetry and
   health are QoS 0 by requirement (REQ-NF-206, REQ-NF-216), alarms QoS 1 (REQ-NF-207): exactly the pattern seen. Fix:
   `queue_qos0_messages true` in `broker/mosquitto/mosquitto.conf`. AWS IoT Core does not have this exposure (its rules engine
   does not depend on a connected subscriber). The earlier hypothesis (messages sent into a dead connection before the
   Gateway notices) was not confirmed: the Gateway noticed immediately. QoS 0 data sent in that window would still be lost,
   as the requirements allow.
10. **F-10 (fixed and verified for the broker-down case, V-27) A command sent while the Gateway is unreachable ran when the link returned** (UC-14 E1,
   UC-19 E1). Cause: with the broker down the cloud's own MQTT client queued the publish and sent it when the broker returned
   (the Gateway uses a clean session, so the broker itself would not have held it). The operator saw `timeout` after 60 s and the
   command then ran 4 minutes later. Fix, as decided by the owner: a command for a device that has not reported within the
   online window is refused with `409 device offline ... the command was not sent`, and a command is refused with `503` while the
   cloud's broker link is down; no command row is created in either case (tests in `test_commands_ota.py`).
11. **F-11 (fixed in firmware; Gateway 1.1.3 on the board, V-28: 6 of 6 outages recovered, the wedge itself did not recur) After a broker outage the Gateway can stay offline until it is reset** (found during the F-9
   investigation; analysed 2026-11-14). *Observed:* in the second broker outage the Gateway noticed the loss in the same second (`Connection lost`, then a failed
   31-byte send, `err=6`) and from then on every connect attempt, with the backoff 2, 4, 8, 16, 32, 60 s, logged
   `wifitask_open_socket failed: 6` (`WIFI_ERR_SOCKET`), also after the broker was back; NTP could not open a socket either. A reset of
   the Gateway cured it in 60 s. In the first and the third outage (and in the short broker restart of the deployment) it reconnected
   by itself in 9 to 35 s: 1 wedge in 4 outages. *Evidence about the module:* the Gateway's health messages kept reporting a changing RSSI
   (-59 to -63 dBm) during the 6 minutes, so the module stayed associated and answered commands, and `resolve 192.168.0.209 -> 0`
   succeeded on every attempt. Only opening a socket failed. *Why it never recovers by itself (code):* (1) `prepare_connect()` in
   `Src/main.c` escalates (rejoin the access point, ping, reset the module) only when the broker name does not resolve; with an IP
   address configured the module's DNS command returns the address without any network, so the escalation never starts and the
   Gateway retries every 60 s for ever; (2) the link state is cached and never changes after a drop (WIFI-O15), so nothing else notices;
   (3) `wifi_open_socket()` returns the same `WIFI_ERR_SOCKET` for any of its five AT steps (P0, P1, P3, P4, P6) and the log does not
   say which. *Likely trigger (hypothesis, not proved):* the known WIFI-O17 case, a module socket left confused when the peer vanishes
   (`P6=0` fails against a peer that already closed); the driver frees its own slot but always reuses the lowest free slot, so
   the next open, and NTP's UDP open, meet the same confused module socket 0, which fits both failing while the AT interface and the
   radio work. Why 1 outage in 4: unknown (timing of the close against the send in flight). *Proposed fix, in this order:*
   (a) a recovery ladder that does not depend on the cause: after 3 consecutive failed connect attempts rejoin the access point, after 6
   ping and reset the module (the calls already exist in `prepare_connect()`), bounding the outage to a few minutes; (b) in
   `wifi_open_socket()` try the next free slot after a failed open (round robin) and send `P6=0` for the slot that failed; (c) log which
   AT step failed. *Verification:* host tests for (a) and (b); on the board repeat the broker stop at least 5 times before and after
   (`validation/outage.py` with a shorter outage) and capture the Gateway console. Needs the owner's go-ahead for each flash.
   *Implemented:* all three. CloudPublisher counts consecutive failed connect attempts and asks `main.c` to rejoin the access point at the
   3rd and to reset the module at the 6th and then every 15th (CP-D41, tests CP-T145..T148); `wifi_open_socket()` retries on the next slot
   and cleans up the failed one (WIFI-T28/T29); MqttClient logs the failed AT step. Gateway host tests 1654 passed, 0 failed.
12. **F-12 (fixed and verified, V-30) The result a Gateway sends after its reboot has no job id, so the cloud never closes the job** (UC-18 step 9 and E3). The `success` after the update and the
   `rolled_back` after the rollback arrive with `req: null` (the Gateway does not know the job id after the reboot), `ota.on_result` needs an integer id, so the job stays
   `running` and the operator sees no result (events 239 and 241 of V-29). The update and the rollback themselves are right. Proposal, two options: (a) cloud: a terminal
   result without a job id belongs to the device's one active job (only one is allowed at a time); small, testable, no firmware change; (b) Gateway: store the job id with the
   update record and send it in the post-reboot result; the proper fix but it changes the firmware and the update record. (a) now, (b) when the update record is next touched.
13. **F-13 (fixed and verified, V-30) An update can be started for a Gateway that is not connected** (UC-18 E1: raise a connection error). `POST /api/devices/{id}/ota` has no online check (config and control
   commands refuse a device silent for more than the online window with `409 device offline`, F-10); the job is created in `sent` and nobody runs it until it expires after 10 minutes.
   Proposal: the same `require_online` for the update start, with a test.

## Open points

- Done 2026-11-14: Gateway 1.1.1 (signed release, built from 7b0dced, version string v0.0.0+7b0dceda) flashed with the owner's go-ahead; identity kept, cloud reconnected; UC-05 x3, UC-14 and UC-15 E3 x2 pass (6/6), including the unknown-key scenario (V-18).
- Done 2026-11-14 (V-19): F-1 and F-5 on the boards with the owner's go-ahead: Gateway 1.1.2 and the Field Device, both built from 09d5588
  (v0.0.0+09d5588f). API scenarios UC-05 x3, UC-14, UC-15 x3 and the new UC-17 E3 pass (8/8). Barometer fault injected for 21 s:
  213 failed reads gave 3 warning lines (reads 1, 100, 200) and one recovery line, instead of about 213 lines. Field Device settings unchanged.
- Done 2026-11-14: the manual session (LCD use cases V-20..V-23, UC-16 V-24, the disconnected cases V-25, V-26).
- F-6..F-10 are deployed and verified (V-27); F-7 (touch after a bus error) could not be provoked again, so its proof is the host tests and
  the touch that kept working; a Field Technician touch session in normal use is the remaining check.
- F-11 (the Gateway stays offline after a broker outage until a reset): fix on the Gateway (1.1.3), 6 of 6 outages recovered (V-28); the wedge did not recur, so the cure is not yet shown by a real case. Open question: should the useless rejoin step (it answers ERROR while associated) become a disconnect and join, and should the module reset wait for a longer outage than 20 s?
- Done 2026-11-14: UC-18 and UC-20 over the air (V-29), F-12 and F-13 fixed and verified (V-30). Follow-ups written and unit-tested, not yet on the Gateway: the Gateway sends the job id in the report after the reboot (F-12), and the F-11 ladder leaves and rejoins the access point and resets the module later. Open: the optional real AWS IoT Core check.
- Optional last: a real AWS IoT Core connection.
