# Defects found during verification

Rules: `verification-plan.md` section 12. Severity: 1 safety or data loss, or a requirement fails with no workaround;
2 a requirement fails, with a workaround; 3 minor deviation; 4 documentation or improvement.

| ID | Found by | Requirement | Severity | Status |
|---|---|---|---|---|
| DEF-000 | TC-VER-FD-006 | REQ-AM-000, -010, -020, REQ-LD-280 | 2 | Closed: PR #180 (the Field Device pushed no alarm events to HealthMonitor: `alarms` always empty, red LED never lit) |
| DEF-001 | TC-VER-CL-004 | REQ-AM-040 | 3 | **Open, decision needed** |
| DEF-002 | wave 4 preparation, 2026-11-08 | none yet | 3 | Open, not reproduced |

## DEF-001: the alarm of a Field Device carries threshold 0

An alarm raised on the Field Device reaches the broker at QoS 1 with all fields, but `threshold_value_raw` is 0
(measured: limit set to 2039 centi-degrees, published 0). REQ-AM-040 lists the threshold among the payload fields.
Cause: `prv_fd_alarm_cb` in `cloud_publisher.c` only sees the `ALARM_FLAGS` register; its comment says "the threshold
lives on the Field Device (0)". Gateway-sourced alarms carry the real limit.
Options: (a) the poller reads the Field Device threshold registers when a flag changes and passes them on (code and
LLD change, one extra transaction per alarm); (b) the requirement is reworded to say the threshold is given for
Gateway alarms only (owner decision, waiver). Not fixed: it needs that decision.

## DEF-002: the Field Device display image hard-faulted at boot once

After the wave 2 and 3 runs the Field Device (image with the display) stopped after `ConfigService initialised`, with a
post-mortem of HardFault, CFSR 0x01000000 (unaligned access), PC 0x200000E0 (a RAM address). Re-flashing the same
image did not help at first; the image without the display ran, and a later flash of the display image booted normally.
Not reproduced since. Watch for it in the soak; if it returns, capture the stack before re-flashing.
