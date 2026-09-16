# Session Report — WifiTask (Gateway)

**Date:** built pre-2026-09-13; hardware-validated 2026-09-13. Backfilled 2026-09-16.
**Branch:** `feature/phase-4-gw-cloud_publisher` → merged as PR #66 (`e17a0f9`)
**Companion:** `docs/lld/middleware/wifi-task.md` v0.1, Status: Implementation-ready (Phase H complete)

> **Backfill note.** These Step 12 deliverables were not produced when the
> module was built. They are reconstructed on 2026-09-16 from the companion,
> the merged source, the test suites (re-run for this report) and the commit
> history. Timings and test counts are measured, not recalled; where a detail
> could not be recovered from evidence it is marked as such rather than guessed.

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/gateway/middleware/wifi_task/wifi_task.h` | 300 | New — ADT (opaque `wifitask_handle_t` + static pool) |
| `firmware/gateway/middleware/wifi_task/wifi_task.c` | 655 | New |
| `tests/gateway/middleware/wifi_task/test_wifi_task.c` | 809 | New — 26 TCs |
| `firmware/gateway/integration-tests/wifi_task/main_test_wifi_task.c` | 557 | New — Step 6 deliverable, 6 hardware TCs |

---

## Reused infrastructure

| File | Status | Symbols added (if extended) |
|------|--------|-----------------------------|
| `tests/mocks/freertos_mock.c` | reused / extended | Queue and task-notification stubs driving the request/reply round trip; notification-index support for WIFITASK-T15/T16 |
| `firmware/gateway/drivers/wifi_driver/` | reused | none — WifiTask is a relocation layer over the existing `wifi_*()` API |
| `firmware/gateway/drivers/{gpio,spi,exti}/` | reused | none — consumed by the integration main for the ISM43362 wiring |
| `firmware/gateway/middleware/logger/` | reused | none — the integration main reports through real Logger, not raw UART |
| `tests/project.yml` | extended | `:test_wifi_task:` defines |

---

## Unit test results

`tests/gateway/middleware/wifi_task/test_wifi_task.c` — re-run 2026-09-16.

| Test ID | Description | Result |
|---------|-------------|--------|
| WIFITASK-T01 | `wifitask_create()` happy path | PASS |
| WIFITASK-T02 | `create()` NULL args | PASS |
| WIFITASK-T03 | `create()` pool exhaustion | PASS |
| WIFITASK-T04 | `connect_ap()` builds request, returns the notified value | PASS |
| WIFITASK-T05 | Queue full returns timeout | PASS |
| WIFITASK-T06 | No reply returns timeout | PASS |
| WIFITASK-T07 | NULL-arg checks | PASS |
| WIFITASK-T08 | `step()` dispatches `connect_ap` | PASS |
| WIFITASK-T09 | `step()` dispatches open_socket / send / recv / close | PASS |
| WIFITASK-T10 | Two callers each get their own reply | PASS |
| WIFITASK-T11 | Liveness check fires when the queue is empty | PASS |
| WIFITASK-T12 | Liveness check skipped when the link is down | PASS |
| WIFITASK-T13 | Liveness probe failure attempts reconnect | PASS |
| WIFITASK-T14 | `reset_for_test()` clears the pool | PASS |
| WIFITASK-T15 | `wait` uses a dedicated notify index | PASS |
| WIFITASK-T16 | `notify_back` uses a dedicated notify index | PASS |
| WIFITASK-T17 | `try_recv()` never blocks on a fresh socket | PASS |
| WIFITASK-T18 | Arm → step → pickup returns READY | PASS |
| WIFITASK-T19 | Pickup of a timeout reports NONE, not ERROR | PASS |
| WIFITASK-T20 | An armed slot does not starve a real request | PASS |
| WIFITASK-T21 | `try_recv()` from IDLE kicks the request queue | PASS |
| WIFITASK-T22 | `step()` ignores a NULL kick without dispatching | PASS |
| WIFITASK-T23 | Partial pickups are consecutive, never duplicated | PASS |
| WIFITASK-T24 | No re-arm over an unconsumed payload | PASS |
| WIFITASK-T25 | An empty attempt is short and does not wake the owner | PASS |
| WIFITASK-T26 | `close_socket` resets the recv slot | PASS |

**Total:** 26 pass, 0 ignored.

Ignored tests (with reason): none.

---

## Integration test — expected behaviour

`firmware/gateway/integration-tests/wifi_task/main_test_wifi_task.c`. Reports
through real Logger on USART1/PB6 @ 115 200 8N1. Requires
`firmware/gateway/certs/bringup_secrets.h` (gitignored) for SSID/password/host.

Scope: unlike `main_test_wifi_driver.c`, this calls `wifitask_*()` exclusively,
so it proves the request-queue → dispatch → notify round trip between two real,
separately-scheduled FreeRTOS tasks — not just against the host `freertos_mock.c`.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | `TC-HW-WIFITASK-001` gpio_init + spi_create + wifi_create all return OK | WifiDriver Phase 1 is sound before WifiTask is layered on |
| 2 | `TC-HW-WIFITASK-002` `wifitask_create()` succeeds | Spawns WifiTask's own task and registers the DATARDY callback internally; must run post-scheduler |
| 3 | `TC-HW-WIFITASK-003` `get_link_state()` reports `WIFI_LINK_DOWN` pre-connect | The simplest possible cross-task request/notify round trip works on real silicon |
| 4 | `TC-HW-WIFITASK-004` `connect_ap()` + `get_rssi()` routed through WifiTask | A long, blocking operation survives the queue relocation |
| 5 | `TC-HW-WIFITASK-005` TCP open → send → `recv()` → close | Real data transfer across the task boundary |
| 6 | `TC-HW-WIFITASK-006` Same over UDP | Datagram path |

**Not exercised here:** WIFITASK-O3 (MqttClient calling WifiDriver directly —
since resolved), and the liveness/reconnect failure path, which needs a
physically triggered AP drop mid-run. The closing heartbeat loop runs long
enough for the 30 s liveness tick to fire at least once with nothing to do,
which is as far as this harness goes toward proving that path is alive.

---

## Deviations from companion

None. ADT pattern applied as the Gateway default (opaque handle + static pool).

---

## Problems encountered

**The module's Phases 1–3 had never been run on hardware until 2026-09-13,
and the first run failed `TC-HW-CP-004` three times over — each layer hiding
the next.** None of it was visible to host tests, because none of it was
logic: it was timing and byte-stream semantics.

1. **No wake on arm.** With the slot IDLE, `prv_wifitask_step()` parks in
   `xQueueReceive(request_queue)` for `WIFI_LIVENESS_CHECK_PERIOD_MS` (30 s)
   and only polls the arm queue at the top of a step. The first
   `wifitask_try_recv()` after any idle period therefore waited up to 30 s —
   exactly MqttClient's handshake deadline. A packet capture showed the module
   ACKing the whole server flight at +7 ms while the firmware never read it.
   *Fix:* `try_recv()` from IDLE also sends a NULL-pointer "kick" into
   `request_queue`, which the step treats as wake-only.
2. **Stream semantics.** Pickup copied `min(scratch_len, buf_len)` from offset
   0 and left the slot DONE, so mbedTLS's 5-byte-header-then-body reads got the
   header twice and lost the rest; the re-arm sent by the first pickup let a new
   attempt overwrite `scratch` underneath it. *Fix:* a `scratch_off` consumption
   cursor, pickup/publish inside a critical section, and re-arm refused while an
   OK payload is unconsumed.
3. **Poll cadence.** Each background attempt used the 5 s
   `WIFITASK_WIFI_RESP_TIMEOUT_MS`, blocking WifiTask — and therefore the same
   owner's queued `wifitask_send()`s — for 5 s per *empty* poll; the handshake's
   four-send client flight drifted to 19 s+ between records. Root cause was one
   layer down (WIFI-O16: `R2` never set, so the module chose the hold-off).
   *Fix:* attempts use `WIFITASK_RECV_POLL_TIMEOUT_MS` (200 ms) and an empty
   attempt no longer wakes the owner.

Fixed in `7a66508`. Separately, the remaining wall-clock cost was **crypto, not
I/O**: RSA-2048 CertificateVerify + P-256 ECDHE at `-O0` alone exceeded 30 s.
Fixed in `d6609ff` by compiling `mbedtls-library` at `-O2` with
`MBEDTLS_HAVE_ASM` + `MBEDTLS_ECP_NIST_OPTIM`.

**Two further hardware-only bugs surfaced by the broker-down test** (fixed in
`b1322d3` / `38e02fa`): the recv slot was not reset across a socket reopen,
leaving a stale `POLL_ERROR` that produced handshake `-0x6800`
(`prv_recv_slot_reset` on open/close, WIFITASK-T26); and `wifi_close_socket()`
leaked the local `socket_open[]` slot when `P6=0` stop-client failed on a dead
peer, so the next `wifitask_open_socket()` returned `err=6` (WIFI-O17).

**Correction carried forward:** the companion's earlier claim that the ~30 s
handshake floor was module-side latency was **wrong** — it was `-O0` crypto plus
the unset `R2`. Do not re-adopt that assumption.

**Outcome:** connect in ~4 s (from ~30 s, and never at all on Phases 1–3).

---

## Open items

| ID | Item | Status |
|---|---|---|
| WIFITASK-O1 | Non-blocking recv. Phases 1–4 shipped and hardware-validated. | Phase 5 (unify `wifitask_recv()` on `try_recv()`, DATARDY wait) deferred, not currently planned |
| WIFITASK-O2 | Reconnect policy — backoff curve, attempt count, socket/session fate on AP drop | **Resolved** — CP-D13 exponential backoff 2 s→60 s, WIFI-O17 + WIFITASK-T26 socket handling, CP-D14 no give-up |
| WIFITASK-O3 | MqttClient called WifiDriver directly rather than through WifiTask | **Resolved** |
| WIFITASK-O4 | `WIFI_LIVENESS_CHECK_PERIOD_MS` (30 s) is a provisional placeholder, not validated against any requirement or field data | **Open** |
| WIFITASK-O5 | `WIFITASK_ENQUEUE_TIMEOUT_TICKS` / `WIFITASK_REPLY_TIMEOUT_TICKS` referenced but not assigned concrete values | **Open** |
| WIFITASK-O6 | Request/reply used the default task-notification index (0), colliding with any caller that also uses notifications | **Resolved** — dedicated notify index, pinned by WIFITASK-T15/T16 |

---

## PR title

feat: implement WifiTask for Gateway

---

## PR description

## What this PR contains

- `firmware/gateway/middleware/wifi_task/wifi_task.h` — ADT handle, request/reply API mirroring the `wifi_*()` surface, plus the non-blocking `wifitask_try_recv()`
- `firmware/gateway/middleware/wifi_task/wifi_task.c` — request queue, dispatch step, per-slot background recv poll, liveness tick
- `tests/gateway/middleware/wifi_task/test_wifi_task.c` — 26 unit tests
- `firmware/gateway/integration-tests/wifi_task/main_test_wifi_task.c` — 6 hardware TCs
- `docs/lld/middleware/wifi-task.md` — companion at Phase H complete

## Design decisions

- **WIFITASK-D1..D6** per the companion — WifiTask is a *synchronous relocation*: every `wifitask_*()` call blocks for the same wall-clock duration the underlying `wifi_*()` call takes; what changes is *where* the block executes.
- **Dedicated task-notification index** for wait/notify-back, so a caller that uses notifications for its own purposes cannot collide (WIFITASK-O6).
- **`try_recv()` kick from IDLE** uses a value-typed NULL pointer in the request queue — nothing to dereference, and no heap or queue sets are available for the textbook answer.

## Test evidence

All 6 CI checks green.
Unity host tests: 26 pass, 0 fail, 0 ignore.
Integration test validated on L475 hardware (2026-09-13): TLS connect ~4 s, data-integrity 33 948 B with zero corruption, mid-flight broker restart recovers in ~12 s.

## Open items carried forward

- WIFITASK-O4 liveness period (30 s) unvalidated.
- WIFITASK-O5 enqueue/reply timeout constants unassigned.
- WIFITASK-O1 Phase 5 deferred.
