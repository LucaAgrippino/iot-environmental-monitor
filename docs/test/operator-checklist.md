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
| OP-01 | REQ-LD-110 | Settings screen: enter a value outside the range (for example temperature high 9999) | An error is shown and APPLY stays blocked | |
| OP-02 | REQ-LD-120 | Enter a valid change, press APPLY | A confirmation dialog opens | |
| OP-03 | REQ-LD-130 | Press CONFIRM | The change applies; console `config list` shows the new value | |
| OP-04 | REQ-LD-155 | Change a value, press APPLY, then CANCEL; repeat and wait 30 s without pressing | In both cases the edit is discarded; `config list` shows the old value | |
| OP-05 | REQ-LD-150 | Change a value, CONFIRM, press the reset button | After the reboot `config list` still shows the new value (restore the old one afterwards) | |

## B. Forced faults

| ID | Requirement | Steps | Expected | Result |
|---|---|---|---|---|
| OP-06 | REQ-LD-040 | Make one sensor fail (hold the I2C line of the humidity sensor low, or unplug it if it is external) | The Sensors screen shows the error state for that sensor, the others stay normal | |
| OP-07 | REQ-LD-190 | With the sensor still faulty, open the chart screen | The failed samples are gaps in the chart, not zeros | |
| OP-08 | REQ-NF-214 | Start a configuration save (`config commit`, `y`) and cut the power during it; power back on | The device boots to a working state, with the previous or the new configuration, never a corrupt one (`status`: Config write fail: no) | |

Restore the sensor after OP-06/07.

## C. Inspections (read, do not run)

| ID | Requirement | What to do | Expected | Result |
|---|---|---|---|---|
| OP-09 | REQ-MB-070 | Compare `modbus-register-map.md` with the register tables in the Field Device and Gateway code (`modbus_register_map.c`, `modbus_poller.c` constants) | Addresses, types and scaling are equal | |
| OP-10 | REQ-DM-074 | Compare the dual-bank layout document with the linker scripts and `FirmwareStore` partition constants | Same bank sizes and addresses | |
| OP-11 | REQ-NF-303 | Search both firmwares for a network listener on the console (`grep -rn "listen\|accept" firmware/*/`) and confirm the console goes only through the UART driver | No network console exists | |
| OP-12 | REQ-NF-505 | Run `doxygen` on both firmwares and read the warnings | No warnings that hide a missing public-function description; list the rest as DEF-nnn severity 4 | |

## D. Still open elsewhere

Wave 6 (the 8 hour endurance run, TC-VER-EN-001) is not run yet; start it with `verification/run.sh 6` on the machine that stays on.
Requirements without a case yet are listed as "Not run" in `verification-results.md`; they need a cloud command path
(remote configuration, restart, OTA, rollback), the Raspberry Pi probe (timings of the bus), or more Robot cases.
