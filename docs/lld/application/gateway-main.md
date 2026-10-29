# Gateway `main()` and task set — analysis for Pass H (WP4, session 1)

**Document:** `docs/lld/application/gateway-main.md`
**Version:** 0.1 (DRAFT, analysis only — no code written)
**Status:** Waits for Pass H. Session 2 (implementation) must not start before the decisions in §8 are made.
**Board:** Gateway (B-L475E-IOT01A)
**Date:** 2026-10-29 · **Repo state reviewed:** `main` @ 7328305
**Closes when implemented:** LC-O15. **Partly:** LC-O8 (stated, not fixed), TS-O6 (production wiring), FS-O4 (stack and arena figures logged).

Sources: `lifecycle-controller-lld.md` §7.2, §17, §18; `docs/hld/task-breakdown.md` §5;
`integration-tests/lifecycle_wiring` and `integration-tests/cloud_publisher`;
`docs/module-implementation-status.md` §6; the headers and task constants of every Gateway
module; the symbol table of the last full build (`Debug/gateway.elf`, the CloudPublisher image).

---

## 1. Summary

The plan's creation order (drivers → middleware → application `create` calls →
`lifecycle_wiring_bind` → `lifecycle_controller_init` → tasks → scheduler) **cannot be built
as written.** Four findings change it; each needs a decision (§8) before the code is written.

| # | Finding | Effect |
|---|---|---|
| F-1 | **The cloud chain needs the scheduler.** `wifitask_create()` is documented "task-context only, after the scheduler has started"; `mqtt_client_create()`, `ntp_client_create()` take its handle; `cloud_publisher_create()` takes the MQTT handle; `update_service_create()` needs the CloudPublisher handle | `main()` cannot create CloudPublisher or UpdateService before `vTaskStartScheduler()` (§4) |
| F-2 | **CloudPublisher ↔ UpdateService is circular.** `cloud_publisher_config_t` needs the `update_service_t *` (both-or-neither); `update_service_config_t` needs an `iota_transport_t` whose function pointers are checked non-NULL at create, and the transport comes from `cloud_publisher_get_ota_transport(handle)` | One of the two must be changed or late-bound (D-2). The WP1 spec (§ "Creation order") assumed the transport comes first and missed that CloudPublisher needs UpdateService at create |
| F-3 | **The real image does not fit in the 96 KB RAM bank.** The CloudPublisher image already has 2 008 B spare; the modules not in it add at least 19 KB of stacks alone (§6) | A memory decision is mandatory (D-1). SRAM2 (32 KB) is empty and has a ready `.bss.sram2` section, but the 44 KB arena does not fit in it |
| F-4 | **Nothing in production code joins the WiFi access point.** `wifitask_connect_ap()` is called only from harnesses; no module re-associates after a drop | `main()` (or a boot task) must own association and its retry policy (D-3) |

Smaller findings: TimeService is created by the Lifecycle adapter, not by `main()`, and its
interface pointer is NULL when CloudPublisher is created (F-5); `us_probe_if_t` has no
production provider (F-6); the HLD task table disagrees with the code on five tasks (F-7).

---

## 2. What exists today

| Piece | State |
|---|---|
| `Src/main.c` of `Gateway-App` | Empty `for(;;)` (Cube stub) |
| Real application entry | None. Every image is a bring-up harness selected through `.cproject` exclusions |
| Closest draft | `integration-tests/lifecycle_wiring/main_test_lifecycle_wiring.c` (560 lines): `lifecycle_wiring_bind` → `lifecycle_controller_init` → three tasks. Stubs `time_service_init`; has no CloudPublisher, UpdateService or Console |
| Start gate | `lifecycle_get_start_gate()`; waited on by exactly one task today, `lifecycle_wiring_poller_task` |
| Credentials | `firmware/gateway/certs/` has `bringup_secrets.h`, `bringup_certs.h` and the PEM files; used only by harnesses |

---

## 3. Who creates which task

`main()` does not create most of the tasks. Seven modules create their own task inside
their `create`/`init` call; `main()` creates three.

