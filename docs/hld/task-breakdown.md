# HLD Artefact #6 — FreeRTOS Task Breakdown

**Companion document to `hld.md`.** This artefact specifies the runtime
execution structure of both boards: which FreeRTOS tasks exist, which
components run inside each task, with what priority and stack size, and
how the tasks communicate.

---

## 1. Purpose and scope

The component view (`hld.md` §5–6) defines **who exists** — modules and
their responsibilities. The sequence diagrams (`hld.md` §10) define **who
calls whom** over time. Neither defines the runtime execution structure:
**which thread runs which code, at what priority, with what stack, and
through which IPC primitive.**

This document defines that structure. It is the bridge from the static
design to the running system, and the primary reference for the LLD phase
where per-task code is implemented.

---

## 2. Drawing conventions

The task interaction diagrams (one per board, in `docs/diagrams/`) use the
following conventions:

- **Task** — rounded rectangle with `«task»` stereotype, named in
  PascalCase ending in `Task`.
- **Hosted components** — listed inside the task rectangle. A passive
  component called from a task is not listed; its hosting task is the
  caller.
- **IPC connector stereotypes:**
  - `«notify»` — FreeRTOS direct-to-task notification (1:1 single event)
  - `«queue»` — FreeRTOS queue (producer → consumer, N items)
  - `«event-group»` — FreeRTOS event group (multi-bit synchronisation)
  - `«mutex»` — FreeRTOS mutex with priority inheritance enabled
  - `«semaphore»` — FreeRTOS counting semaphore (typically ISR → task). *Reserved; no counting-semaphore path exists in v1.0.*
- **ISR** — separate rectangle with `«ISR»` stereotype, located at the
  diagram edge. ISRs feed into tasks via `«notify»` or `«semaphore»`.
- **Colours** — blue for Field Device tasks, green for Gateway tasks
  (per `diagram-colour-palette.md`).

---

## 3. Engineering method

The task design is derived by a ten-step procedure applied consistently to
both boards.

### Step 1 — Catalogue active components

Enumerate every Application-layer and Middleware-layer component from
`components.md` for the board.

### Step 2 — Classify activation source

Each component is one of:

- **Periodic** — runs on a timer tick. Examples: sensor poll, Modbus
  poll, periodic publish.
- **Event-driven** — runs in response to an asynchronous event (ISR,
  message arrival, state change). Examples: UART RX, MQTT inbound,
  alarm signal.
- **Reactive** — runs when called by another task. Often a service
  invoked synchronously.
- **Passive** — has no thread of control; called by whatever task needs
  its services. Examples: `ConfigStore`, `Logger`, register encoders.

### Step 3 — Drop passive components

Passive components do not get their own task. They are listed in the
hosting task's "components used" line.

### Step 4 — Cluster by activation source

Components sharing an activation source **may** share a task. The Observer
pattern (e.g., `SensorService` notifying `AlarmService`) means the
subscriber runs in the producer's task context — the two are naturally
co-located.

### Step 5 — Apply the blocking-behaviour test

Split clusters where one execution path blocks on slow I/O (network,
flash, long Modbus transactions) and another has tight deadlines. A task
that blocks for 200 ms on UART cannot also be the one running a 100 ms
periodic poll.

### Step 6 — Assign priorities (rate-monotonic-ish)

Shorter deadline → higher priority. Each priority level is anchored to an
SRS NF requirement. In FreeRTOS, larger number = higher priority.

This project uses five application levels (1 to 5) plus the conventional
idle (0) and timer (configMAX_PRIORITIES-1 = 7) tasks.

### Step 7 — Estimate stack sizes

Worst-case call depth × average frame size + worst-case local arrays.
Sized conservatively at first; refined at runtime with
`uxTaskGetStackHighWaterMark()` after representative workload runs.

Each estimate is expressed in **words** (FreeRTOS convention; 1 word =
4 bytes on Cortex-M4).

