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


**Findings added by the bench (2026-10-30, PR #119):**

| # | Finding | Evidence |
|---|---|---|
| F-9 | **ModbusPollerTask must be created after `lifecycle_controller_init()`.** The start gate it waits on is created inside that call, which now runs in BootTask after the scheduler starts. Created in Phase A it calls `xEventGroupWaitBits(NULL, ...)` at scheduler start and hard-faults | Hard fault on the board; the PC decodes to `vTaskPlaceOnUnorderedEventList` from `xEventGroupWaitBits`. The firmware's own panic dump names the wrong function (its symbol table belongs to another image): decode with `addr2line` |
| F-10 | **Correction of F-3's premise: SRAM2 is not empty, and there is no 128 KB alias.** The console places 24.6 KB of buffers in SRAM2 (`console_mem.h`: two cell grids and the scrollback), leaving 8.1 KB; and `0x20018000` is not accessible on the STM32L475 (a read fails on the board) | Link of the full image; board test |
| F-11 | **The arena needs more than its peak.** At 48 128 bytes the TLS handshake fails (`RSA private key operation failed: BIGNUM memory allocation failed`, -0x4310) although the peak counter reads 45 364; at 52 992 it works with a peak of 46 280. The RSA private-key operation needs one contiguous block. The working size is in (48 128, 52 992], not bisected | Board |
| F-12 | **Cloud chain and NTP together are not stable** (cause not isolated): after connecting, `wifitask_send(34 B) failed, err=6` coincides with an NTP sync; the three subscribes fail (`failed (8)`); later `wifitask_send(4039 B) failed, err=7` (a send larger than the WiFi driver's 1536-byte AT buffer reached it) and `Connection lost` | Board, `Debug/bench/logs/stack-A3.log` |
| F-13 | **The WiFi module cannot open a socket to a host name.** `wifitask_open_socket()` with `a29hfe9n68rs9a-ats.iot.eu-north-1.amazonaws.com` returns `WIFI_ERR_SOCKET` (6) in under a second, before any TLS traffic; the same call with the resolved address `13.60.114.42` works. Nothing in the firmware resolves names, and AWS addresses change | Board, MqttClient harness against AWS IoT (measurement 3). Cause found: `P3` does not resolve names, but the module has a resolver, `D0=<host name>` (70-110 ms on the board). `wifi_resolve()` is in PR #120; a failing name must be given a 60 s wait, or the module stays silent for good |
| F-14 | **A TLS handshake message must fit the input record buffer, with or without maximum fragment length.** Mosquitto honours RFC 6066 max_fragment_length (the ServerHello echoes it and the 5.6 KB Certificate message arrives in 2048-byte records), but mbedTLS 3.6.7 reassembles a fragmented handshake message inside the input buffer, so the handshake still failed at an input size of 2048. Also `mbedtls_ssl_conf_max_frag_len()` fails (BAD_INPUT_DATA) for a size above the smaller of the input and output buffers; the bench call ignored the result at first | Board against Mosquitto with a 5.2 KB server certificate; the AWS chain is 3 847 B (measurement 3) |

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
| `ModbusPollerTask` | BootTask, entry `lifecycle_wiring_poller_task` | **after `lifecycle_controller_init()`** (F-9; was "before the scheduler" in version 0.1) | wiring header |
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
    xTaskCreateStatic: ConsoleTask                   (ModbusPollerTask is NOT here: see F-9)
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
    xTaskCreateStatic: ModbusPollerTask              (gated; the start gate exists only now, F-9)
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


### Decisions taken (Pass H, 2026-10-29)

| ID | Decision | Consequence |
|---|---|---|
| D-1 | ~~**Measure first.** No RAM option is chosen yet~~ **Decided 2026-10-30: option F** (record buffers 5 120 in / 2 048 out, arena 28 KB, stacks at 1.5x use) | TLS part: PR #122 (hardware-checked against AWS IoT). Stacks: per-module constants, done in WP4 session 2 (ModbusPoller 768 and log_drain 384 words first). Static RAM 127 518 of 131 072 after both |
| D-2 | `update_service_get_instance()`: the accessor returns the static instance before create; CloudPublisher stores the pointer | Small change in UpdateService and in the CloudPublisher wiring; own PR before `main()` |
| D-3 | BootTask joins the access point with a bounded retry (3 attempts) before creating the MQTT client; a later drop is a follow-up work package | Bench: the first join after reset fails almost every time, so the retry is mandatory |
| D-6 | BootTask becomes LifecycleTask (calls `lifecycle_task_body()` and never returns) | One boot task, one stack |
| D-7 | **Name resolution (GM-O14): BootTask resolves the endpoint with `wifi_resolve()` right after the join (D-3) and gives MqttClient the address as `broker_endpoint`, with the endpoint name as `tls_server_name`** (decided 2026-10-30) | Driver: PR #120. MqttClient field `tls_server_name` (MQTT-D18): PR #121. A failed lookup is one more BootTask retry step (the lookup itself takes about 10 s to fail) |
| D-4, D-5 | **Not yet asked.** Proposed defaults: D-4 call `time_service_init(ntp)` in Phase B before `cloud_publisher_create` (idempotent, so the Lifecycle adapter's later call is a no-op); D-5 correct the HLD table to the code values in a separate docs PR | Stand unless changed |

**Measurement 1 done 2026-10-29 (TC-HW-MQTT-014, PR #118):** the mbedTLS arena peak with a TLS 1.2 mutual-auth
session open is **46 280 bytes** (RSA-2048 chain; the ECDSA verify added 0; heap grew 0). The provisional 44 KB
arena (45 056) is **1 224 bytes too small**: the handshake fails on the board. Effect on section 6: the deficit
becomes at least **18.1 KB** with the arena at the bare minimum (46 280 + 0 margin) and about **31 KB** with the
25 % margin (58 KB). Of the 46.3 KB peak, the two 16 KB SSL record buffers are about 33 KB, so option B
(smaller record buffers) is the biggest single lever; it is a design decision (a peer may send records up to
16 KB unless a maximum fragment length is negotiated) and is not made here. Not measured then: the peak for an
ECDSA server chain (measurement 3 below).

**Measurement 2 done 2026-10-30 (stack high-water marks, PR #119).** The real task set ran together on the board
(CloudPublisher in TLS, NTP, telemetry and health publishing, INFO logging, no Field Device). The all-time
minimum free stack gives the maximum use:

| Task | Declared (words) | Max used | % | Sized at 1.5x use (64-word steps) |
|---|---|---|---|---|
| ModbusPoller | 512 | **507** | **99 %** | 768 |
| log_drain | 256 | **218** | **85 %** | 384 |
| WifiTask | 256 | 175 | 68 % | 320 |
| CloudPublisher | 768 | 484 | 63 % | 768 |
| Console | 768 | 438 | 57 % | 704 |
| TimeService | 768 | 337 | 44 % | 512 |
| SensorTask | 384 | 170 | 44 % | 256 |
| UpdateService | 1024 | 438 (verify path, from the UpdateService bench; idle here 36) | 43 % | 704 |
| Lifecycle / BootTask | 2048 | 566 (all of Phase B; Restarting, UpdatingFirmware and EditingConfig not driven) | 28 % | 896 |
| Timer service | 256 | 48 | 19 % | 128 |
| Idle | 256 | 24 | 9 % | 128 |
| **Total** | **7 296 words (29.2 KB)** | | | **5 568 words (22.3 KB): saves 6.75 KB** |

Two stacks are already at their limit: ModbusPoller has 5 words free (measured with no Field Device attached,
so a real one may use more) and log_drain 38. Both must grow whatever else is decided.

**Corrected RAM picture (replaces the figures in section 6 and the premise of option A).** The full image links
with **150 652 bytes** of static data (147 KB, console included, 44 KB arena), against 98 304 (main bank) plus
32 768 (SRAM2) = 131 072 bytes. With the arena at the 52 992 that works (+7.9 KB) and the stacks trimmed
(-6.9 KB) it is about **149.6 KB: roughly 20 KB too much**. The 19.9 KB of pools proposed for SRAM2 in option A
do not fit: only 8.1 KB of SRAM2 is free while the console is linked. Stacks cannot move there either. The
only single measure that is large enough is the size of the SSL record buffers (option B: they are about 33 KB of
the peak; an estimate, not measured, is that an 8 KB input and 4 KB output buffer would free about 20 KB). Other
candidates: the console scrollback `s_lines` (13 000 bytes), the StoreAndForward pool (8 184), the MqttClient
pool (6 312).

**Measurement 3 done 2026-10-30 (TLS memory levers, MqttClient harness, PR #118/#119 bench script).** The arena
peak was re-measured on the board with the record buffers shrunk (`MBEDTLS_SSL_IN_CONTENT_LEN` /
`MBEDTLS_SSL_OUT_CONTENT_LEN`, build-time only, no source change) and with an ECDSA instead of an RSA chain:

| Chain | Server | Input / output buffer | Arena peak |
|---|---|---|---|
| RSA-2048 | Mosquitto | 16 384 / 16 384 (current) | 46 280 |
| RSA-2048 | Mosquitto | 4 096 / 2 048 | 19 656 |
| ECDSA P-256 | Mosquitto | 16 384 / 16 384 | 39 452 |
| ECDSA P-256 | Mosquitto | 4 096 / 2 048 | 12 828 |
| ECDSA P-256, max fragment length 2048 | Mosquitto | 2 048 / 2 048 | 10 784 |
| **RSA-2048 client certificate, real server chain** | **AWS IoT Core (eu-north-1)** | **5 120 / 2 048** | **20 920** |

The last row is the product configuration: TLS 1.2 mutual authentication against the real endpoint, full
certificate verification, MQTT CONNACK, QoS 0 and QoS 1 publish and a subscription all worked. The buffer
shrink saves exactly 26 624 bytes; an ECDSA client certificate would save about 6.8 KB more. AWS IoT's server
chain is RSA (three certificates, 1 535 + 1 122 + 1 174 bytes), so ECDSA only helps on the client side. The
server's Certificate handshake message is **3 847 bytes**, which sets the floor of the input buffer (F-14); 5 120
leaves 1 273 bytes for a longer chain after a rotation. The record size of application data is not bounded by
that: a peer may send a record up to 16 384 bytes unless max fragment length is negotiated, and AWS IoT's
support for the extension is **not tested**. The measurements used the 60 KB bench arena; the peak is what counts
(F-11 says the working size must also leave room for the contiguous RSA block, so the arena is not set to the
peak).

Effect on the RAM budget (bytes, from the 150 652 of the full image): arena 45 056 → 28 672 (peak 20 920 plus
a 37 % margin), stacks at 1.5x use (-6 750): **150 652 − 45 056 + 28 672 − 6 750 = 127 518 against 131 072,
3 554 bytes spare.** That is thin (2.7 %): a 32 KB arena already goes over by 542 bytes, so the stack trims and
the arena size are one decision. Further room, if wanted: ECDSA client certificate (-6.8 KB of peak), the console
scrollback (13 000), the StoreAndForward pool (8 184).

**Measurements that gate D-1** (all three are now done, see above):

1. **TC-HW-MQTT-014**, the mbedTLS arena peak with a TLS session open (also closes FS-O4). **Done: 46 280 bytes, the arena must grow, not shrink, unless the record buffers shrink (option B).**
2. **Task stack high-water marks**, from a first image that creates the real task set. **Done: the table above.** The sum in section 6 uses declared sizes; the real use of LifecycleTask (8 KB declared) is the largest unknown (option C).

Until both are in, option A (SRAM2 for 19.9 KB of pools) remains the only measure that is known to cover the
lower bound, so it is the fallback if the measurements do not free enough.

### D-1 RAM (F-3)

| Option | What | Cost / risk |
|---|---|---|
| ~~**A (recommended first step)**~~ **Superseded by F-10: not feasible while the console is linked (8.1 KB of SRAM2 free, not 32 KB).** Original text: | Move the large blocks that nothing needs in the fast bank to SRAM2 with `__attribute__((section(".bss.sram2")))` (the section exists): StoreAndForward pool 8 184, MqttClient pool 6 312, CloudPublisher pool 2 872, logger queue 2 496 = **19 864 B**, and optionally task stacks | Covers the 16.9 KB lower bound with about 3 KB to spare: thin. Needs a check that no DMA or ISR path reads them from a location SRAM2 does not allow (not verified) |
| B | Shrink the TLS buffers (smaller record size, needs broker support) and re-measure the arena | **Measured (measurement 3): the peak against AWS IoT falls from 46 280 to 20 920 with input 5 120 / output 2 048.** Risk: a peer record larger than the input buffer ends the connection; a handshake message larger than it (AWS: 3 847 B) cannot be fragmented around it (F-14). AWS support for max fragment length not tested |
| C | Trim stacks after the high-water marks are logged | Cannot be decided before the first run; LifecycleTask 8 KB is the biggest single item |
| D | A + C, decided in two steps: A now, C after measurement | Superseded: A is not feasible (F-10) |
| **F (proposed)** | **B + C:** input buffer 5 120, output 2 048, arena about 28 KB (set after a bisection, F-11), stacks at 1.5x use (ModbusPoller 768 and log_drain 384 whatever is decided) | Fits with 3 554 bytes spare (measurement 3). Needs: a record-size check against AWS (traffic larger than 5 KB per record, or max fragment length), the arena bisection, and a rerun of the stack marks with a real Field Device. **My recommendation** |

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
| GM-O9 | ModbusPollerTask creation moves to BootTask after `lifecycle_controller_init()` (F-9). The same applies to any task that waits on the start gate |
| GM-O10 | ModbusPoller (99 %) and log_drain (85 %) stacks are at their limit; the poller was measured without a Field Device. Resize per the table in section 8 |
| GM-O11 | The arena size: **closed 2026-10-30.** Bisected on the board against AWS IoT: 22 528 and 23 552 fail, 24 576 works, peak 20 920; 28 672 chosen (PR #122, MQTT-D19) |
| GM-O12 | Cloud chain with NTP running: send/subscribe failures and a 4039-byte send reaching the WiFi driver (F-12). Needs its own investigation before WP5 |
| GM-O13 | The whole product does not fit the chip's 128 KB of SRAM: **decided, D-1 option F.** The TLS part is PR #122; with the stack trims of session 2 the image is 127 518 bytes against 131 072 (3 554 spare) |
| GM-O14 | Name resolution (F-13): **decided, D-7**. Driver half `wifi_resolve` in PR #120, MqttClient `tls_server_name` in PR #121; the BootTask call is part of session 2. A failing lookup takes about 10 s and the call may wait up to 60 s |
| GM-O15 | AWS IoT and max fragment length / the largest record it sends: **not needed.** `MQTT_PKT_BUF_SIZE` is 4 096 and the OTA chunk is 1 024, so no inbound record needs more than the 5 120 input buffer unless two large packets share a record. Reopen if a measured record exceeds it |
| GM-O16 | The bench-only hooks used for measurement 3 (`MQTT_BENCH_MAX_FRAG_LEN`, `MQTT_BENCH_TLS_HOSTNAME`, `MQTT_BENCH_VERIFY_OPTIONAL`) are not committed. The hostname one is superseded by `tls_server_name` (PR #121); the other two are bench-only |
