# Validation summary

**Phase:** validation (V-model, right side), 2026-11-14. **Record:** `validation-plan.md` (method, one record per scenario V-04 to V-33,
findings F-1 to F-14). **Result:** complete; the optional check against a real AWS IoT Core is deferred.

## What was done

Each of the 20 use cases (`docs/use-case-descriptions.md`) was run as its actor would run it, on the real boards:

- **Remote Operator** (UC-05, 06, 09 to 12, 14, 15, 17 to 19): through the cloud API and a headless browser, with Robot Framework and Playwright
  (`validation/suites/10_*`, `20_*`, `30_*`).
- **Field Technician** (UC-01 to 04, 15, 16): at the touch screen and the consoles, with faults injected over SWD, with the owner at the boards.
- **System** (UC-07, 08, 13, 20): from the consoles, the cloud and the OTA path.
- **Cloud side:** the Raspberry Pi service (Mosquitto, API, web app) stood in for AWS IoT Core. The firmware was never changed to make a scenario pass.

## Result by use case

All 20 use cases pass, with every exception run on the system except two that cannot be provoked safely and are covered by unit tests
(UC-18 E4: TC-FS-036b, TC-US-034; UC-20 E1: TC-FS-048, TC-FS-050).

## What validation found (14 findings, all fixed)

| Finding | What a user would have met | Fix | Shown by |
|---|---|---|---|
| F-1 | A failed sensor flooded the console (14 lines a second) | Log the first failure, then every 100th, and the recovery | V-19 |
| F-2 | Acceleration, rotation and magnetic values missing in the cloud | Cloud API and web app show them | V-05 |
| F-3 | The cloud on the Pi was older than `main` | Deployed | UC-19 scenario |
| F-4 | A rejected unknown setting was not reported | The request id is echoed | V-18 |
| F-5 | A second `restart` rebooted without the confirmation | Only the confirmation restarts | V-19 |
| F-6 | A sensor fault showed ALARM with no alarm | New amber FAULT pill | V-27 |
| F-7 | Touch stayed dead after an I2C bus error | Bus recovery with a peripheral reset | V-27 |
| F-8 | `prov set modbus-addr 300` was accepted (wrapped to 44) | Range checked first | V-27 |
| F-9 | Telemetry and health lost after a broker restart | Broker queues QoS 0 for the cloud session | V-27 |
| F-10 | A command to an unreachable Gateway ran minutes later | Refused at once (409 or 503) | V-27 |
| F-11 | The Gateway could stay offline after a broker outage | Recovery ladder: rejoin, then module reset | V-28, V-31, V-33 |
| F-12 | The cloud never closed an OTA job (no job id after the reboot) | The Gateway sends the id; the cloud has a fallback | V-30, V-32 |
| F-13 | An update could be started for an offline Gateway | Refused with 409 | V-30 |
| F-14 | An interrupted download could be abandoned (1 in 4 runs) | Three failures end it only after 2 min without progress | V-33 |

F-9 to F-14 were found only by cutting a link or running a real update on the system, which the unit-level verification could not do.

## State of the system at the end

Gateway firmware **1.1.20** (the code of 1.1.10, signed again), Field Device **v0.0.0+66f7b136**, cloud service and broker deployed from `main`.
The repository holds the fixes, the tests (Gateway host tests 1661, Field Device 898, cloud 55) and the validation suites.

## What is not closed

- **Real AWS IoT Core:** deferred by the owner. All cloud results are against the local stand-in.
- **F-11:** the original wedge did not recur in about 26 outages since the fix, but its cause was never seen. Accepted as a residual risk; the recovery does not
  depend on the cause, and the log now shows what happened. Reopen if a connect keeps failing for more than three minutes.
- **Touch lock-up (F-7):** the trigger was never reproduced; the fix is proved by unit tests and by touch working through the rest of the session.

## How to run it again

```text
VAL_PASS=<password of the validation account> bash validation/run.sh api        # Remote Operator, API (about 8 minutes, restarts the Gateway)
VAL_PASS=... bash validation/run.sh web                                          # Remote Operator, browser (about 1 minute)
VAL_PASS=... python validation/outage.py 240                                     # a broker outage, with the checks
VAL_PASS=... python validation/ota_series.py ~/rel/<release> <first patch>      # interrupted OTA downloads
```

The OTA suite (`validation/suites/30_ota.robot`) updates the Gateway: ask the owner first. Needs the Python environment described in `validation/run.sh`.