### Step 8 — Choose IPC primitive per interaction

Preference order, lightest first:

1. **Direct task notification** — 1:1 single event, no kernel object
   allocation beyond the notification value embedded in the TCB.
2. **Queue** — when the consumer needs to buffer N items, or when the
   payload is too large for the 32-bit notification value.
3. **Event group** — when one task waits on multiple independent events.
4. **Counting semaphore** — typically ISR-to-task signalling where each
   event must be counted.
5. **Mutex** — shared-resource protection. Priority inheritance **on**
   by default in this project to prevent unbounded priority inversion.

### Step 9 — Map ISR → task contracts

ISRs do the minimum: clear the interrupt flag, capture data (DMA pointer,
register snapshot), notify the owning task via direct-to-task notification.
All non-trivial processing runs in task context where the kernel scheduler
applies.

### Step 10 — Sanity check

- 6–10 tasks per board is the healthy range.
- 3 or fewer suggests under-decomposition (no concurrency benefit; can't
  use priority).
- 15 or more suggests over-engineering (context-switch overhead exceeds
  the benefit of separation; mental model becomes hard to defend).

---

## 4. Field Device — task design

### 4.1 Component activation analysis

| Component | Layer | Activation | Notes |
|---|---|---|---|
| `LifecycleController` | Application | Event-driven | Top-level lifecycle state machine; events from boot, console, modbus |
| `SensorService` | Application | Periodic (100 ms) | REQ-SA-070 sampling rate |
| `AlarmService` | Application | Event-driven (Observer) | Subscribes to `SensorService` new-reading events; runs in producer task |
| `LcdUi` | Application | Periodic (200 ms) + touchscreen events | LVGL refresh + input |
| `ConsoleService` | Application | Event-driven | UART RX from debug UART |
| `ModbusRegisterMap` | Application | Event-driven | UART RX from RS-485 bus, frame timeout |
| `ConfigService` | Application | Reactive | Called by `ConsoleService`, `ModbusRegisterMap` |
| `HealthMonitor` | Application | Reactive | Called by producers via `IHealthReport` |
| `TimeProvider` | Middleware | Passive | Wraps RTC; called as needed |
| `ModbusSlave` | Middleware | Passive | Protocol stack invoked by `ModbusRegisterMap` |
| `GraphicsLibrary` | Middleware | Passive | LVGL wrapper, called from `LcdUi` |
| `Logger` | Middleware | Passive | Synchronous logging; short critical sections |
| `ConfigStore` | Middleware | Passive | Flash persistence; called by `ConfigService` |
| Drivers | Driver | Passive | All driver calls run in caller task context |

### 4.2 Task list

Five application tasks, plus the conventional idle and timer tasks.

| Task | Hosted components | Activation | Priority | Stack (words / bytes) | Anchors to |
|---|---|---|---|---|---|
| `SensorTask` | `SensorService`, `AlarmService` | Periodic (100 ms tick) | **5** | 512 / 2 KB | REQ-SA-070, REQ-NF-101 alarm latency, D30, D32 |
| `ModbusSlaveTask` | `ModbusRegisterMap`, `ModbusSlave` | Event (UART RX notify) | **4** | 512 / 2 KB | REQ-MB-050 (200 ms response), REQ-MB-040 (FC handling) |
| `LcdUiTask` | `LcdUi`, `GraphicsLibrary` callbacks | Periodic (200 ms) + event (touch) | **2** | 1024 / 4 KB | REQ-NF-108 (5 Hz display refresh), LVGL workload |
| `ConsoleTask` | `ConsoleService` | Event (UART RX notify) | **1** | 512 / 2 KB | REQ-LI-010 command latency |
| `LifecycleTask` | `LifecycleController` | Event (state-change queue) | **1** | 256 / 1 KB | UC-01 boot, UC-15 config edit |
| *(Idle hook)* | — | Always | 0 | (FreeRTOS default) | Built-in |
| *(Timer service)* | — | Highest | 7 | (FreeRTOS default) | Built-in |