| Task | Created by | When | Source |
|---|---|---|---|
| idle, timer service | FreeRTOS (static callbacks in `freertos_hooks.c`) | scheduler start | `configMAX_PRIORITIES` = 7, timer priority 6 |
| `log_drain` | `logger_init()` | before the scheduler | `logger.c:292` |
| `SensorTask` | `sensor_task_create()` | before the scheduler (SS-D9: sensor drivers need it) | `sensor_task.c:66` |
| `WifiTask` | `wifitask_create()` | **after** the scheduler (F-1) | `wifi_task.c:409` |
| `CloudPublisherTask` | `cloud_publisher_create()` | after `wifitask_create` (F-1) | `cloud_publisher.c:318` |
| `UpdateServiceTask` | `update_service_create()` | after CloudPublisher (F-1, F-2) | `update_service.c:121` |
| `TimeServiceTask` | `time_service_init()`, which the Lifecycle adapter `time_init` calls in StartingMiddleware | after the scheduler | `time_service.c:106` |
| `ModbusPollerTask` | `main()`, entry `lifecycle_wiring_poller_task` | before the scheduler | wiring header |
| `ConsoleTask` | `main()`, entry `console_task_body` (`console_service_init` creates none) | before the scheduler | `console_service.h` |
| `LifecycleTask` | `main()`, entry `lifecycle_task_body` | last | companion §17 |

---

## 4. Proposed creation sequence (for decision)

Two phases, because of F-1. Phase B runs in task context.

```
main():                                              -- Phase A, before the scheduler
    cpu_init; tls_arena_init                         (first: before any mbedTLS call)
    gpio, debug_uart, rtc, qspi_flash, internal_flash, spi, wifi_create (driver only)
    logger_init                                      (creates log_drain)
    led_init, health_monitor_create, time_provider_init
    config_store_create, config_service_init
    i2c, sensor_task_create                          (creates SensorTask)
    modbus_uart_init, modbus_master_init
    console_service_init                             (after the services it uses)
    xTaskCreateStatic: ModbusPollerTask (gated), ConsoleTask
    xTaskCreateStatic: BootTask                      (NEW, see below)
    vTaskStartScheduler

BootTask (priority above every application task, runs once):   -- Phase B
    wifitask_create                                  (creates WifiTask)
    join the access point                            (F-4, D-3)
    ntp_client_create, mqtt_client_create            (callbacks: cloud_publisher_on_message / _on_disconnect)
    firmware_store_create
    cfl_create, saf_create
    [time_service_init]                              (D-4, so CloudPublisher gets a non-NULL ITimeService)
    cloud_publisher_create                           (creates CloudPublisherTask)
    update_service_create                            (creates UpdateServiceTask; needs F-2 resolved)
    lifecycle_wiring_bind, lifecycle_controller_init
    lifecycle_task_body()                            (BootTask becomes LifecycleTask; never returns)
```

Why a `BootTask` rather than the plan's "LifecycleTask last": the order above needs one
task-context sequence, and `lifecycle_task_body` already runs the Init sequence itself. Making
the boot task *become* LifecycleTask avoids a second task and keeps "LifecycleTask owns Init".
This is a design choice, not a given; D-1 in §8 lists the alternative.

Stack consequence: BootTask runs `mqtt_client_create`, `cloud_publisher_create` and the
`lifecycle_task_body` frame, so it must be at least as large as the biggest of them. The
harness used 2 048 words for LifecycleTask; the LLD §18 says 8 192 B. Both agree.

---

## 5. Task table

Values are **from the code**, not the HLD (F-7). "Gate" = waits on `lifecycle_get_start_gate()`.

| Task | Prio | Stack (words / B) | Gate | HLD §5.2 says | Notes |
|---|---|---|---|---|---|
| `SensorTask` | 5 | 384 / 1 536 | no | 5 / 512 | starts at create; Lifecycle adapters only verify (SS-D9) |
| `ModbusPollerTask` | 4 | 512 / 2 048 | **yes** | 4 / 512 | `modbus_poller_init` runs in StartingMiddleware, so the task must wait |
| `WifiTask` | 3 | 256 / 1 024 | no | 3 / 256 | |
| `CloudPublisherTask` | **3** | **768 / 3 072** | **no** (required) | **2 / 2 048** | priority equals WifiTask. Not found in the HLD why; confirm (D-5) |
| `TimeServiceTask` | 2 | 768 / 3 072 | no | 2 / 768 | created by `time_service_init` |
| `UpdateServiceTask` | 1 | 1 024 / 4 096 | **no** (required) | 1 / 1 024 | 586 of 1 024 words free measured in TC-HW-US |
| `ConsoleTask` | 1 | 768 / 3 072 | no | 1 / 512 | |
| `LifecycleTask` | 1 | 2 048 / 8 192 | n/a | 1 / **256** | HLD value is wrong against LLD §18 |
| `log_drain` | 1 | 256 / 1 024 | no | not listed | |
| idle | 0 | 256 / 1 024 | n/a | default | |
| timer service | **6** | 256 / 1 024 | n/a | **7** | `configMAX_PRIORITIES` is 7, so the highest usable level is 6 |
| `BootTask` (proposed) | 6 or 2 | ≥ 2 048 / 8 192 | n/a | — | becomes LifecycleTask if D-1 option A |

