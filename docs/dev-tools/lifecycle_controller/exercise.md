# Exercise — LifecycleController

**Date:** 2026-06-24
**Target:** docs/lld/application/lifecycle-controller.md

---

## Exercise 1 — Fault-injection during init sequence

**Scenario:**
You are a new team member reviewing the LifecycleController FD boot sequence.
TC-LC-034 passes: when `sensors->init()` returns an error, the state is FAULTED and `alarms->init()` is never called.

**Task (FD test file):**
1. Write TC-LC-034b: verify that when `sensors->init()` fails, `graphics->init()` is also NOT called (i.e., the entire sub-state chain after BringingUpSensors is skipped).
2. Write TC-LC-034c: verify that `health_report->push_event(HEALTH_EVENT_FAULT, ...)` is called exactly once when sensors init fails.
3. Run `scripts/test-module.ps1 -Module lifecycle_controller_fd` and confirm both new TCs pass.

**Expected learning:** Early-exit guards in `fd_run_init_sequence()` prevent downstream sub-states from running on failure. `push_event` is called inside `enter_faulted()` which is called exactly once per fault.

---

## Exercise 2 — EditingConfig timeout timer period

**Scenario:**
REQ-NF-214 mandates a 5-minute (300 s) config-edit timeout. The current implementation uses `LC_EDIT_TIMEOUT_MS = 5 * 60 * 1000` passed to `xTimerCreateStatic`.

**Task:**
1. Add a mock-observable `g_mock_xTimerCreateStatic_last_period` variable to `FreeRTOS.h` and `freertos_mock.c` (similar to `g_mock_xTimerStart_call_count`).
2. Write TC-LC-074b: after `lifecycle_controller_init()`, verify that one of the created timers has a period of `pdMS_TO_TICKS(300000)` (5 minutes in ticks).
3. Hint: the edit timer is the first non-init timer created. You may need to track the period of each `xTimerCreateStatic` call separately.

**Expected learning:** Ceedling mock state variables are the primary mechanism for asserting RTOS primitive configuration in host-side tests. This mirrors the approach used in `ConsoleService` for task notification timing.

---

## Exercise 3 — Dual-board `handle_remote_command` divergence

**Scenario:**
TC-LC-130 (GW): `RESET_METRICS` calls `health_admin->reset_metrics()` once and returns OK.
TC-LC-137 (FD): `RESET_METRICS` returns OK without calling any health_admin (there is no health_admin on FD).

**Task:**
1. In `test_lifecycle_controller_fd.c`, extend TC-LC-137 to also verify that `g_health_push_event_calls == 0` after `RESET_METRICS` — confirming no health side-effect on FD.
2. In `test_lifecycle_controller_gw.c`, write TC-LC-131b: verify that `RESET_METRICS` called while in `EDITING_CONFIG` state also calls `health_admin->reset_metrics()` (direct dispatch, not queued — should work from any state).
3. Write TC-LC-132b (GW): verify that `RESET_METRICS` does NOT post any event to the queue (i.e., `g_mock_xQueueSend_call_count` is unchanged) when called while in `RESTARTING` state.

**Expected learning:** `RESET_METRICS` is direct-dispatched (synchronous), not queued. It must work regardless of the lifecycle state. This is an important design property (LLD §14): the health-metrics reset cannot be blocked by state-machine occupancy.

---

## Exercise 4 — GW SelfChecking fault injection

**Scenario:**
TC-LC-056 passes: when `cloud->is_ready()` returns false, the state is FAULTED after the init sequence.

**Task (GW test file):**
1. Write TC-LC-056b: verify that when `cloud->is_ready()` returns false, `health_report->push_event(HEALTH_EVENT_FAULT, ...)` is called exactly once.
2. Write TC-LC-056c: verify that when both `sensors->is_ready()` AND `cloud->is_ready()` return false, the final state is still FAULTED (not double-faulted, and `health_report->push_event` called exactly once).
3. Hint: for TC-LC-056c, instrument `enter_faulted()` by counting `g_health_push_event_calls`.

**Expected learning:** `enter_faulted()` is idempotent in the sense that once `s_state == FAULTED`, `gw_run_init_sequence()` returns early and no second call to `push_event` can occur. The early-return after the first probe failure guarantees this.

---

## Exercise 5 — Integration test teardown

**Scenario:**
For IT-LC-007 (EditingConfig entry with no input for 5 minutes → auto-cancel and Operational), the integration test observer (`monitor_task` in `test_lifecycle_controller_main.c`) must detect the EDITING_CONFIG → OPERATIONAL transition after a 5-minute wait.

**Task:**
1. Extend `it_lc_start()` to accept a `bool verbose` parameter. When `verbose == true`, the monitor task logs each raw state value (integer) in addition to the human-readable name.
2. Add a `uint32_t it_lc_get_transition_count(void)` function that returns how many state transitions were observed since `it_lc_start()` was called.
3. Modify the monitor task to call a weak-attribute callback `void it_lc_on_transition(lifecycle_state_t from, lifecycle_state_t to)` on each transition, so specific integration test setups can register their own assertion handlers.

**Expected learning:** Integration test harnesses on embedded systems need observable hooks (`__attribute__((weak))` callbacks, counters, verbose modes) to avoid relying on a human reading UART output for every pass/fail decision. This bridges toward automated on-target testing with a Modbus master or JTAG probe that can read registers.