### 4.3 Component-to-task mapping rationale

**Why `AlarmService` is hosted in `SensorTask`:** the Observer pattern runs
the subscriber callback in the producer's task. Splitting them would
require an extra queue and context switch per reading — overhead unjustified
by REQ-NF-101's per-polling-cycle budget.

**Why `LcdUi` is its own task:** LVGL has its own refresh cadence (200 ms)
and processes touchscreen input with latency requirements. Folding it
into `SensorTask` would couple display refresh to sensor period — bad
practice (P5 violation). Render cadence is 200 ms (5 Hz per REQ-NF-108).
Internal double-buffering decouples sensor data acquisition rate from
display refresh rate.

**Why `SensorTask` is priority 5 (above `ModbusSlaveTask`):** Sensor data
freshness is a harder real-time constraint than Modbus response throughput
(REQ-NF-101 requires alarm detection within one polling cycle). A Modbus
response at priority 4 must not block sensor acquisition at priority 3 for
an extended window. SensorTask raised to priority 5 to enforce this
ordering. Stack high-watermark (`uxTaskGetStackHighWaterMark`) must be
validated at integration.

**Why `ModbusSlaveTask` is priority 4:** REQ-MB-050 mandates response
within 200 ms. A slave that misses the timeout window forces the master
into retry (REQ-MB-060), wasting bus time. The slave must preempt
display refresh and console handling.

**Why `ConsoleTask` and `LifecycleTask` are equal lowest priority:** both
serve admin/lifecycle paths with no hard real-time constraint. Equal
priority + round-robin is acceptable; neither preempts the other.

### 4.4 IPC primitives — Field Device

| Producer → Consumer | Primitive | Rationale |
|---|---|---|
| `ModbusUartDriver` ISR → `ModbusSlaveTask` | `«notify»` | Single-event signal "frame ready"; 1:1 |
| 200 ms timer → `LcdUiTask` | `«notify»` (bit 0 — refresh tick) | Periodic tick; 1:1 |
| Touchscreen ISR → `LcdUiTask` | `«notify»` (bit 1 — touch event) | Distinct from refresh tick |
| `DSI_LTDC_IRQHandler` → `LcdUiTask` | `«notify»` (bit 2 — frame done) | Signals DMA flush complete; task rearms DMA for next frame. |
| 100 ms timer → `SensorTask` | `«notify»` | Periodic tick |
| `DebugUartDriver` ISR → `ConsoleTask` | `«notify»` | Single-event "line received" |
| Any task → `LifecycleTask` | `«queue»` (depth 4) | Multiple sources, decoupled |
| Any task → `ConfigService` | direct call (`ConfigService` is reactive) | No IPC needed; mutex guards internal state |

---

## 5. Gateway — task design

### 5.1 Component activation analysis

| Component | Layer | Activation | Notes |
|---|---|---|---|
| `LifecycleController` | Application | Event-driven | Boot, restart, OTA reboot events |
| `SensorService` | Application | Periodic (100 ms) | Onboard sensors |
| `AlarmService` | Application | Event-driven (Observer) | Subscribes to `SensorService` |
| `ModbusPoller` | Application | Periodic + event | Per-slave poll cycle, retry on timeout |
| `CloudPublisher` | Application | Event-driven | Publish triggers from telemetry, alarms, commands |
| `StoreAndForward` | Application | Reactive | Called by `CloudPublisher` |
| `TimeService` | Application | Periodic (hourly) + event (post-reconnect trigger) | NTP sync |
| `UpdateService` | Application | Event-driven | OTA command from cloud; rare |
| `ConsoleService` | Application | Event-driven | Debug UART RX |
| `ConfigService` | Application | Reactive | Called by remote-config and CLI paths |
| `DeviceProfileRegistry` | Application | Reactive | Called by `ModbusPoller`, `ConsoleService` |
| `HealthMonitor` | Application | Reactive | Called by producers |
| `MqttClient`, `NtpClient`, `ModbusMaster`, `CircularFlashLog`, `ConfigStore`, `TimeProvider`, `Logger` | Middleware | Passive | Called from hosting tasks |
| Drivers | Driver | Passive | All driver calls in caller task |

