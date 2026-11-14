# Wave 7: operator checklist

**Plan:** `verification-plan.md` section 9, wave 7. **Method table:** `verification-method-table.md`.
These are the requirements that need hands, a forced fault or a reading of the code, so no Robot suite runs them.
Record the result of each line (PASS, FAIL or blocked, date, initials) in the last column. A FAIL becomes a `DEF-nnn`
in `defects.md`. After the run, the results merger does not read this file: copy the verdicts into the report by hand.

**Before you start:** Field Device flashed with the display image (`GUI=1 ST_SERIAL=... bash scripts/build-fd-app.sh --flash`), Gateway on the product
image, both consoles open (Field Device 115 200, Gateway 460 800). If the display does not react to touch, stop and
report it: all of part A depends on it.

## A. Field Device display (touch)

| ID | Requirement | Steps | Expected | Result |
|---|---|---|---|---|
| OP-01 | REQ-LD-110 | Settings screen: enter a value outside the range (for example temperature high 9999) | An error is shown and APPLY stays blocked |  PASS 2026-11-14 (Luca): Settings screen: temperature high 9999 shows an error and APPLY stays blocked (one decimal digit at most is accepted, by design) |
| OP-02 | REQ-LD-120 | Enter a valid change, press APPLY | A confirmation dialog opens |  PASS 2026-11-14 (Luca): a valid change and APPLY opens the confirmation dialog |
| OP-03 | REQ-LD-130 | Press CONFIRM | The change applies; console `config list` shows the new value |  PASS 2026-11-14 (Luca): CONFIRM applied the change: console config list showed temp-alarm-high 4350 (43.5 degC) |
| OP-04 | REQ-LD-155 | Change a value, press APPLY, then CANCEL; repeat and wait 30 s without pressing | In both cases the edit is discarded; `config list` shows the old value |  PASS 2026-11-14 (Luca): the bar CANCEL and the 30 s timeout both discarded the edit (config list unchanged at 4350); the popup CANCEL kept the edits, changed to discard in this PR (retested OK on the board) |
| OP-05 | REQ-LD-150 | Change a value, CONFIRM, press the reset button | After the reboot `config list` still shows the new value (restore the old one afterwards) |  PASS 2026-11-14 (Luca): after CONFIRM and the reset button config list showed temp-alarm-high 4400, the new value survived the reboot |
| OP-11 | REQ-LD-060 | Power-cycle the Field Device (reset button) and watch the Sensors screen from the first frame | A "waiting for data" message shows until the first reading arrives, then the values replace it |  PASS 2026-11-14 (Luca) on the second image: first run FAIL (no waiting message, the splash hid it: DEF-015); after the fix (sensors start after the splash, 1 s pause) the message shows and is readable |

## B. Forced faults

| ID | Requirement | Steps | Expected | Result |
|---|---|---|---|---|
| OP-06 | REQ-LD-040 | Make one sensor fail (hold the I2C line of the humidity sensor low, or unplug it if it is external) | The Sensors screen shows the error state for that sensor, the others stay normal |  PASS 2026-11-14 (Luca): pressure fault injected over SWD (barometer driver fault flag): the card shows the error state, temperature and humidity stay normal; the stale number found on the first run was fixed (the card now shows a dash) and retested OK |
| OP-07 | REQ-LD-190 | With the sensor still faulty, open the chart screen | The failed samples are gaps in the chart, not zeros |  PASS 2026-11-14 (Luca): with the pressure fault the chart shows gaps, not zeros; the history buffer on the board held 28 valid then 26 invalid pressure samples, temperature and humidity all valid |
| OP-08 | REQ-NF-214 | Start a configuration save (`config commit`, `y`) and cut the power during it; power back on | The device boots to a working state, with the previous or the new configuration, never a corrupt one (`status`: Config write fail: no) |  PASS 2026-11-14 (Luca): commit loop (about 12 saves of 43.0/43.5/44.0 degC) and the power cut by hand: the device booted, status Config write fail: no, config list complete; two trials: the first equal to the last save sent (4400), the second equal to the previous complete save (4350), the save in flight was not applied |

Restore the sensor after OP-06/07.

## C. Inspections (read, do not run)