Total task stacks: 29 184 B (28.5 KB), counting BootTask as LifecycleTask.

Why CloudPublisherTask and UpdateServiceTask must not wait: both start receiving before
Operational (MQTT connect on the first 1 Hz tick; the post-reboot OTA report in SelfChecking
needs the transport up). Waiting on a gate that opens only after SelfChecking would risk a deadlock
with the `resume_self_checking` path (inferred from the SelfChecking sequence in LLD §7.2, not tested).

---

## 6. Static RAM budget (F-3)

RAM bank: 98 304 B (`0x20000000`–`0x20018000`). SRAM2: 32 768 B, **empty**.

**Measured** — `Debug/gateway.elf`, the CloudPublisher harness image (after WP1 PR-D and the 44 KB arena):

| | Bytes |
|---|---:|
| `.bss` + `.data` | 96 296 |
| Free below `_estack` (`_estack` − `_ebss`) | 3 544 |
| Reserved by the linker for heap + MSP (`_Min_Heap_Size` + `_Min_Stack_Size`) | 1 536 |
| **Spare** | **2 008** |

Largest objects in it: `s_arena` 45 056 · `g_pool` of StoreAndForward 8 184 · MqttClient 6 312 ·
CloudPublisher 2 872 · bring-up task stack 4 096 (harness only) · logger queue 2 496.

**Not in that image** (it links no lifecycle, update, sensor, console, poller, master or time
service). Lower bound, stacks only, from §5:

| Added | Bytes |
|---|---:|
| LifecycleTask / BootTask stack | 8 192 |
| UpdateServiceTask stack | 4 096 |
| ConsoleTask stack | 3 072 |
| TimeServiceTask stack | 3 072 |
| ModbusPollerTask stack | 2 048 |
| SensorTask stack | 1 536 |
| `s_cfg_snapshot` (`LIFECYCLE_CFG_SNAPSHOT_BYTES`, default; LC-O1 allows 32 712) | 1 024 |
| Subtotal | 23 040 |
| Minus the harness-only bring-up stack | −4 096 |
| **Net added** | **18 944** |

**Deficit ≥ 16.9 KB** (18 944 − 2 008), before the module state of UpdateService, ConsoleService,
SensorService, ModbusPoller and ModbusMaster, which I could not measure without linking an image
that does not exist. The LC-O1 snapshot at its maximum would add 31 KB more.

What would still have to be measured before the budget is final: the arena peak with a TLS
session open (TC-HW-MQTT-014, FS-O4), and every stack's high-water mark (the plan's
"done when" requires them logged).

---

## 7. OTA bindings and other wiring (checklist for session 2)

| Binding | Value | Note |
|---|---|---|
| MQTT message callback | `cloud_publisher_on_message` | takes `msg_cb` at `mqtt_client_create()`, before CloudPublisher exists; early messages are dropped by design |
| MQTT disconnect callback | `cloud_publisher_on_disconnect` | mandatory |
| `control_handler` | `lifecycle_handle_remote` | |
| `fd_slave_addr` | 1 | |
| `fw_version` | from `firmware_boot_info_t.running_version` | needs `firmware_store_create` first |
| `cfg_read` / `cfg_write` | `config_service_get_provider()` / `_get_manager()` | |
| `lifecycle` | `lifecycle_controller` (static const vtable; valid before `lifecycle_controller_init`) | checked in `lifecycle_controller.c:787` |
| `time` | `time_service_get_interface()` | **NULL until `time_service_init` ran** (F-5) |
| `update_ops` / `update` | `&update_service_ops` / the instance | F-2 |
| `update_service_config_t.transport` | from `cloud_publisher_get_ota_transport()` | F-2 |
| `update_service_config_t.probes` | `us_probe_if_t{sensor_service_is_ready, modbus_poller_is_ready}` | **no production provider exists** (F-6) |
| `update_service_config_t.lifecycle` | `lifecycle_controller` | |
| Lifecycle wiring config, OTA members | `&update_control_ops`, the instance, `&firmware_store_boot_ops`, the FirmwareStore | all four or none |
| `running_image_base` | `cpu_get_vector_table_base()` | |
| Credentials | `certs/` handling as the harnesses; nothing committed | |

