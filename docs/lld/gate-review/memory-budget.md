# LLD Gate Review — Memory Budget
## Layer 3 Synthesis — Check 2

**Date:** 2026-05-21
**Branch:** `feature/lld-layer-3-synthesis`

All figures are conservative (rounded up). Sources: `task-breakdown.md` §4–§5 (stack sizes); companion §3 private structs; open items MQTT-O1 and GL-O1.

---

## 1. Gateway (STM32L475) — 128 KB internal SRAM

LCD framebuffer: N/A (Gateway has no display).

### 1.1 Task stacks

| Task | Priority | Stack |
|------|----------|-------|
| `SensorTask` | 5 | 2 KB |
| `ModbusPollerTask` | 4 | 2 KB |
| `WifiTask` | 3 | 1 KB |
| `CloudPublisherTask` | 2 | **8 KB** ← TLS handshake worst case (see §3) |
| `TimeServiceTask` | 2 | 3 KB |
| `UpdateServiceTask` | 1 | 4 KB |
| `ConsoleTask` | 1 | 2 KB |
| `LifecycleTask` | 1 | 1 KB |
| Idle + Timer tasks | 0 / 7 | 1 KB |
| **Total stacks** | | **24 KB** |

### 1.2 Static data

| Allocation | Bytes | Source |
|-----------|-------|--------|
| FreeRTOS TCBs (10 tasks × ~180 B) | 1 800 | ARM Cortex-M4 `StaticTask_t` |
| FreeRTOS IPC objects (queues, mutexes, timers, event groups) | 1 536 | `task-breakdown.md` §5.4 |
| MqttClient — mbedTLS all-in [MQTT-O1 **nominal** minimal config] | 35 840 | MQTT-O1 lower bound |
| MqttClient — `MQTT_PKT_BUF_SIZE` (MQTT-O3 provisional) | 4 096 | `mqtt-client.md` §3 |
| MqttClient — coreMQTT context + state | 512 | `mqtt-client.md` §3 |
| CloudPublisher — JSON scratch buf + command queue | 6 144 | `cloud-publisher-lld.md` §3 |
| WifiDriver — AT command buffer + socket table | 2 048 | `wifi-driver.md` §3 |
| StoreAndForward — ring-state metadata | 256 | `store-and-forward-lld.md` §3 |
| CircularFlashLog — head/tail/sector metadata | 256 | `circular-flash-log.md` §3 |
| ConfigStore — key-value RAM cache | 1 024 | `config-store.md` §3 |
| Logger — format buffer (`s_buf`) | 256 | `logger.md` §3 |
| ModbusMaster + ModbusPoller — PDU and frame buffers | 1 024 | `modbus-master-poller.md` §3 |
| NtpClient — UDP packet buffer | 512 | `ntp-client.md` §3 |
| TimeProvider — BKP0R shadow + state | 128 | `time-provider.md` §3 |
| SensorService + AlarmService — state | 384 | companions §3 |
| UpdateService + FirmwareStore — state | 512 | companions §3 |
| ConsoleService — UART RX line buffer | 512 | `console-service-lld.md` §3 |
| HealthMonitor — snapshot + mutex | 512 | `health-monitor.md` §3 |
| LifecycleController — state machine | 256 | `lifecycle-controller.md` §3 |
| DeviceProfileRegistry — profile cache | 512 | `device-profile-registry-lld.md` §3 |
| BSS / startup code / ISR stacks | 2 048 | linker estimate |
| **Total static** | **60 232** | |

### 1.3 Budget summary — Gateway

| Item | Bytes | KB |
|------|-------|----|
| Task stacks | 24 576 | 24.0 |
| Static data (nominal, MQTT-O1 minimal config) | 60 232 | 58.8 |
| **Nominal total** | **84 808** | **82.8** |
| Internal SRAM (REQ-NF-400) | 131 072 | 128.0 |
| **Headroom (nominal)** | **46 264** | **45.2** |
| **Utilisation (nominal)** | | **64.7%** ← OK |

**Worst-case (standard mbedTLS defaults, 16 KB × 2 record buffers):**

| Item | Bytes | KB |
|------|-------|----|
| Nominal total | 84 808 | 82.8 |
| Extra: standard record buffers vs minimal (68 KB all-in − 35 KB) | 33 792 | 33.0 |
| **Worst-case total** | **118 600** | **115.8** |
| **Utilisation (worst-case)** | | **90.4% → FIX_NOW** |

**→ FIX_NOW: MQTT-O1 must be resolved before Phase 4 integration.**
The minimal-config mbedTLS plan keeps the budget at 65%. If standard mbedTLS defaults are
used without reduction, the budget reaches 90%, which exceeds the 80% FIX_NOW threshold.

---