| ID | Requirement | What to do | Expected | Result |
|---|---|---|---|---|
| OP-09 | REQ-MB-070 | Compare `modbus-register-map.md` with the register tables in the Field Device and Gateway code (`modbus_register_map.c`, `modbus_poller.c` constants) | Addresses, types and scaling are equal | PASS 2026-11-08 (Claude): addresses, types, scaling and alarm bits equal; deviations closed as DEF-003/004 |
| OP-10 | REQ-DM-074 | Compare the dual-bank layout document with the linker scripts and `FirmwareStore` partition constants | Same bank sizes and addresses | PASS 2026-11-08 (Claude): Gateway equal; Field Device linker limit fixed, DEF-006 |
| OP-11 | REQ-NF-303 | Search both firmwares for a network listener on the console (`grep -rn "listen\|accept" firmware/*/`) and confirm the console goes only through the UART driver | No network console exists | PASS 2026-11-08 (Claude): no listener; sockets are client only (NTP, MQTT); console is UART only |
| OP-12 | REQ-NF-505 | Run `doxygen` on both firmwares and read the warnings | No warnings that hide a missing public-function description; list the rest as DEF-nnn severity 4 | PASS 2026-11-08 (Claude) after DEF-005: Doxygen reports 0 warnings for the public headers of both boards |

### C findings (2026-11-08)

- **OP-09, deviations from `modbus-register-map.md`** (addresses, types and scaling are equal; these are content differences):
  1. `MODBUS_TIMEOUT_COUNT` (0x0038/0x0039) always reads 0 (`read_zero`), the map says it counts frame timeouts.
  2. `SAMPLING_PERIOD_MS` (0x0130) is accepted up to 10000 ms (`SAMP_PERIOD_MAX`, REQ-NF-114), the map says 100..60000.
  3. `DEVICE_ID_HI/LO` return 0, the map says hardware-fused.
  4. `read_pressure` still carries the comment "INTENTIONAL BUG (MRM-BUG-001)" although the code reads `SENSOR_ID_PRESSURE` correctly: stale comment.
  Known and already documented: `MODBUS_SLAVE_ADDR` at 0x0150 (in the reserved range).
- **OP-10:** Gateway matches the flash-partition document. The Field Device linker script declares 2048 KB of flash where the document gives 1 MB for the image (the second megabyte is reserved there); not a conflict, but the script does not stop an image above 1 MB.
- **OP-12:** the repository has no project Doxyfile. A default run over application, drivers and middleware gave 2436 warnings (Gateway) and 1731 (Field Device), mostly undocumented macros, enum members and missing `@return`/`@param`. Seven public functions have no description: `device_profile_get_next`, `device_profile_is_registered`, `health_monitor_update_mcu_temp`, `us_unlock`, `tls_arena_uninit_calloc`, `tls_arena_uninit_free`, `i2c_bus_unlock`. Filed and closed as DEF-003 to DEF-006; `scripts/gen-doxygen.sh` regenerates the documentation (`docs/doxygen/`).

## D. Still open elsewhere

Wave 6 (the 8 hour endurance run, TC-VER-EN-001) ran on 2026-11-08 from 14:44 to 22:44 on the Raspberry Pi and passed:
no reset of either board (uptimes 29 347 s and 29 324 s at the last sample), 605 MQTT publishes with 0 failures, buffer
empty, 0 sensor failures. That run carried no `REQ-` tag. From the next run the test is tagged REQ-CC-000 and REQ-NF-111
(periodic telemetry, at least 90 % of one message per 60 s over the whole run), which it measures; both are already
Verified by TC-VER-CL-003, so this adds the long-run evidence and changes no status. The other checks (no reset, 1 %
bus limit, no MQTT failure) have no requirement behind them and stay untagged on purpose: the 10 s watchdog
(REQ-NF-109) and the health metrics (REQ-CC-010) are not measured by this test.

- **Bus errors to look at with the boards in hand.** The pass limit is 1 % of transactions. The run ended with 2 277
  Modbus timeouts against 292 789 good transactions (0.78 %) and 2 260 Field Device CRC errors in 294 736 frames
  (0.77 %), about 280 of each per hour and steady. The two counts almost match, which points at one cause on the RS-485 link (noise, a marginal
  contact, missing or doubled termination) and not at the firmware. Check the wiring and the termination; a quiet
  bus should show none. Not filed as a defect: the requirement limit is met.
Requirements without a case yet are listed as "Not run" in `verification-results.md`; they need a cloud command path
(remote configuration, restart, OTA, rollback), the Raspberry Pi probe (timings of the bus), or more Robot cases.
