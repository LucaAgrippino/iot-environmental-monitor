# Defects found during verification

Rules: `verification-plan.md` section 12. Severity: 1 safety or data loss, or a requirement fails with no workaround;
2 a requirement fails, with a workaround; 3 minor deviation; 4 documentation or improvement.

| ID | Found by | Requirement | Severity | Status |
|---|---|---|---|---|
| DEF-000 | TC-VER-FD-006 | REQ-AM-000, -010, -020, REQ-LD-280 | 2 | Closed: PR #180 (the Field Device pushed no alarm events to HealthMonitor: `alarms` always empty, red LED never lit) |
| DEF-001 | TC-VER-CL-004 | REQ-AM-040 | 3 | Closed: PR #184 (the poller reads the channel's limits when a flag is raised; TC-VER-CL-004 passes) |
| DEF-002 | wave 4 preparation, 2026-11-08 | none yet | 3 | Open, not reproduced |

## DEF-001: the alarm of a Field Device carried threshold 0

An alarm raised on the Field Device reached the broker at QoS 1 with all fields, but `threshold_value_raw` was 0
(limit set to 2039 centi-degrees, published 0), against REQ-AM-040. Cause: `prv_fd_alarm_cb` only saw the
`ALARM_FLAGS` register. Fix (PR #184): when a flag is raised the poller reads that channel's low and high limit
registers (FC03, 0x0100 + 0x10 x channel, two registers; at most three short reads, only on a transition),
`modbus_poller_get_fd_reading` returns them, CloudPublisher publishes the limit of the side that was crossed.
Verified on the boards: TC-VER-CL-004 and CL-001 pass, waves 4 and 5 8/8. Cost: 112 B of RAM.

## DEF-002: the Field Device display image hard-faulted at boot once

After the wave 2 and 3 runs the Field Device (image with the display) stopped after `ConfigService initialised`, with a
post-mortem of HardFault, CFSR 0x01000000 (unaligned access), PC 0x200000E0 (a RAM address). Re-flashing the same
image did not help at first; the image without the display ran, and a later flash of the display image booted normally.
Not reproduced since. Watch for it in the soak; if it returns, capture the stack before re-flashing.