## 2. Field Device (STM32F469) — 320 KB internal SRAM

LCD framebuffer (800 × 480 × 2 bytes = 768 KB) lives in **external SDRAM** — excluded from this budget. LVGL draw buffers and heap are in internal SRAM.

### 2.1 Task stacks

| Task | Priority | Stack |
|------|----------|-------|
| `SensorTask` | 5 | 2 KB |
| `ModbusSlaveTask` | 4 | 2 KB |
| `LcdUiTask` | 2 | 4 KB |
| `ConsoleTask` | 1 | 2 KB |
| `LifecycleTask` | 1 | 1 KB |
| Idle + Timer tasks | 0 / 7 | 1 KB |
| **Total stacks** | | **12 KB** |

### 2.2 Static data

| Allocation | Bytes | Source |
|-----------|-------|--------|
| FreeRTOS TCBs (7 tasks × ~180 B) | 1 260 | ARM Cortex-M4 `StaticTask_t` |
| FreeRTOS IPC objects | 768 | `task-breakdown.md` §4.4 |
| LVGL draw buffers (2 × 8 KB, Option A in SRAM) | 16 384 | `graphics-library.md` §3.6 |
| LVGL internal heap (`LV_MEM_SIZE` ≥ 32 KB) | 32 768 | `graphics-library.md` §3 |
| GraphicsLibrary + LcdUi — state structs | 512 | companions §3 |
| LcdDriver + SdramDriver + TouchscreenDriver — state | 384 | companions §3 |
| ConfigStore — key-value RAM cache | 1 024 | `config-store.md` §3 |
| Logger — format buffer (`s_buf`) | 256 | `logger.md` §3 |
| ModbusSlave + ModbusRegisterMap — PDU + register map | 1 024 | companions §3 |
| SensorService + AlarmService — state | 384 | companions §3 |
| ConsoleService — UART RX line buffer | 512 | `console-service-lld.md` §3 |
| HealthMonitor — snapshot + mutex | 512 | `health-monitor.md` §3 |
| LifecycleController — state machine | 256 | `lifecycle-controller.md` §3 |
| DeviceProfileRegistry — profile cache | 512 | `device-profile-registry-lld.md` §3 |
| BSS / startup code / ISR stacks | 2 048 | linker estimate |
| **Total static** | **58 104** | |

### 2.3 Budget summary — Field Device

| Item | Bytes | KB |
|------|-------|----|
| Task stacks | 12 288 | 12.0 |
| Static data | 58 104 | 56.7 |
| **Nominal total** | **70 392** | **68.7** |
| Internal SRAM | 327 680 | 320.0 |
| **Headroom** | **257 288** | **251.3** |
| **Utilisation** | | **21.5% ← OK** |

No threshold concerns. The dominant allocation is the LVGL internal heap (32 KB) and draw buffers (16 KB). If RAM becomes constrained at integration, LVGL's Option B draw strategy (SDRAM framebuffer as draw buffer) eliminates the 16 KB draw buffers from internal SRAM; the 32 KB `LV_MEM_SIZE` is a LVGL configuration constant and can be tuned.

---

## 3. Notes

### 3.1 CloudPublisherTask 8 KB stack

The 8 KB stack for `CloudPublisherTask` is the largest single task allocation. It covers the worst-case TLS handshake stack frame: certificate chain parsing, X.509 validation, ECDH/RSA operations, and PRNG consumption during the mbedTLS handshake. Runtime measurement via `uxTaskGetStackHighWaterMark()` in Phase 4 will likely allow a reduction to 4–6 KB once the actual peak is known.

### 3.2 MQTT-O1 open item

`mqtt-client.md` open item **MQTT-O1**: mbedTLS SRAM footprint must be verified against REQ-NF-400 (128 KB GW internal SRAM) during integration. The companion specifies minimal-config operation (~35 KB all-in for SSL context + reduced cipher suite + small record buffers). If standard library defaults are used without configuration reduction (SSL context ~36 KB + 16 KB × 2 record buffers = ~68 KB), the GW budget reaches 90.4% — above the 80% FIX_NOW threshold.

**Resolution mechanism:** Phase 4 integration — configure `MBEDTLS_SSL_MAX_CONTENT_LEN` and cipher suite selection in `mbedtls_config.h`; verify via `uxTaskGetStackHighWaterMark()` and `configTOTAL_HEAP_SIZE` statistics.

### 3.3 External SDRAM (Field Device)

The STM32F469 discovery board has 16 MB external SDRAM connected via FMC. The LCD framebuffer (768 KB) is allocated there by `SdramDriver` / `LcdDriver`. This budget is separate from the 320 KB internal SRAM tracked above and is not constrained — 768 KB uses less than 5% of the 16 MB SDRAM.