### 5.2 Task list

Eight application tasks, plus idle and timer.

| Task | Hosted components | Activation | Priority | Stack (words / bytes) | Anchors to |
|---|---|---|---|---|---|
| `SensorTask` | `SensorService`, `AlarmService` | Periodic (100 ms) | **5** | 512 / 2 KB | REQ-SA-070, REQ-NF-101, D30, D32 |
| `ModbusPollerTask` | `ModbusPoller`, `ModbusMaster` | Periodic + event (timeout, retry) | **4** | 512 / 2 KB | REQ-MB-050 (200 ms), REQ-NF-103/104 (link state) |
| `WifiTask` | `WifiDriver` | Event (ISR + API request) | **3** | 256 / 1 KB | D29; sole WiFi SPI owner |
| `CloudPublisherTask` | `CloudPublisher`, `MqttClient`, `StoreAndForward` calls | Event (publish queue) | **2** | 2048 / 8 KB | REQ-CC-* (MQTT publish), TLS handshake worst case |
| `TimeServiceTask` | `TimeService`, `NtpClient` | Periodic (hourly) + event (post-reconnect) | **2** | 768 / 3 KB | REQ-TS-* (NTP), D13 (re-sync trigger) |
| `UpdateServiceTask` | `UpdateService`, `FirmwareStore` | Event (OTA command) | **1** | 1024 / 4 KB | REQ-DM-050..-074 (firmware update) |
| `ConsoleTask` | `ConsoleService` | Event (UART RX) | **1** | 512 / 2 KB | REQ-LI-010 |
| `LifecycleTask` | `LifecycleController` | Event (queue) | **1** | 256 / 1 KB | UC-01, UC-17 restart, UC-18 OTA reboot |
| *(Idle hook)* | — | Always | 0 | default | Built-in |
| *(Timer service)* | — | Highest | 7 | default | Built-in |

### 5.3 Component-to-task mapping rationale

**Why `TimeServiceTask` and `CloudPublisherTask` are separate** (despite
both using the WiFi/socket stack): they have different activation
patterns (P5). NTP runs hourly; MQTT publishes continuously. Folding
them into one task would force either NTP to wait for MQTT activity or
MQTT to share its hot path with NTP. The shared resource — the WiFi
driver — is exclusively owned by `WifiTask`; both tasks route WiFi I/O
through its API (D29, §7).

**Why `UpdateServiceTask` is priority 1** despite being safety-critical:
OTA is rare and tolerant of seconds-level latency. While downloading, it
must not preempt sensor acquisition or Modbus polling. The criticality
applies to **correctness** (verified signature, rollback on self-check
failure), not to **scheduling priority**.

**Why `CloudPublisherTask` has 8 KB stack:** TLS handshake worst case can
need 4–8 KB depending on cipher suite and certificate chain. MQTT
payload assembly adds another KB. Conservative initial estimate; will
likely shrink once `uxTaskGetStackHighWaterMark` data is collected.

**Why `SensorTask` and `ModbusPollerTask` aren't merged:** both are
periodic but with different blocking profiles. `ModbusPollerTask` blocks
on UART for up to 200 ms per failed transaction (REQ-MB-050). If merged,
sensor reads would be delayed by Modbus retries — direct violation of
REQ-NF-101.

