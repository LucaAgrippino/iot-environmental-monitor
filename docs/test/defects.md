# Defects found during verification

Rules: `verification-plan.md` section 12. Severity: 1 safety or data loss, or a requirement fails with no workaround;
2 a requirement fails, with a workaround; 3 minor deviation; 4 documentation or improvement.

| ID | Found by | Requirement | Severity | Status |
|---|---|---|---|---|
| DEF-000 | TC-VER-FD-006 | REQ-AM-000, -010, -020, REQ-LD-280 | 2 | Closed: PR #180 (the Field Device pushed no alarm events to HealthMonitor: `alarms` always empty, red LED never lit) |
| DEF-001 | TC-VER-CL-004 | REQ-AM-040 | 3 | Closed: PR #184 (the poller reads the channel's limits when a flag is raised; TC-VER-CL-004 passes) |
| DEF-002 | wave 4 preparation, 2026-11-08 | REQ-NF-213 (boot), REQ-LD-* (display) | 2 | Closed: the fix branch `fix/fd-enable-dwt-at-boot` (the Field Device with the display did not boot after a power-up) |
| DEF-003 | OP-09, 2026-11-08 | REQ-MB-070 | 4 | Closed: the register map document now says what the code does (sampling period range 100..10000, timeout counter reserved, device ID 0, slave address at 0x0150) |
| DEF-004 | OP-09, 2026-11-08 | REQ-MB-070 | 4 | Closed: stale "INTENTIONAL BUG" comments about `read_pressure` removed (code and test were already right) |
| DEF-005 | OP-12, 2026-11-08 | REQ-NF-505 | 4 | Closed: all Doxygen warnings of the public headers removed; `scripts/gen-doxygen.sh` reports 0 for both boards |
| DEF-006 | OP-10, 2026-11-08 | REQ-DM-074 | 4 | Closed: the Field Device linker script limits the image to the 1 MB the flash-partition document gives it (was 2048 KB; the image uses 46.8%) |
| DEF-007 | real-data review on the Pi, 2026-11-09 | REQ-SA-070, REQ-AM-040 | 4 | Closed: the simulated Field Device barometer is pulled back towards 1013.2 hPa (it drifted ~27 hPa per hour and flapped the 950 hPa low-pressure alarm about 3 times an hour) |
| DEF-008 | TC-VER-CL-012, 2026-11-10 | REQ-CC-010 | 3 | Open: the health message has no Modbus CRC count (ModbusMaster counts `crc_errors`, nothing publishes them) |
| DEF-009 | TC-VER-GW-015, 2026-11-10 | REQ-TS-000, REQ-TS-010 (display only) | 4 | Open: the console `status` line says "Time unsynchronised" after the Gateway has synchronised (the health message shows `last_ntp_sync_epoch` set 20 s after boot, 0 failures) |
| DEF-010 | OTA suite first run, 2026-11-11 | REQ-NF-213 (boot) | 2 | Closed: PR #206 (after a cold power-up the Field Device stopped in `abort()` before the scheduler: the 32.768 kHz crystal was not ready, `rtc_init()` failed, the Logger's first timestamp hit an assert; boots now, hardware-checked with the crystal off and after a full power cycle) |
| DEF-011 | OTA suite first run, 2026-11-11 | no single requirement (REQ-DM-072, REQ-DM-073 results were mixed up) | 3 | Closed: PR #207 (the Gateway republished messages it had already delivered after every reset: `CircularFlashLog` persisted only the head sector; `SAF 7 / 254 entries at boot` with the ring drained, 0 after the fix, hardware-checked) |
| DEF-012 | analysis of DEF-010, 2026-11-11 | REQ-NF-213 (boot), robustness | 4 | Open: no Field Device code calls `rtc_set_tick_source()`, `debug_uart_set_tick_source()` or `modbus_uart_set_tick_source()`, so their poll loops have no timeout (RTCD-O1 expected the call at start-up) |

