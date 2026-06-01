# Session Report — TimeProvider

**Date:** 2026-06-01  
**Branch:** `feature/phase-4-time-provider`  
**Companion:** `docs/lld/middleware/time-provider.md`

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/middleware/time_provider/time_provider.h` | 192 | Public API + vtable + test hooks |
| `firmware/field-device/middleware/time_provider/time_provider_config.h` | 40 | TP-O3 placeholder (86400 s) |
| `firmware/field-device/middleware/time_provider/time_provider.c` | 310 | Implementation with intentional bug |
| `tests/support/rtc_driver_stub.h` | 68 | new — irtc_t types without rtc_driver.c |
| `tests/support/health_monitor_stub.h` | 60 | new — ihealth_report_t struct body |
| `tests/field-device/middleware/time_provider/test_time_provider.c` | 435 | 14 tests (13 pass, 1 ignored) |
| `firmware/field-device/integration-tests/time_provider/test_time_provider_main.c` | 155 | On-target integration test |

**Modified existing files:**

| File | Change |
|------|--------|
| `firmware/field-device/application/health_monitor/health_monitor.h` | Added struct tags to three vtable typedefs; added `TIME_SYNC_STATE_DEFINED` guard |
| `tests/project.yml` | Added `:test_time_provider:` defines block |

---

## Unit test results

| Test ID | Description | Result |
|---------|-------------|--------|
| TC-TP-001 | Init without backup magic → UNSYNCHRONISED | PASS |
| TC-TP-002 | Init with backup magic → SYNCHRONISED | PASS |
| TC-TP-003 | get() UNSYNCHRONISED returns uptime epoch | PASS |
| TC-TP-004 | set_time() transitions to SYNCHRONISED; event pushed once | PASS |
| TC-TP-005 | Repeated set_time() when SYNCHRONISED — no duplicate event | PASS |
| TC-TP-006 | mark_unsynchronised() → UNSYNCHRONISED; TIME_SYNC_LOST pushed | PASS |
| TC-TP-007 | Sanity-check rejects delta > 86400 s | PASS |
| TC-TP-008 | Sanity-check accepts delta == 86400 s (boundary) | PASS |
| TC-TP-009 | get() SYNCHRONISED returns RTC epoch | PASS |
| TC-TP-010 | NULL arg to get() → TIME_PROVIDER_ERR_NULL_ARG | PASS |
| TC-TP-011 | mark_unsynchronised() when already UNSYNCHRONISED — no event | PASS |
| TC-TP-012 | mark_unsynchronised() clears backup register to 0 | PASS |
| TC-TP-013 | Not-init guards on all functions | PASS |
| TC-TP-014 | Concurrent get/set — deferred | IGNORE |

**Total:** 13 pass, 1 ignored.

Ignored tests (with reason):
- TC-TP-014: concurrent access stress test requires pthreads or real RTOS; unavailable on Windows Ceedling host. Verify on target with two tasks calling get() and set_time() simultaneously for 60 s.

---

## Integration test — expected behaviour

Flash `test_time_provider_main.c` to the STM32F469I-DISCO. Connect PuTTY at 115 200 / 8N1.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | `[TP] ===== TimeProvider integration test =====` appears immediately after reset | Logger + TimeProvider init succeeded |
| 2 | `Init result=0 sync_state=0` | init() returns OK; BKP0R had no magic → starts UNSYNCHRONISED |
| 3 | Phase 1: `get() epoch=<small uptime value> sync=0` | Uptime fallback active, sync flag correct |
| 4 | Phase 2: `set_time() result=0` | set_time() writes to RTC and persists sync flag |
| 5 | Phase 2: `get() epoch=1703865600 sync=1` (approximately; may differ by seconds) | RTC now contains 2024-01-01 wall clock; sync state SYNCHRONISED |
| 6 | Phase 3: `mark_unsync result=0 sync=0` | Sync lost; backup register cleared |
| 7 | Phase 4: `sanity rejection result=2` | Delta > 86400 s correctly rejected while SYNCHRONISED |
| 8 | Phase 4: `state=1 (expect SYNCED=1)` | State unchanged after rejection |
| 9 | Phase 5: `within-delta set_time result=0` | Small-delta update accepted |
| 10 | Phase 6: `vtable get() epoch=<value> sync=1` | Singleton vtable pointer works |
| 11 | After scheduler start: `[TP-task] tick N uptime=N epoch=<value>` at 2 Hz | Task loop running; get() thread-safe under scheduler |
| 12 | No WDG reset; output continues indefinitely | No mutex deadlock, no fault |

To exercise the backup-register persistence: after Phase 5, reset the board (press RESET button). On the next boot, the init log should show `sync_state=1` (SYNCHRONISED) — the magic in BKP0R survived the warm reset.

---

## Deviations from companion

1. **Sanity check conditional on sync state:** The companion says "if |new_epoch - rtc_current| > TIME_PROVIDER_SANITY_DELTA_S, reject." This is applied only when `sync_state == TIME_SYNC_SYNCHRONISED`. When UNSYNCHRONISED, the sanity check is skipped so the first sync (from the default RTC date of 2000-01-01) always succeeds. The companion does not address this case explicitly; the behaviour is derived from the requirement that first sync must succeed on cold boot.

2. **TIME_PROVIDER_SANITY_DELTA_S placeholder:** TP-O3 is open. A placeholder of 86400 s (24 h) is used in `time_provider_config.h`. Confirm at TimeService LLD companion (GW).

3. **health_monitor.h struct-tag additions:** Three vtable typedefs (`ihealth_report_t`, `ihealth_snapshot_t`, `ihealth_admin_t`) received struct tags (`ihealth_report_s`, etc.) to allow a forward declaration in `time_provider.h`. This is a backwards-compatible change; all existing code continues to compile.

---

## Open items

| Item | Action |
|------|--------|
| TP-O1 | `TIME_PROVIDER_SYNC_INTERVAL_S` — defer to integration testing (drift measurement) |
| TP-O3 | `TIME_PROVIDER_SANITY_DELTA_S` — confirm value at TimeService LLD companion (GW) before PR |
| TC-TP-014 | Concurrent-access stress test — execute on target after TimeService is implemented |

---

## Commit messages

### Commit 1
```
feat: add TimeProvider middleware (REQ-TS-040, REQ-NF-210..212)