---

## 8. Decisions needed

### D-1 RAM (F-3)

| Option | What | Cost / risk |
|---|---|---|
| **A (recommended first step)** | Move the large blocks that nothing needs in the fast bank to SRAM2 with `__attribute__((section(".bss.sram2")))` (the section exists): StoreAndForward pool 8 184, MqttClient pool 6 312, CloudPublisher pool 2 872, logger queue 2 496 = **19 864 B**, and optionally task stacks | Covers the 16.9 KB lower bound with about 3 KB to spare: thin. Needs a check that no DMA or ISR path reads them from a location SRAM2 does not allow (not verified) |
| B | Shrink the TLS buffers (smaller record size, needs broker support) and re-measure the arena | Changes MQTT behaviour; needs TC-HW-MQTT-014 first |
| C | Trim stacks after the high-water marks are logged | Cannot be decided before the first run; LifecycleTask 8 KB is the biggest single item |
| D | A + C, decided in two steps: A now, C after measurement | My recommendation |

The arena (44 KB) **cannot** move to SRAM2: it is larger than the bank.

**Option E, TLS inside the WiFi module, was investigated on the bench (2026-10-29) and is closed.**
With the module's own TLS the host would need no record buffers (most of the arena). Results on the
board with Mosquitto, firmware `C3.5.2.3.BETA9` (as shipped), `C3.5.2.7.STM` and the release `C3.5.2.3`:

| Mode | Result |
|---|---|
| `P9=0` (no verification, no client certificate), ECDSA server certificate | Works: handshake 350-600 ms, MQTT CONNECT/CONNACK/PUBLISH through it, TLS 1.2 with ECDHE-ECDSA-AES256-CBC-SHA. **Unauthenticated, so unusable for the product** |
| `P9=0` with our RSA server certificate | Refused by the broker: `no suitable signature algorithm` (the module offers ECDSA signature algorithms only) |
| `P9=2` (verify server, client identity) with RSA, ECDSA-signed-by-RSA and all-ECDSA chains; PEM, DER and PEM+NUL; certificate sets 0-3 | **Always** `Unable to initialize TLS identity. Error = [3036]` (or `Initialize Identity failed` on `C3.5.2.7`), before any network traffic. Not documented anywhere online |

Certificate sets are write-once (cleared only by a full module reflash), so each attempt costs a reflash.
The bench harness is the throwaway branch `spike/wifi-module-tls` (`spikes/wifi_module_tls/`, local, not pushed).

**Untested route (GM-O7):** every `P9=2` attempt loaded the credentials at run time with `PG` from the host.
Inventek's own documented flow for authenticated TLS (`AN20096` AWS app note, "TCP-SSL-TLS How to Create
Firmware Image") instead bakes the PEM certificate and PEM key into a customized firmware image with the
Windows eS-WiFi Demo program (`v1.5.zip`, Firmware > Customize, base image `C3.5.2.3`) and reflashes the
module; host `PG` is listed only as an alternative. It might work where `PG` does not. It was not tried
(needs the Windows tool), and even if it works it fixes the certificates into the firmware image, which fits
per-device certificates poorly. Recommendation stays: keep host mbedTLS (options A and D).

### D-2 CloudPublisher ↔ UpdateService cycle (F-2)

| Option | What |
|---|---|
| **A (recommended)** | `update_service_get_instance()` returning the static `g_us` before create. CloudPublisher stores the pointer and dereferences it only when an OTA message arrives; `is_created` already guards that path. Order: CP (with the pointer) → transport → `update_service_create` |
| B | `cloud_publisher_set_update(handle, ops, update)` called once after both exist, before the first connect. Adds mutable state to CloudPublisher |
| C | Fill a static `iota_transport_t` after CP create and relax UpdateService's non-NULL check at create. Weakens a check that exists for a reason |

