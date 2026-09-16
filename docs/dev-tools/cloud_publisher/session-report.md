# Session Report — CloudPublisher (Gateway)

**Date:** built pre-2026-09-13; hardware-validated 2026-09-13. Backfilled 2026-09-16.
**Branch:** `feature/phase-4-gw-cloud_publisher` → merged as PR #66 (`e17a0f9`), 46 commits
**Companion:** `docs/lld/application/cloud-publisher-lld.md` v0.2, Status: Implementation-ready (Phase H complete)

> **Backfill note.** Step 12 was not performed when this module was built.
> Reconstructed 2026-09-16 from the companion, merged source, the test suite
> (re-run for this report) and commit history. Counts and timings are measured.

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/gateway/application/cloud_publisher/cloud_publisher.h` | 344 | New — ADT handle, config injection, notify bits |
| `firmware/gateway/application/cloud_publisher/cloud_publisher.c` | 686 | New — task, timers, queues, reconnect |
| `firmware/gateway/application/cloud_publisher/cloud_publisher_json.h` | 24 | New |
| `firmware/gateway/application/cloud_publisher/cloud_publisher_json.c` | 128 | New — telemetry/health/alarm serialisation |
| `tests/gateway/application/cloud_publisher/test_cloud_publisher.c` | 837 | New — 22 TCs |
| `firmware/gateway/integration-tests/cloud_publisher/main_test_cloud_publisher.c` | 913 | New — Step 6 deliverable, 12 hardware TCs |
| `scripts/bringup-data-integrity.py` | — | New — laptop-side driver for TC-HW-CP-010 |

---

## Reused infrastructure

| File | Status | Symbols added (if extended) |
|------|--------|-----------------------------|
| `firmware/gateway/middleware/mqtt_client/` | reused | none — consumed via `IMqttClient` |
| `firmware/gateway/middleware/wifi_task/` | reused | indirectly, through MqttClient |
| `tests/mocks/freertos_mock.c` | reused / extended | timer and notification stubs for the tick-driven task |
| `tests/project.yml` | extended | `:test_cloud_publisher:` defines |

---

## Unit test results

`tests/gateway/application/cloud_publisher/test_cloud_publisher.c` — re-run 2026-09-16.

| Test ID | Description | Result |
|---------|-------------|--------|
| CP-T01 | `create()` happy path | PASS |
| CP-T02 | `create()` NULL config | PASS |
| CP-T03 | `create()` pool exhaustion | PASS |
| CP-T04 | Telemetry published when connected | PASS |
| CP-T05 | Telemetry when disconnected | PASS |
| CP-T06 | Health published when connected | PASS |
| CP-T07 | Alarm published when connected | PASS |
| CP-T08 | Alarm when disconnected | PASS |
| CP-T09 | SAF drain on reconnect | PASS |
| CP-T10 | SAF drain stops on publish failure | PASS |
| CP-T11 | Inbound config command | PASS |
| CP-T12 | Inbound unknown topic | PASS |
| CP-T13 | Stats polling | PASS |
| CP-T14 | JSON truncation | PASS |
| CP-T15 | Alarm queue full | PASS |
| CP-T16 | Configurable intervals | PASS |
| CP-T17 | Reconnect attempted when disconnected | PASS |
| CP-T18 | Reconnect backoff does not hammer every tick | PASS |
| CP-T19 | Reconnect in progress retries every tick without backoff | PASS |
| CP-T20 | `wifi_recv_ready` triggers a stats poll | PASS |
| CP-T21 | `wifi_recv_ready` bit cleared — no busy loop | PASS |
| CP-T25 | Reconnect backoff is exponential and resets | PASS |

**Total:** 22 pass, 0 ignored.

Ignored tests (with reason): none.

---

## Integration test — expected behaviour

`firmware/gateway/integration-tests/cloud_publisher/main_test_cloud_publisher.c`.
Needs `bringup_secrets.h` (gitignored) and, for TC-010/011/012, the laptop-side
`scripts/bringup-data-integrity.py` plus a stoppable local Mosquitto.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | `TC-HW-CP-001` gpio + spi + wifi create OK | Transport stack up |
| 2 | `TC-HW-CP-002` `wifitask_connect_ap()` associates | Routed through WifiTask |
| 3 | `TC-HW-CP-003` `mqtt_client_create()` succeeds, unconnected | Handle exists before CloudPublisher owns it |
| 4 | `TC-HW-CP-004` `mqtt_client_connect()` succeeds | Full TLS + CONNACK. Connects here *before* handing over, so TC-005 can subscribe from the same task — MqttClient is single-caller with no locking |
| 5 | `TC-HW-CP-005` `subscribe()` to the config topic; external publish arrives at `bringup_msg_cb` | Inbound path (CP-O5 stand-in, not routed into CloudPublisher) |
| 6 | `TC-HW-CP-006` `cloud_publisher_create()` returns OK | Queues, timers, task; `alarm_service_subscribe()` on the stand-in |
| 7 | `TC-HW-CP-007` Telemetry frame on `dt/iotmonitor/<serial>/telemetry` | Timer-driven publish |
| 8 | `TC-HW-CP-008` Health frame on `.../health` | Timer-driven publish |
| 9 | `TC-HW-CP-009` 1 Hz stats tick drives `get_stats()` repeatedly without crashing | CP-D10 ticked reconnect path is alive |
| 10 | `TC-HW-CP-010` Data integrity — patterned payloads via the laptop script | **PASS: 33 948 B, 0 corrupt** — the strongest proof the WifiTask stream-cursor fix is correct end to end |
| 11 | `TC-HW-CP-011` Wall-clock parity vs the ~30 s baseline | **PASS: ~4.7 s connect**, no regression |
| 12 | `TC-HW-CP-012` Alarm latency against REQ-NF-113 (500 ms) | Initially **FAILED** — see below; after MQTT-D13 + CP-D12: **240/240 alarms, max 125 ms, 0 over budget** |

**Note on TC-HW-CP-004's scope:** because this bring-up connects before handing
the handle over, CP-D9/CP-D10's "CloudPublisherTask's stats tick drives the
connect" behaviour is **not** exercised for the *initial* connect — only for a
later reconnect after a drop.

---

## Deviations from companion

1. **CP-D12 — drain alarms before telemetry.** Added after the companion, as
   part of the CP-O6 fix: an alarm must not queue behind a telemetry publish.
2. **CP-D13 — exponential reconnect backoff**, 2 s → 60 s doubling, reset on
   connect. Minimum is 2 s **because the countdown ticks at 1 Hz** — a 1 s
   minimum is indistinguishable from no backoff at that resolution.
3. **CP-D14 — no give-up.** Indefinite retry with capped backoff. Correct for a
   mains-powered always-on gateway; safe because per-tick blocking is bounded
   and StoreAndForward is a bounded drop-oldest buffer.
4. **StoreAndForward is a drop-everything stand-in.** `cloud_publisher.c` calls
   the SAF interface, but no StoreAndForward module exists yet, so buffered
   data is discarded rather than persisted. **This is the one real hole in what
   shipped** and is the reason StoreAndForward was the leading candidate for the
   next module.

---

## Problems encountered

**REQ-NF-113 (alarm detection → publish within 500 ms) was not met on first
hardware measurement — CP-O6.** A single QoS 1 publish measured ~347 ms median
(one ISM43362 PUBLISH+PUBACK round trip), leaving almost no headroom, and a
broker drop stalled the path ~15 s.

Resolved across two changes rather than one:

- **MQTT-D13** (`3f7c7d7`) made `mqtt_client_publish()` non-blocking — transmit
  and return, PUBACK reaped in `process()` — and made `prv_mbedtls_net_send()`
  fast-fail a dead socket instead of looping to `MQTT_SEND_TIMEOUT_MS`. This
  killed the ~15 s stall and dropped the tail from 766 ms to 584 ms.
- **CP-D12** drained alarms ahead of telemetry.

**Honest characterisation:** this is **not a hard 500 ms guarantee.** A single
ISM43362 send is ~350 ms — about 70% of the budget (WIFI-O11) — so a co-in-flight
publish on the single CloudPublisherTask can still spike. A full guarantee needs
faster module I/O or a dedicated alarm path, both out of scope. Confirmed on
hardware (`6465fb9`/`40d28e9`): **240/240 alarms handled, max 125 ms, 0 over
budget, no stall**, where the blocking build stalled ~15 s.

**Reconnect took two further hardware-only fixes** before it worked (`b1322d3`,
`38e02fa`) — both in the layers below, documented in the WifiTask and MqttClient
bug logs. With them, a mid-flight broker restart recovers in ~12 s with alarms
buffered within budget throughout.

**Bench gotcha:** the ISM43362 cold-join goes flaky after many re-associations
in a session (`err=3` repeatedly) and needs a **physical USB power-cycle** — not
an openocd reset, and not an RST-pin reset. Also: always
`stty -F /dev/ttyACM0 115200 raw` before reading the VCP, and re-apply after any
board reset, or false "join failed" readings follow.

---

## Open items

| ID | Item | Status |
|---|---|---|
| CP-O1 | `IUpdateService` interface not yet defined; `update_svc` may be NULL | **Open** — define at UpdateService LLD |
| CP-O2 | `IModbusPoller.get_latest_fd_readings()` — confirm the method exists | **Open** — confirm at ModbusPoller LLD |
| CP-O3 | Largest telemetry payload must fit `CP_JSON_BUF_SIZE` (4096) | **Open** — validate with the full sensor set |
| CP-O4 | `field_device_valid = false` — omit vs null for the cloud consumer | **Open** — confirm with cloud schema design |
| CP-O5 | `msg_cb`/`disconnect_cb` registered at `mqtt_client_create()` time, upstream of CloudPublisher's config injection — inbound command delivery has no real wiring | **Open** |
| CP-O6 | REQ-NF-113 alarm latency | **Largely resolved** — 240/240 within budget, but not a hard guarantee (see above) |

---

## PR title

feat: implement CloudPublisher for Gateway

## PR description

## What this PR contains

- `firmware/gateway/application/cloud_publisher/cloud_publisher.{h,c}` — CloudPublisherTask: telemetry/health timers, alarm queue, 1 Hz stats tick, ticked reconnect with exponential backoff
- `cloud_publisher_json.{h,c}` — telemetry/health/alarm serialisation with truncation handling
- `tests/gateway/application/cloud_publisher/test_cloud_publisher.c` — 22 unit tests
- `firmware/gateway/integration-tests/cloud_publisher/main_test_cloud_publisher.c` — 12 hardware TCs
- `scripts/bringup-data-integrity.py` — laptop-side driver for TC-HW-CP-010
- `docs/lld/application/cloud-publisher-lld.md` — companion at Phase H complete

## Design decisions

- **CP-D12 alarms drained before telemetry** — an alarm must not queue behind a telemetry frame.
- **CP-D13 exponential backoff** 2 s → 60 s, reset on connect; 2 s minimum because the countdown ticks at 1 Hz.
- **CP-D14 no give-up** — indefinite retry with capped backoff, correct for a mains-powered always-on gateway.
- **CP-D9/CP-D10 connect driven by the task's own tick**, so no separate connect thread is needed.

## Test evidence

All 6 CI checks green.
Unity host tests: 22 pass, 0 fail, 0 ignore.
Integration test validated on L475 hardware (2026-09-13): data-integrity 33 948 B with zero corruption, connect ~4.7 s, alarm latency 240/240 within the 500 ms budget (max 125 ms), broker-drop reconnect ~12 s.

## Open items carried forward

- **StoreAndForward is a drop-everything stand-in** — offline buffering is the one real hole in what shipped.
- CP-O1/O2 dependent interfaces, CP-O3 payload sizing, CP-O4 cloud schema, CP-O5 inbound command wiring.
- CP-O6 alarm latency is met in practice but is not a hard guarantee while a single module send costs ~70% of the budget.