## DEF-001: the alarm of a Field Device carried threshold 0

An alarm raised on the Field Device reached the broker at QoS 1 with all fields, but `threshold_value_raw` was 0
(limit set to 2039 centi-degrees, published 0), against REQ-AM-040. Cause: `prv_fd_alarm_cb` only saw the
`ALARM_FLAGS` register. Fix (PR #184): when a flag is raised the poller reads that channel's low and high limit
registers (FC03, 0x0100 + 0x10 x channel, two registers; at most three short reads, only on a transition),
`modbus_poller_get_fd_reading` returns them, CloudPublisher publishes the limit of the side that was crossed.
Verified on the boards: TC-VER-CL-004 and CL-001 pass, waves 4 and 5 8/8. Cost: 112 B of RAM.

## DEF-002: the Field Device with the display did not finish booting after a power-up

Seen twice: on 2026-11-08 after the wave 2 and 3 runs (HardFault post-mortem, unaligned access at a RAM address) and
after the boards were moved to the Raspberry Pi (no fault, log stops after `ConfigService initialised`, display off,
touch dead). Cause found by halting the core: it loops in `HAL_Delay` / `HAL_GetTick`
(`lcd_hal_tick.c`), which read the DWT cycle counter, and `Src/main.c` never started that counter: only the
integration harnesses call `system_clock_enable_dwt()`. A debugger session starts the counter and leaves it running
over a reset, so every flash-and-run worked; a cold power-up left it stopped and the LCD bring-up waited forever.
Fix: `system_clock_enable_dwt()` right after `system_clock_init()`. Checked on the board with the counter's enable bits
cleared by hand and a reset without debugger: the Field Device reaches Operational. The earlier HardFault was probably
the same missing counter (a stuck bring-up that then ran on garbage); the endurance run will show if it returns.

## DEF-003 to DEF-006: findings of the wave 7 inspections (part C)

Found by reading the code against the documents; no behaviour on the boards changed.
DEF-003: for each of the four differences the document was amended, not the code, because the code is what the
requirements ask for (REQ-NF-114 gives the 10 s upper bound of the sampling period) or what the hardware offers
(the slave keeps no timeout counter; the device-ID source is not wired in). Two of them stay open as improvements, not
defects: a real Field Device identifier (STM32 unique ID) and a timeout counter in `IModbusSlaveStats`.
DEF-005: first the seven public functions without a description were documented, then every other warning of the
public headers was removed (about 1 700 per board: members, macros, enumerators, `@param` and `@return`). `scripts/gen-doxygen.sh`
now ends with 0 warnings for both boards; the descriptions of members and macros were written from the names and the
surrounding code, so they deserve a read when a module is next touched. The `.c` files are outside the gate (their
file-local items are not public API); `--with-sources` documents them with call graphs.

## DEF-007: the simulated Field Device pressure drifted across the alarm limit

Field data from the Raspberry Pi showed the Field Device pressure at 943-978 hPa (swings of about 35 hPa per hour)
while the Gateway, with a real sensor, read a steady 1018 hPa, so the low-pressure alarm (950.0 hPa) was raised
about three times an hour. Cause: the Field Device has no barometer; `barometer_driver.c` is a bounded random walk
(+-0.2 hPa per read at 100 ms), whose spread grows with the square root of time (about 27 hPa per hour). Not a driver
bug but a weakness of the simulation model. Fix: one unit (0.1 hPa) of pull towards 1013.2 hPa when the value is more
than 3 hPa away; LLD §3.2 and test T-BARO-08 (200 000 reads stay within 10 hPa; fails without the fix). Host tests
only: the Field Device image has not been rebuilt or flashed.

## DEF-008: the health message has no Modbus CRC count

REQ-CC-010 lists "Modbus CRC/timeout/success counts". The health JSON has `modbus_timeouts` and `modbus_transactions_ok`, but no CRC field.
`modbus_master.c:243` increments `crc_errors` in `modbus_master_stats_t`; no module reads it (not the health serialiser, not the console).
Found by TC-VER-CL-012 (every other metric of the requirement is present). The wave 6 run saw 2 260 Field Device CRC errors in 294 736
frames, so the number is worth publishing. Fix: add the counter to the health statistics and the payload (a schema field, backwards compatible).

## DEF-009: the console says "Time unsynchronised" after the time is synchronised

On 2026-11-10 the Gateway's health message (uptime 306 s) carried `last_ntp_sync_epoch` 1794346481, `ntp_sync_fail_count` 0, the right time, 20 s after
boot; the console `status` line still read `Time unsynchronised` at 3 min and at 40 min of uptime. The time works (REQ-TS-000/010 pass from
the health data); only the status line is wrong, probably because `time_sync_state` of the health snapshot is not fed from TimeService.

## DEF-010: the Field Device stopped in abort() after a cold power-up

Found when the OTA suite left the Field Device silent after a reset. After a full power-up the 32.768 kHz crystal (LSE) needs from a
few hundred milliseconds to a couple of seconds to start; `system_clock_init()` only switched it on. `rtc_init()` then failed with
`RTC_ERR_LSE_NOT_READY`, `main()` ignored the result, and the Logger's first `rtc_get_time()` hit `assert(initialised)`: `abort()`, then
`_exit()`, before the scheduler, with a silent console. A warm reset keeps the LSE running, so a debugger session never showed it.
Fix: `system_clock_wait_lse_ready()` (3 s limit, timed with the DWT counter); the RTC driver returns `RTC_ERR_NOT_INIT` instead of asserting;
`main()` keeps the `rtc_init()` result and logs a warning (the Logger then stamps the uptime). Host tests TC_RTC_035 to 037.
Hardware: with `LSEON` cleared and reset, and after a full power cycle, the board reaches Operational and answers on the console. Not
shown: the old image failing on the same power cycle, and the case of a crystal that never starts (host tests only). Left open on purpose:
the Field Device never calls `rtc_set_tick_source()`, so the RTC polling loops still have no timeout.

## DEF-011: the Gateway republished delivered messages after every reset

The first OTA run showed old OTA results arriving again after each reboot, and earlier work had seen 284 entries for 254 stored
(CFL-O6 in the CircularFlashLog LLD). Cause: only the head sector is persisted, so records already consumed inside the head sector were
found again by the boot scan. Fix: an append-only consume journal in the unused part of each metadata sector (8-byte entries, no erase per
consume, one rotation per about 500), the boot scan skips journaled records. Host tests (including a torn journal entry and 1300
append and consume rounds under an erase budget). Hardware: the old image logged `SAF 7 / 254 entries at boot` on every reset with the ring
drained; the new one logs 0, keeps records buffered during a 100 s broker outage across a reset, and logs 0 again after delivery. A reset
between a consume and its journal write still replays that one record.

## DEF-012: the Field Device poll loops have no timeout

The RTC driver (and, with the same "no timeout" default, the debug UART and Modbus UART drivers) takes an injected millisecond tick
source. The default returns 0, so a deadline is never reached and a wait for a hardware flag can spin forever. The RTC header and
RTCD-O1 say production code installs a real tick source at start-up; a search of `firmware/field-device` finds no caller of
`rtc_set_tick_source()`, `debug_uart_set_tick_source()` or `modbus_uart_set_tick_source()`. Today the flags (`INITF`, `RSF`) do come up,
so nothing fails in the field; but a stuck peripheral would hang the boot or a task instead of returning `RTC_ERR_INIT_TIMEOUT` or
`RTC_ERR_SYNC_TIMEOUT`. The same default made a host test of this repository spin at 100% CPU for 23 minutes (TC_RTC_037, fixed in the test).
Proposal: install a SysTick or DWT based reader in `main()` before `rtc_init()` and exercise the timeout paths on the board (clear `LSEON`
and the flags cannot come up). Not part of the DEF-010 fix.
