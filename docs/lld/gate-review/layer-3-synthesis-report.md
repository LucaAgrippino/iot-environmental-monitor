# LLD Gate Review — Layer 3 Synthesis Report

**Date:** 2026-05-21
**Branch:** `feature/lld-layer-3-synthesis`
**Script:** `scripts/lld_gate_review_check.py` — `check_layer_3_synthesis()`
**Companions reviewed:** 39 (17 drivers, 10 middleware, 12 application)

---

## 1. Gate criterion (lld-methodology.md §5, Synthesis pass)

Three cross-companion checks that cannot be satisfied by reviewing any single companion in isolation:

1. **Boot-ordering verification** — the init-dependency graph must be directed and acyclic; no component may be initialised before a stated prerequisite.
2. **Memory budget** — sum of task stacks + FreeRTOS overhead + static buffers must be < 80% of internal SRAM per board (FIX_NOW if ≥ 80%; BLOCKER if ≥ 95%).
3. **Task-boundary consistency** — every component called from more than one task must declare a synchronisation primitive in §3; no component may be listed in two different tasks.

---

## 2. Pre-remediation findings

| Category | Count |
|---|---|
| BLOCKER — task-boundary: shared component missing mutex declaration | 2 |
| FIX_NOW — memory budget: GW worst-case > 80% | 1 |
| **Total BLOCKERs** | **2** |
| **Total FIX_NOWs** | **1** |

### 2.1 Task-boundary BLOCKERs (2)

The gate check searched for the mutex name (from `task-breakdown.md` §7) in each shared component's §3 Synchronisation section.

| Companion | Missing reference | Actual mutex declared? |
|---|---|---|
| `config-store.md` | `config_store_mutex` not mentioned | Yes — `s_cs.mutex` (internal name only) |
| `health-monitor.md` | `health_mutex` not mentioned | Yes — `s_hm.mutex` (internal name only) |

Both components correctly declared and implemented internal mutexes; the gap was that §3 did not cross-reference the name used in `task-breakdown.md` §7, making the cross-check impossible to verify from companions alone.

### 2.2 Memory budget FIX_NOW (1)

GW (STM32L475, 128 KB internal SRAM):

| Scenario | Total | % of 128 KB |
|---|---|---|
| Nominal (mbedTLS minimal config, MQTT-O1 best estimate) | 84.8 KB | 64.7% |
| Worst-case (standard mbedTLS defaults: 16 KB × 2 record bufs) | 115.8 KB | **90.4%** |

The nominal budget is well within limits. However, MQTT-O1 is unresolved: if mbedTLS is used at standard defaults (without `MBEDTLS_SSL_MAX_CONTENT_LEN` reduction), the GW budget would reach 90% — above the 80% FIX_NOW threshold but below the 95% BLOCKER halt-and-report threshold.

FD (STM32F469, 320 KB internal SRAM): 68.7 KB = 21.5% — no finding.

---

## 3. Remediation

### 3.1 Task-boundary BLOCKERs — companion cross-reference additions

Added the `task-breakdown.md` §7 mutex name to the §3 Synchronisation prose in each companion:

- `config-store.md` §3: "This component uses an internal mutex (`config_store_mutex` per task-breakdown.md §7) …"
- `health-monitor.md` §3: "This component uses an internal mutex (`health_mutex` per task-breakdown.md §7) …"

No design change. Both components already correctly implemented their mutexes; the fix adds the cross-reference that makes the task-breakdown ↔ companion traceability explicit.

### 3.2 Memory FIX_NOW — MQTT-O1 (deferred to Phase 4)

No remediation at LLD phase. The fix is a Phase 4 integration-time action: configure `MBEDTLS_SSL_MAX_CONTENT_LEN` and the cipher suite whitelist in `mbedtls_config.h` to achieve the 35 KB all-in mbedTLS footprint stated in the companion. Resolution trigger: Phase 4, when the mbedTLS port is instantiated. Verification: `uxTaskGetStackHighWaterMark()` on `CloudPublisherTask` + mbedTLS memory accounting via `mbedtls_memory_buffer_alloc_status()`.

Supporting artefacts produced:
- `docs/lld/gate-review/boot-order-graph.md` — directed init-dependency graph (mermaid), topological sort, task-component map.
- `docs/lld/gate-review/memory-budget.md` — per-board budget tables with line items, totals, headroom, and MQTT-O1 worst-case.

---

## 4. Post-remediation verification

```
python scripts/lld_gate_review_check.py

LLD gate review — Layer 1 + Layer 2 (Passes B, C, D, E, F, G) + Layer 3 Synthesis
Summary: 1 findings (0 blockers)
Layer 1 PASSES — no blockers found.
```

Remaining finding: 1 FIX_NOW (GW memory worst-case) — accepted per synthesis criterion:
> "FIX_NOW warnings on memory budget are acceptable if Phase 4 measurement is the resolution mechanism."

All prior Layer 1 and Pass B/C/D/E/F/G findings remain at 0. No regressions.

---

## 5. Boot-order verification detail

### 5.1 Acyclicity