**Why `SensorTask` is priority 5 (above `ModbusPollerTask`):** Sensor data
freshness is a harder real-time constraint than Modbus retry throughput
(REQ-NF-101 requires alarm detection within one polling cycle). A Modbus
retry storm at priority 4 must not block sensor acquisition for up to
600 ms (five to six consecutive missed 100 ms cycles). SensorTask raised
to priority 5 to enforce this ordering. Stack high-watermark
(`uxTaskGetStackHighWaterMark`) must be validated at integration.

### 5.4 IPC primitives — Gateway

| Producer → Consumer | Primitive | Rationale |
|---|---|---|
| 100 ms timer → `SensorTask` | `«notify»` | Periodic tick |
| `ModbusUartDriver` ISR → `ModbusPollerTask` | `«notify»` (RX done, RX error) | Two distinct events on separate notification bits |
| Modbus poll timer → `ModbusPollerTask` | `«notify»` (separate bit) | Periodic slave poll |
| `SensorTask` (alarm) → `CloudPublisherTask` | `«queue»` (depth 8) | Alarm events; decoupled, may burst |
| `ModbusPollerTask` (event) → `CloudPublisherTask` | `«queue»` (shared, depth 8) | Field-device events; same queue as alarms. Queue item carries a type tag (e.g. `EventType { GATEWAY_ALARM, FD_EVENT }`) plus payload union; discriminator defined in LLD per §11. |
| FreeRTOS timer → `CloudPublisherTask` | `«notify»` (telemetry-tick bit) | Periodic publish trigger; timer callback calls `xTaskNotifyFromISR`. |
| `CloudPublisher` → `TimeServiceTask` | `«notify»` (resync-trigger bit) | CloudPublisher calls TimeService.requestResync() which posts the notification; no blocking. D13. |
| Cloud command (via `MqttClient`) → `UpdateServiceTask` | `«queue»` (depth 1) | OTA commands; rare, serialised |
| Cloud command → `LifecycleTask` (restart) | `«queue»` (shared, depth 4) | Lifecycle events from any source |
| `DebugUartDriver` ISR → `ConsoleTask` | `«notify»` | Single-event "line received" |
| NTP timer → `TimeServiceTask` | `«notify»` | Hourly tick |

---

## 6. ISR → task contracts (cross-board)

Every ISR follows the same contract:

1. **Acknowledge** the interrupt (clear flag).
2. **Capture** the minimum data the task will need (e.g., DMA pointer,
   timestamp).
3. **Notify** the owning task via `xTaskNotifyFromISR()` or
   `xQueueSendFromISR()` — with `pxHigherPriorityTaskWoken` propagated
   to `portYIELD_FROM_ISR()`.
4. **Return.**

No driver state machine logic runs in ISR context. No `printf`, no
`xQueueSend` (without `FromISR`), no mutex acquisition.

### 6.1 ISR inventory

| Board | ISR | Trigger | Owning task | Primitive |
|---|---|---|---|---|
| FD | `USART_modbus_IRQHandler` | RS-485 RX done / line idle | `ModbusSlaveTask` | `«notify»` |
| FD | `USART_debug_IRQHandler` | Debug UART RX | `ConsoleTask` | `«notify»` |
| FD | `EXTI_touch_IRQHandler` | Touchscreen IRQ pin | `LcdUiTask` | `«notify»` (separate bit) |
| FD | `DSI_LTDC_IRQHandler` | Frame-flush done | `LcdUiTask` | `«notify»` |
| GW | `USART_modbus_IRQHandler` | RS-485 RX done / line idle | `ModbusPollerTask` | `«notify»` |
| GW | `USART_debug_IRQHandler` | Debug UART RX | `ConsoleTask` | `«notify»` |
| GW | `SPI_wifi_IRQHandler` | WiFi data ready (cmd/data) | `WifiTask` | `«notify»` |
| Both | `SysTick_Handler` | 1 ms FreeRTOS tick | Kernel | (kernel internal) |