### D-3 WiFi association (F-4)

Who joins the AP, with what retry policy, and who re-joins after a drop (CloudPublisher
reconnects MQTT only)? **Bench evidence (2026-10-29):** the first `wifi_connect_ap()` after a module reset
returned a bare module error (code 3) in almost every run, on both `C3.5.2.3` and `C3.5.2.7`, and the second
attempt succeeded, so a retry is required whatever else is decided. Proposal: BootTask joins with a bounded retry before creating the MQTT
client, and a drop is left to a later WP. This is a policy decision and affects availability; not made here.

### D-4 TimeService creation (F-5)

`time_service_init` is idempotent. Calling it from Phase B **before** `cloud_publisher_create`
makes `time_service_get_interface()` non-NULL, so CloudPublisher gets its NTP-retry-on-reconnect
(TS-O6). The Lifecycle adapter's later call is then a no-op. Alternative: leave it and accept
that production CloudPublisher has no NTP retry — which would silently reopen TS-O6.

### D-5 HLD table versus code (F-7)

CloudPublisherTask is priority 3 / 768 words in code and 2 / 2 048 in the HLD; SensorTask,
ConsoleTask, LifecycleTask stacks and the timer priority also differ. I did not find a recorded
decision for CloudPublisher's priority. Confirm which side is authoritative; the HLD (or the
code) is then corrected in a separate docs PR.

### D-6 Startup structure

BootTask becomes LifecycleTask (§4, proposed) **versus** a separate short-lived BootTask that
creates LifecycleTask last and deletes itself (matches the plan's wording, costs one more
stack during boot and a `vTaskDelete` of a static task).

---

## 9. Watchdog

None. The Gateway has no watchdog driver; `lifecycle_deps_t.watchdog` stays NULL and nothing is
kicked (LC-O8). A hung task will not reset the device. This is stated here so the "done when"
for WP4 is not read as covering it.

---

## 10. Session 2 plan (after Pass H)

1. `firmware/gateway/Src/main.c` per §4 with the D-1..D-6 decisions applied; no secrets committed.
2. Boot log: one line per Init sub-state (the controller already emits events; verify what it logs), and a periodic high-water-mark line for every task in §5 plus arena peak (`MBEDTLS_MEMORY_DEBUG` is already on).
3. Host tests cannot cover `main()`; add a link-level check that the image has no undefined symbols and that `.bss` + stacks stay under the bank (a script over `arm-none-eabi-size`).
4. `.cproject` changes (exclusions for the new image) are **written in the PR description, not made** (standing rule).
5. Hardware cases for Luca: boots to Operational; telemetry and health reach Mosquitto; `/control` restart and `/config` set work; stack marks logged.

---

## 11. Open items opened by this document

| ID | Item |
|---|---|
| GM-O1 | RAM does not fit (F-3, D-1) |
| GM-O2 | CP/US creation cycle (F-2, D-2) |
| GM-O3 | WiFi association owner and policy (F-4, D-3) |
| GM-O4 | `us_probe_if_t` provider (F-6): proposal is an adapter in `lifecycle_wiring` next to the others |
| GM-O5 | HLD task table out of date (F-7, D-5) |
| GM-O6 | `wifitask_create` "task-context only" rule: the code in `wifi_task.c:409-420` shows no scheduler dependency (it creates a task and attaches a NULL callback). If the rule is stale, Phase B shrinks; I have not tested it pre-scheduler, so it is kept as a constraint |
| GM-O7 | Untested route for on-module TLS: Inventek's firmware-image customization (Windows eS-WiFi Demo, Firmware > Customize) instead of run-time `PG`. See D-1, option E. Not pursued; reopen only if per-fleet certificates baked into the module image are acceptable |
| GM-O8 | `docs/SRS.md` CON-001 and `docs/lld/drivers/spi-driver.md:31` still say "All TCP/IP and TLS operations are handled by the module's internal stack". The design (WIFI-D2) and the bench say otherwise: TLS runs on the host with mbedTLS. Correct the requirement text |