`_topo_sort_init_deps()` (Kahn's algorithm) processed 33 nodes and 40 directed edges with no cycle. One valid topological ordering:

```
GpioDriver → RtcDriver, SimulatedSensorDrivers →
ExtiDriver, SpiDriver, I2cDriver, UartDriver, QspiFlashDriver, SdramDriver →
Logger →
CircularFlashLog, ConfigStore, FirmwareStore →
ConfigService →
SensorDrivers, LcdDriver, ModbusMaster, ModbusSlave, WifiDriver →
SensorService, AlarmService, GraphicsLibrary, TouchscreenDriver,
ModbusRegisterMap, ModbusPoller, MqttClient, NtpClient, TimeProvider →
LcdUi, StoreAndForward →
CloudPublisher, TimeService, UpdateService →
ConsoleService, HealthMonitor →
LifecycleController, DeviceProfileRegistry
```

### 5.2 Bootstrap exception verified

Logger must log timestamps before `TimeProvider` is initialised (TimeProvider depends on NTP, which requires the scheduler). The bootstrap exception (Logger reads RtcDriver directly, bypassing TimeProvider) breaks the potential circular dependency:

- Without exception: Logger → TimeProvider → NtpClient → WifiDriver (post-scheduler) — Logger could not be pre-scheduler-safe.
- With exception: Logger → RtcDriver (pre-scheduler safe). TimeProvider → RtcDriver independently. No cycle.

LLD-D15 (Logger bootstrap exception) and LLD-D16 (TimeProvider BKP0R after RtcDriver init) are both satisfied by the graph structure.

### 5.3 PH7 reset ordering (FD)

`TouchscreenDriver` depends on `LcdDriver` (TSD-D5): `lcd_init()` deasserts PH7, releasing the FT6206 reset. The graph edge `LcdDriver → TouchscreenDriver` captures this constraint. Verified: `touchscreen_init()` is called in LifecycleTask sub-state 4, after `lcd_init()`.

---

## 6. Memory budget summary

| Board | Nominal | % SRAM | Worst-case | % SRAM | Status |
|-------|---------|--------|-----------|--------|--------|
| GW (128 KB) | 84.8 KB | 64.7% | 115.8 KB | 90.4% | FIX_NOW (worst-case; MQTT-O1 open) |
| FD (320 KB) | 68.7 KB | 21.5% | — | — | PASS |

Notes:
- **CloudPublisherTask 8 KB stack** is the largest single allocation (TLS handshake worst case per `task-breakdown.md` §5.3). Phase 4 `uxTaskGetStackHighWaterMark` measurement will refine.
- **FD LVGL heap + draw buffers** (48 KB combined) dominate the FD static budget. Relocating draw buffers to SDRAM (Option B, `graphics-library.md` §6) would recover 16 KB of internal SRAM if needed.
- **FD external SDRAM**: LCD framebuffer (768 KB) is in 16 MB external SDRAM — no internal SRAM impact.

---

## 7. Task-boundary consistency summary

| Resource | Guard | §3 cross-ref | Status |
|---|---|---|---|
| `Logger` | `logger_mutex` | ✓ `logger.md` §3 | PASS |
| `ConfigStore` | `config_store_mutex` | ✓ `config-store.md` §3 (added) | PASS |
| `HealthMonitor` | `health_mutex` | ✓ `health-monitor.md` §3 (added) | PASS |
| `WifiDriver` (GW) | None (D29: `WifiTask` is sole owner) | ✓ `task-breakdown.md` §7 | PASS |

---

## 8. Acceptance

| Criterion | Result |
|---|---|
| 0 BLOCKER from `check_layer_3_synthesis` | PASS |
| FIX_NOW ≤ 1 (memory budget, Phase 4 measurement resolution) | PASS |
| Boot-order graph produced and verified acyclic | PASS |
| Bootstrap exception correctly modelled (no false cycle) | PASS |
| Memory budget produced for both boards | PASS |
| GW nominal budget < 80% SRAM | PASS (64.7%) |
| FD nominal budget < 80% SRAM | PASS (21.5%) |
| Task-component map produced | PASS |
| All shared resources have declared sync primitives in §3 | PASS |
| All prior gate passes (Layer 1, B, C, D, E, F, G) still at 0 | PASS |

**Layer 3 Synthesis GATE PASSES — 0 BLOCKERs, 1 FIX_NOW (MQTT-O1, Phase 4 resolution).**

---

## 9. Escalations

None. No cycle in the boot-order graph, no board exceeds 95% SRAM, no missing USES edges, no unhandled error propagation. All halt-and-report conditions (prompt §Halt-and-report) remained clear.

---

## 10. Open items carried forward to Phase 4

| Item | Companion | Phase 4 action |
|---|---|---|
| MQTT-O1: mbedTLS RAM at minimal config | `mqtt-client.md` §8 | Configure `MBEDTLS_SSL_MAX_CONTENT_LEN`; verify with `mbedtls_memory_buffer_alloc_status()` |
| MQTT-O3: `MQTT_PKT_BUF_SIZE` max OTA chunk size | `mqtt-client.md` §8 | Confirm against `UpdateService` max chunk at integration |
| GL-O2: `lcd_driver_blit()` DMA-async vs blocking | `graphics-library.md` §8 | Confirm at `LcdDriver` implementation |
| TSD-O1: FT6206 IRQ pin MCU mapping | `touchscreen-driver.md` §8 | Check UM1932 schematic at implementation |
| LOG-O1: Logger mutex hold > 2 ms at 115200 baud | `logger.md` §8 | Increase baud to 921600 or reduce `s_buf` |
