# Defects found during verification

Rules: `verification-plan.md` section 12. Severity: 1 safety or data loss, or a requirement fails with no workaround;
2 a requirement fails, with a workaround; 3 minor deviation; 4 documentation or improvement.

| ID | Found by | Requirement | Severity | Status |
|---|---|---|---|---|
| DEF-000 | TC-VER-FD-006 | REQ-AM-000, -010, -020, REQ-LD-280 | 2 | Closed: PR #180 (the Field Device pushed no alarm events to HealthMonitor: `alarms` always empty, red LED never lit) |
| DEF-001 | TC-VER-CL-004 | REQ-AM-040 | 3 | Closed: PR #184 (the poller reads the channel's limits when a flag is raised; TC-VER-CL-004 passes) |
| DEF-002 | wave 4 preparation, 2026-11-08 | REQ-NF-213 (boot), REQ-LD-* (display) | 2 | Closed: the fix branch `fix/fd-enable-dwt-at-boot` (the Field Device with the display did not boot after a power-up) |

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
