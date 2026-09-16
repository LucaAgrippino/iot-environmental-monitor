# Session Report — MqttClient (Gateway)

**Date:** built pre-2026-09-13; hardware-validated 2026-09-13. Backfilled 2026-09-16.
**Branch:** `feature/phase-4-gw-cloud_publisher` → merged as PR #66 (`e17a0f9`)
**Companion:** `docs/lld/middleware/mqtt-client.md` v0.2, Status: Implementation-ready (Phase H complete)

> **Backfill note.** Step 12 was not performed when this module was built.
> Reconstructed 2026-09-16 from the companion, merged source, the test suite
> (re-run for this report) and commit history. Counts and timings are measured.

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/gateway/middleware/mqtt_client/mqtt_client.h` | 318 | New — ADT handle, vtable, stats, wake bits |
| `firmware/gateway/middleware/mqtt_client/mqtt_client.c` | 1039 | New |
| `firmware/gateway/middleware/mqtt_client/mqtt_topic_config.h` | 43 | New — topic templates |
| `firmware/gateway/middleware/mqtt_client/mbedtls_config_gateway.h` | 85 | New — trimmed mbedTLS feature set for the L475 |
| `tests/gateway/middleware/mqtt_client/test_mqtt_client.c` | 942 | New — 27 TCs |
| `firmware/gateway/integration-tests/mqtt_client/main_test_mqtt_client.c` | 755 | New — Step 6 deliverable, 12 hardware TCs |

---

## Reused infrastructure

| File | Status | Symbols added (if extended) |
|------|--------|-----------------------------|
| `vendor/coreMQTT` | reused | none — vendored library, `MQTT_DO_NOT_USE_CUSTOM_CONFIG` |
| `vendor/mbedtls` | reused | configured via `mbedtls_config_gateway.h`; **built at `-O2` with `MBEDTLS_HAVE_ASM` + `MBEDTLS_ECP_NIST_OPTIM`** (`.cproject` folder override) |
| `firmware/gateway/middleware/wifi_task/` | reused | none — transport is `wifitask_*()`, resolving WIFITASK-O3 |
| `tests/mocks/freertos_mock.c` | reused | none |
| `tests/project.yml` | extended | `:test_mqtt_client:` defines |

---

## Unit test results

`tests/gateway/middleware/mqtt_client/test_mqtt_client.c` — re-run 2026-09-16.

| Test ID | Description | Result |
|---------|-------------|--------|
| MQTT-T01 | `create()` happy path | PASS |
| MQTT-T02 | `create()` NULL config | PASS |
| MQTT-T03 | `create()` pool exhaustion | PASS |
| MQTT-T04 | `connect()` CONNACK accepted | PASS |
| MQTT-T05 | `connect()` CONNACK rejected | PASS |
| MQTT-T06 | `connect()` TLS handshake failure | PASS |
| MQTT-T07 | `publish()` QoS 0 | PASS |
| MQTT-T08 | `publish()` QoS 1 acked in `process()` | PASS |
| MQTT-T09 | `publish()` QoS 1 transmit failure | PASS |
| MQTT-T09b | QoS 1 missing PUBACK is **not** a failure | PASS |
| MQTT-T10 | `publish()` when not connected | PASS |
| MQTT-T11 | `subscribe()` happy path | PASS |
| MQTT-T12 | `subscribe()` SUBACK failure | PASS |
| MQTT-T13 | `process()` inbound PUBLISH | PASS |
| MQTT-T14 | `process()` keep-alive timeout | PASS |
| MQTT-T15 | `disconnect()` graceful | PASS |
| MQTT-T16 | `reset_stats()` | PASS |
| MQTT-T17 | `get_stats()` matches internal state | PASS |
| MQTT-T18 | Reconnect after keep-alive timeout | PASS |
| MQTT-T19 | Multi-cycle disconnect/reconnect, no socket exhaustion | PASS |
| MQTT-T20 | `is_connected()` reflects state | PASS |
| MQTT-T21 | `is_connected()` NULL handle | PASS |
| MQTT-T22 | `connect_step()` idle tick opens socket, returns IN_PROGRESS | PASS |
| MQTT-T23 | `connect_step()` TLS handshake ticks do not reopen the socket | PASS |
| MQTT-T24 | `connect_step()` TLS handshake deadline expires across ticks | PASS |
| MQTT-T25 | `connect_step()` full sequence reaches ESTABLISHED | PASS |
| MQTT-T26 | `connect_step()` NULL args | PASS |

**Total:** 27 pass, 0 ignored.

Ignored tests (with reason): none.

---

## Integration test — expected behaviour

`firmware/gateway/integration-tests/mqtt_client/main_test_mqtt_client.c`.
Needs `bringup_secrets.h` (gitignored) and, for TC-011, a **local Mosquitto you
can stop and restart on command** — a real AWS IoT Core endpoint serves every
other case but cannot be stopped on demand.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | `TC-HW-MQTT-001` gpio + spi + wifi create OK | Transport stack is up |
| 2 | `TC-HW-MQTT-002` `wifitask_create()` + `connect_ap()` associates | Routed through WifiTask, not WifiDriver (WIFITASK-O3) |
| 3 | `TC-HW-MQTT-003` `mqtt_client_create()` returns OK | Pool and context init |
| 4 | `TC-HW-MQTT-004` `connect()` completes TLS handshake + CONNACK | The whole mutual-auth path on an 80 MHz core with no crypto accelerator |
| 5 | `TC-HW-MQTT-005` QoS 0 telemetry publish | Fire-and-forget path |
| 6 | `TC-HW-MQTT-006` QoS 1 alarm publish, PUBACK observed | Acked path, PUBACK reaped in `process()` |
| 7 | `TC-HW-MQTT-007` `subscribe()` to the config topic, SUBACK observed | Inbound wiring |
| 8 | `TC-HW-MQTT-008` publish externally during the countdown → `msg_cb` fires | Real inbound delivery |
| 9 | `TC-HW-MQTT-009` `get_stats()` — `connect_ok == 1`, `publishes_sent == 2`, `publishes_acked == 1` | Counters track reality |
| 10 | `TC-HW-MQTT-011` Stop the broker → `disconnect_cb` fires on a **real** keep-alive/TCP failure; restart → reconnects | The hardware-only half of MQTT-O8: only real silicon proves the ISM43362 actually frees the socket on `P6=0` |
| 11 | `TC-HW-MQTT-012` `WIFI_MAX_SOCKETS` further cycles, broker up, fully automated | The module's real 4-slot socket table does not drift into exhaustion |
| 12 | `TC-HW-MQTT-013` `disconnect()` completes without invoking `disconnect_cb` | Graceful close is distinguishable from a drop |

**Result 2026-09-13:** TLS connect ~4 s (was ~30 s); mid-flight broker restart
recovers in ~12 s.

---

## Deviations from companion

1. **MQTT-D13 — `mqtt_client_publish()` made non-blocking** (commit `3f7c7d7`),
   after the companion was written. Publish now transmits and returns; the
   PUBACK is reaped in `mqtt_client_process()`. `MQTT_PUBACK_TIMEOUT_MS` was
   removed. Driven by CP-O6 (REQ-NF-113 alarm latency) — see the CloudPublisher
   report.
2. **MQTT-D8 — `mqtt_client_connect_step()`** — `connect()` became a thin
   wrapper looping the step to completion, so a caller's tick can drive the
   handshake instead of blocking for its whole duration.

---

## Problems encountered

- **The TLS handshake missed its 30 s deadline on first hardware run**
  (MQTT-O2 / MQTT-O10). Measured ~29 s for a full mutual-auth TLS 1.2
  handshake. **Root cause was not the radio** — it was RSA-2048
  CertificateVerify and P-256 ECDHE compiled at `-O0`. Fixed in `d6609ff` by
  building `mbedtls-library` at `-O2` with `MBEDTLS_HAVE_ASM` and
  `MBEDTLS_ECP_NIST_OPTIM`. Mosquitto 2.x independently drops clients that have
  not sent CONNECT within 30 s, which made the failure look like a broker
  rejection.
- **A dead socket could stall a publish for `MQTT_SEND_TIMEOUT_MS`.**
  `prv_mbedtls_net_send()` now fast-fails — **but only when `inst->connected`**.
  During a (re)handshake it must return `WANT_WRITE`, or a transient
  `WIFI_ERR_SOCKET` on a just-opened socket aborts every first connect and every
  reconnect. That conditional is the subtle half of the fix.
- **mbedTLS entropy** (MQTT-O6): the L475's RNG needs its *kernel* clock
  (`RCC->CCIPR.CLK48SEL`) selected separately from its bus-clock gate
  (`AHB2ENR.RNGEN`) — not covered by the companion's original hardware section.

---

## Open items

| ID | Item | Status |
|---|---|---|
| MQTT-O1 | mbedTLS RAM ~35–50 KB against a 128 KB SRAM budget; also bounding mbedTLS's internal calloc/free | **Open** — verify at integration |
| MQTT-O2 | `MQTT_CONNECT_TIMEOUT_MS` — provisional 10 000 ms proved insufficient (~29 s measured) | Addressed by the `-O2` crypto fix; constant still to be pinned |
| MQTT-O3 | `MQTT_PKT_BUF_SIZE` = 4096 provisional; must exceed the largest payload | **Open** — confirm max OTA chunk at UpdateService LLD |
| MQTT-O4 | `MQTT_SUBACK_TIMEOUT_MS` provisional 15 000 ms | **Open** |
| MQTT-O5 | Certificate storage partition address/format | **Open** — depends on QspiFlashDriver/ConfigStore |
| MQTT-O6 | CTR-DRBG entropy needs the real RNG with `CLK48SEL` set | **Open** |
| MQTT-O7 | `process()` not the fast non-blocking call §7's cadence assumes | Relocated to WIFITASK-O1; addressed by the `try_recv()` path |
| MQTT-O8 | Broker-drop robustness audit | Addressed; hardware-confirmed 2026-09-13 |
| MQTT-O9 | Two phases remain atomic single-tick calls with a larger worst case (TCP socket open) | **Open** |
| MQTT-O10 | Phase 4 bring-up: handshake missed the 30 s deadline | Resolved by `-O2` crypto (`d6609ff`) |

---

## PR title

feat: implement MqttClient for Gateway

## PR description

## What this PR contains

- `firmware/gateway/middleware/mqtt_client/mqtt_client.{h,c}` — coreMQTT over mbedTLS over WifiTask; connect/publish/subscribe/process/disconnect, stats, ticked connect state machine
- `mqtt_topic_config.h`, `mbedtls_config_gateway.h`
- `tests/gateway/middleware/mqtt_client/test_mqtt_client.c` — 27 unit tests
- `firmware/gateway/integration-tests/mqtt_client/main_test_mqtt_client.c` — 12 hardware TCs
- `docs/lld/middleware/mqtt-client.md` — companion at Phase H complete

## Design decisions

- **MQTT-D8 ticked connect** — `connect_step()` bounds per-tick blocking so a caller's 1 Hz tick can drive a multi-second handshake.
- **MQTT-D13 non-blocking publish** — transmit and return; PUBACK reaped in `process()`. Removed the ~15 s broker-drop stall and dropped the alarm tail from 766 ms to 584 ms max.
- **Transport is `wifitask_*()`**, not `wifi_*()` — resolves WIFITASK-O3.
- **mbedTLS at `-O2` with ASM + NIST optimisations** — a build-configuration decision that was worth ~25 s of handshake time.

## Test evidence

All 6 CI checks green.
Unity host tests: 27 pass, 0 fail, 0 ignore.
Integration test validated on L475 hardware (2026-09-13): TLS connect ~4 s, broker-drop reconnect ~12 s, socket table stable across `WIFI_MAX_SOCKETS + 1` cycles.

## Open items carried forward

- MQTT-O1 mbedTLS RAM budget, MQTT-O3 packet buffer size, MQTT-O5 certificate storage, MQTT-O6 RNG entropy, MQTT-O9 atomic socket-open phase.