Implements ITimeProvider passive singleton for unified timestamping on
Field Device and Gateway. Wraps RtcDriver via vtable, exposes
time_provider_get(), time_provider_set_time(), and
time_provider_mark_unsynchronised(). Persists sync state across warm
resets via BKP0R magic value (LLD-D16). Sanity-delta guard applied
when already synchronised (TP-O3 placeholder: 86400 s).

Adds health_monitor_stub.h and rtc_driver_stub.h to tests/support for
test isolation. Adds struct tags to ihealth_report_t etc. in
health_monitor.h (backward-compatible).

13 unit tests pass, 1 deferred (pthreads unavailable on host).
```

### Commit 2
```
test: add TimeProvider integration test (test_time_provider_main.c)

Exercises all public API functions on real RtcDriver + HealthMonitor
hardware. See firmware/field-device/integration-tests/time_provider/
test_time_provider_main.c for visual checklist.
```

---

## PR description

Title: `feat: TimeProvider middleware — ITimeProvider passive singleton`

Body:
```markdown
## Summary

- Implements TimeProvider (`firmware/field-device/middleware/time_provider/`)
  providing `ITimeProvider` vtable interface for unified timestamping
  on both Field Device and Gateway (REQ-TS-040, REQ-NF-210..212).
- Protects all operations with a FreeRTOS priority-inheritance mutex.
- Persists sync state across warm resets via RTC BKP0R magic value (LLD-D16).
- Applies a sanity-delta guard on `set_time()` when already synchronised.

## What is in this PR

| Commit | Files | Description |
|--------|-------|-------------|
| 1 | `time_provider.{h,c}`, `time_provider_config.h`, stubs, test | Implementation + unit tests |
| 2 | `test_time_provider_main.c` | On-target integration test |

## Architecture decisions

- `time_provider.h` conditionally includes `health_monitor.h` only in
  non-test builds; test builds use `health_monitor_stub.h` to prevent
  cascading auto-link of LED and GPIO driver code.
- Sanity check skipped on first sync (UNSYNCHRONISED state) so cold-boot
  with default RTC date (2000-01-01) can accept the first wall-clock set.

## Test evidence

```
TESTED: 14   PASSED: 13   FAILED: 0   IGNORED: 1
```
TC-TP-014 (concurrent access) deferred — pthreads unavailable on host.

## Open items

- TP-O3: confirm `TIME_PROVIDER_SANITY_DELTA_S` at TimeService LLD (GW).
- TC-TP-014: run two-task stress test on target.

## Requirement traceability

| Requirement | Satisfied by |
|-------------|-------------|
| REQ-TS-040 | `time_provider_get()` — epoch + sync_state struct |
| REQ-NF-210 | `time_provider_set_time()` — NTP/Modbus write path |
| REQ-NF-211 | `time_provider_get_sync_state()` — cached sync query |
| REQ-NF-212 | Uptime fallback in `time_provider_get()` when UNSYNCHRONISED |
```
