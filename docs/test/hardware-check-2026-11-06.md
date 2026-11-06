# Hardware check — 2026-11-06

**Images:** Gateway Release path via `scripts/build-gateway-app.sh --flash`, Field Device via `scripts/build-fd-app.sh --flash`, both built from the working tree (`0c157b3`-based, docs-only changes on top). Broker: local Mosquitto, TLS mutual auth. Alarm bench: Raspberry Pi RS-485 dongle.

## Results

| Check | Requirement | Result |
|---|---|---|
| Reset → Operational | REQ-NF-213 (10 s) | **PASS** — 4–5 s (two cold boots, one remote restart); MQTT connected 15–16 s after reset |
| Remote restart, confirmed | REQ-DM-010/020, NF-203 | **PASS** — command → Operational about 7 s; reset → Operational 4 s |
| Remote restart, unconfirmed | REQ-DM-020/021, NF-307 | **PASS** — cancelled after 30 s (`Restarting timed out -> Operational`) |
| Malformed / unknown `/control` payload | REQ-MB-140 | **PASS (log only)** — `unknown control command ignored`; no reply reaches the cloud (CP-O8) |
| Alarm latency, 10 raise + 10 clear | REQ-NF-113 (500 ms) | **PASS** — raise median 124 / max 285 ms; clear median 221 / max **471** ms; bus-to-publish gap median 13 ms. Margin on the clear path is 29 ms |
| Telemetry period and QoS | REQ-NF-111, NF-206 | **PASS** — 60 s, QoS 0, schema 1.1 |
| Time push on link-up and after each NTP sync | REQ-TS-030, NF-211 | **PASS** — Field Device logged `CMD_SET_TIME applied` after both |
| Time re-push after the FD was held in reset 10 s | REQ-TS-060 | **PASS** — node offline logged within 1–2 s, then `CMD_SET_TIME applied` after the FD resumed |
| Node offline after failed polls | REQ-NF-215 | **PASS** — `slave 1 node offline` 1 s after the FD stopped |
| Broker outage 105 s, then recovery | REQ-BF-000/010, NF-200 | **PASS** — two telemetry messages buffered and replayed in timestamp order, then live |
| Reconnect rate | REQ-NF-209 (1 Hz) | **PASS against SRS v1.4** (was FAIL against the old 1 Hz text) — back-off 2, 4, 8, 16, 32, 60 s. Reconnected 30 s after the broker returned |
| Stacks | — | lowest: Tmr Svc 84 words, SensorTask 87 words; TLS arena peak 20 680 of 28 672 bytes |

## Findings (new)

1. **Health message is incomplete (CP-O9).** The 20 metrics carry `stack_free_words: []`, no free heap, CPU load or MCU temperature, and `last_ntp_sync_epoch: 0` although NTP had synced. REQ-CC-010/LD-070 list those items.
2. **A health message was not delivered after the outage.** One was due at about 21:22:28, inside the outage; none arrived on reconnect, and the next came at 21:27:14 (`buffer_entry_count` 0). REQ-BF-000 says health is buffered. Cause not isolated.
3. **`fw_version` reports `1.0.0`** while the console shows `v0.0.0+0c157b3-dirty` (REQ-NF-503 wants one version).
4. **Field Device `status` shows `Modbus frames: 0` and `Uptime: 0 s`** while the Gateway counts 1785 good transactions; the two counters are not fed.
5. **`openocd reset run` did not reset the Field Device** with `dapdirect_swd`; `reset halt` + `resume` did.
6. **The FD Modbus slave stack has 109 of 512 words free** (`stack modbus`), the lowest on that board.
7. **REQ-SA-170 / REQ-NF-308 (on-demand sensor read) has no command.** `/control` accepts only `restart`, `restart_confirm` and `reset_metrics`.

## Not run

OTA over MQTT (needs a signed release, bootloader and slot images), true power-cut tests, RTC drift measurement, Field Device display.