Note: `SPI_wifi_IRQHandler` signals `WifiTask` via direct-to-task notification
(`xTaskNotifyFromISR`). No other task accesses the WiFi SPI peripheral
directly — all WiFi I/O is routed through `WifiTask`'s API (D29).

---

## 7. Shared-resource locking strategy

Five shared resources are documented below; four require mutex protection.
**Priority inheritance is enabled on all mutexes** in this project to
prevent unbounded priority inversion.

| Resource | Holders | Mutex | Hold duration target |
|---|---|---|---|
| WiFi driver (Gateway only) | `WifiTask` (sole SPI owner) | — | No mutex needed; `WifiTask` is the only WiFi SPI accessor. `CloudPublisherTask`, `TimeServiceTask`, `UpdateServiceTask` route WiFi I/O through `WifiTask`'s API (D29). |
| `ConfigStore` flash region (Gateway) | `ConfigService`, `DeviceProfileRegistry` | `config_store_mutex` | < 20 ms; flash writes serialised |
| `ConfigStore` flash region (Field Device) | `ConfigService` (called from `ConsoleTask`, `ModbusSlaveTask`) | `config_store_mutex_fd` | < 20 ms |
| `HealthMonitor` snapshot (both boards) | All tasks (`IHealthReport` writers + `IHealthSnapshot` readers) | `health_mutex` | < 1 ms; single struct copy per critical section |
| `Logger` output stream (both boards) | All tasks | `logger_mutex` | < 2 ms; single line per critical section |

Note: the Modbus UART bus on the Gateway is touched only by
`ModbusPollerTask`. No mutex needed in practice; documenting the
convention is sufficient.

Note: the LCD framebuffer on the Field Device is touched only by
`LcdUiTask`. No mutex needed.

---

## 8. Schedulability check (informal)

A formal RMS analysis is deferred to LLD when the worst-case execution
times are measured. The informal check below validates that the task set
is reasonable.

### 8.1 Field Device

Total assumed worst-case execution time per second:

| Task | Period / occurrence | WCET assumed | Utilisation |
|---|---|---|---|
| `ModbusSlaveTask` | up to 100 frames/s | 1 ms/frame | 10% |
| `SensorTask` | 100 ms | 3 ms | 3% |
| `LcdUiTask` | 200 ms | 4 ms | 2% |
| `ConsoleTask` | bursts | < 1% | < 1% |
| `LifecycleTask` | events | < 0.1% | < 0.1% |
| **Total** | | | **~15%** |

Comfortably within bounds. Headroom for unexpected load.

### 8.2 Gateway

| Task | Period / occurrence | WCET assumed | Utilisation |
|---|---|---|---|
| `ModbusPollerTask` | 1 poll/s typical | 5 ms (one slave, success) up to 600 ms (timeout + 3 retries) | 0.5% nominal, peak 60% during retry storm |
| `SensorTask` | 100 ms | 3 ms | 3% |
| `CloudPublisherTask` | event-driven, ~1 publish/s | 20 ms typical, 500 ms during reconnect | 2% nominal |
| `TimeServiceTask` | hourly + event | 100 ms per sync | < 0.1% |
| `UpdateServiceTask` | rare | seconds during OTA | bursty, low priority |
| `ConsoleTask`, `LifecycleTask` | events | < 1% | < 1% |
| **Total** | | | **~10% nominal** |

During a retry storm (up to 600 ms), `SensorTask` (p5) preempts
`ModbusPollerTask` (p4) — sensor acquisition and alarm detection remain
REQ-NF-101 compliant. `CloudPublisherTask` (p2), `TimeServiceTask` (p2),
`WifiTask` (p3), and lower-priority tasks queue until the storm resolves;
publish latency may reach 600 ms but no real-time requirement binds those
paths.

---

## 9. Task interaction diagrams

Two diagrams to be drawn in Visual Paradigm and committed to
`docs/diagrams/`:

- `task-interaction-field-device.png` — five application tasks, hosted
  components inside each, ISRs at the diagram edge, IPC connectors
  labelled with stereotype.
- `task-interaction-gateway.png` — eight application tasks, same
  conventions.

Each diagram uses the colour palette from `diagram-colour-palette.md`:
blue for Field Device, green for Gateway.

The diagrams are companion views to the tables in §4 and §5. The tables
remain the authoritative source.

---

## 10. Architectural decisions made in this phase

| Decision | Rationale |
|---|---|
| **D21** — `AlarmService` runs in `SensorTask` (no separate task) | Observer pattern places subscriber in producer context; an extra queue + context switch per reading is unjustified by REQ-NF-101 |
| **D22** — `TimeService` and `CloudPublisher` in separate tasks despite shared WiFi resource | Different activation patterns (hourly vs continuous); P5 single-responsibility honoured; WiFi I/O routed through `WifiTask` (D29) |
| **D23** — `UpdateServiceTask` at priority 1 despite criticality of OTA | OTA is rare and seconds-tolerant; criticality is in correctness (signature verify, rollback), not in scheduling priority |
| **D24** — `LifecycleTask` as dedicated task rather than synchronous from caller | Cross-task lifecycle events (boot, restart, OTA reboot) processed in one context; eliminates need for an external mutex protecting the lifecycle state machine |
| **D25** — Direct-to-task notification preferred over queue for 1:1 single-event paths | Lighter than queue (no kernel object beyond TCB notification value); used wherever payload fits in 32 bits or no payload is needed |
| **D26** — Priority inheritance enabled on all mutexes | Prevents unbounded priority inversion; FreeRTOS-standard practice |
| **D27** — ISRs perform only acknowledge / capture / notify | Keeps interrupt latency bounded; all driver state machines run in task context |
| **D28** — Stack sizes initially estimated; refined via `uxTaskGetStackHighWaterMark` during integration | Conservative starting values minimise risk; runtime measurement gives the real numbers |
| **D29** — Dedicated `WifiTask` as sole owner of the WiFi SPI peripheral | `xTaskNotifyFromISR` requires a fixed compile-time task handle; the prior "or owning caller" contract is unimplementable. `WifiTask` (priority 3) hosts `WifiDriver` exclusively; `CloudPublisherTask`, `TimeServiceTask`, `UpdateServiceTask` route WiFi I/O through `WifiTask`'s API, eliminating the need for `wifi_mutex` |
| **D30** — `SensorTask` priority raised to 5 (above `ModbusPollerTask` at priority 4) on both boards | REQ-NF-101 requires alarm detection within one polling cycle. A 600 ms Modbus retry storm at priority 4 would preempt `SensorTask` at priority 3 for five to six consecutive 100 ms cycles, breaching REQ-NF-101 |
| **D31** — `LcdUiTask` render cadence set to 200 ms (5 Hz, matching REQ-NF-108) | REQ-NF-108 mandates a 5 Hz display refresh rate. Internal double-buffering decouples sensor data acquisition rate from display render cadence |
| **D32** — `SensorTask` default sample period 100 ms | REQ-NF-100 mandates a full polling cycle within 100 ms — that is a WCET deadline, not a mandated period. The 100 ms default provides a comfortable duty-cycle margin; REQ-SA-070 allows operator configuration of the actual rate |

---

## 11. LLD handoff

The LLD will refine this artefact into:

- **Concrete task entry-point functions** (`vModbusPollerTask`,
  `vSensorTask`, etc.) with their actual body code.
- **Measured WCETs** for each task path, replacing the assumed values in §8.
- **Refined stack sizes** based on `uxTaskGetStackHighWaterMark` data.
- **Per-IPC message types** (struct definitions for queue payloads).
- **Formal schedulability analysis** if any task approaches its deadline.

---

*This document is HLD Artefact #6. It is updated when task design changes.
The task tables in §4 and §5 are the authoritative reference; diagrams in
`docs/diagrams/` are companion views.*
