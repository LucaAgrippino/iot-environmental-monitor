# LLD Companion — ModbusMaster · ModbusPoller

**Layer:** ModbusMaster — Middleware · ModbusPoller — Application  
**Board:** Gateway (GW) only  
**Provides:**  
- `ModbusMaster` → `IModbusMaster`, `IModbusMasterStats`  
- `ModbusPoller` → `IModbusPoller`  
**Consumes:**  
- `ModbusMaster` → `IModbusUart` (ModbusUartDriver), `ILogger`  
- `ModbusPoller` → `IModbusMaster`, `IModbusMasterStats`, `IDeviceProfileProvider` (DeviceProfileRegistry), `IHealthReport`, `ILogger`  
**SRS traces:** REQ-MB-010, MB-020, MB-030, MB-040, MB-050, MB-060, MB-080, MB-090, MB-100, MB-110, MB-120, MB-130, MB-0E1; REQ-NF-103, NF-104, NF-105, NF-201, NF-215  
**HLD ref:** `components.md` §Middleware — ModbusMaster; §Application — ModbusPoller; `state-machines.md` Machine 4; `hld.md` §7.5; `sequence-diagrams.md` SD-02, SD-00b; `modbus-register-map.md` §2–§3
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** ModbusMaster + ModbusPoller in `components.md` (GW middleware layer)

---

## 1. Sources

ModbusMaster and ModbusPoller are separate components at different layers,
but they implement **one state machine together** (`state-machines.md`
Machine 4). ModbusMaster owns the protocol mechanics (frame encoding,
CRC, timeout, retry loop); ModbusPoller owns the scheduling, link-state
hysteresis, and routing. Documenting them separately would split the
state machine description across two files. The split point is still
made explicit in every section below.

---

## 3. Internal design

| Concern | Owner |
|---------|-------|
| Frame encoding / decoding | ModbusMaster |
| CRC calculation | ModbusMaster (reuses `modbus_crc16()` from `modbus_crc.h`) |
| 200 ms response timer | ModbusMaster |
| Retry loop (up to 3 retries) | ModbusMaster |
| Per-transaction stats counters | ModbusMaster (exposed via `IModbusMasterStats`) |
| Poll timer and scheduling | ModbusPoller |
| Per-slave link-state hysteresis | ModbusPoller |
| `node_offline` / `link_up` event emission | ModbusPoller |
| Command routing from other application components | ModbusPoller (`IModbusPoller`) |
| Polling DeviceProfileRegistry for slave list | ModbusPoller |
| Reporting Modbus metrics via `IHealthReport` | ModbusPoller |

ModbusMaster is a **reusable protocol library**. It knows nothing about
polling schedules, link state, or device profiles. ModbusPoller is the
**application-level orchestrator** that uses ModbusMaster as a transport.

---

### Principles applied

- **P1 (Strict directional layering).** ModbusMaster depends on IModbusUart (driver layer) and Logger; ModbusPoller depends on IModbusMaster, IDeviceProfileProvider, IHealthReport, and Logger — all at the same or lower layer.
- **P2 (Dependency Inversion).** Both components expose vtable interfaces; consumers depend on `IModbusMaster` / `IModbusPoller`, not the concrete modules.
- **P3 (Interface Segregation).** ModbusMaster splits into `IModbusMaster` (transaction execution) and `IModbusMasterStats` (statistics read-back) because ModbusPoller needs the former while HealthMonitor reads the latter; these are non-overlapping consumer sets — P3 applied.
- **P4 (Cross-cutting concern exception).** Logger referenced concretely per the cross-cutting exception; documented in §1 Sources.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static Modbus PDU buffer and stats counter struct; response timeout managed via FreeRTOS task delay; no heap.
- **P6 (Responsibility traces to requirements).** Request/response cycle traces to REQ-MB-010-130 Modbus master requirements.
- **P7 (Pull-based downstream consumption).** ModbusPoller polls ModbusMaster on its own task cadence; ModbusMaster does not push results unprompted.
- **P8 (Total error propagation, no silent failures).** All functions return typed `_err_t`; CRC mismatch, timeout, and exception responses are distinct error codes.
- **P9 (BARR-C coding standard).** Modbus function codes `uint8_t`; register addresses and counts `uint16_t`; no floating-point.
- **P10 (Naming conventions).** Prefixes `modbus_master_` / `modbus_poller_`; interfaces `IModbusMaster` -> `imodbus_master_t`; errors `MODBUS_MASTER_ERR_*` / `MODBUS_POLLER_ERR_*`.


## 3. Shared type — `modbus_frame_t`

```c
/* modbus_frame.h — shared by both components */

#define MODBUS_MAX_FRAME_BYTES  256U
#define MODBUS_MAX_DATA_BYTES   252U   /* 256 - addr(1) - FC(1) - CRC(2) */

typedef struct {
    uint8_t  slave_addr;
    uint8_t  function_code;
    uint8_t  data[MODBUS_MAX_DATA_BYTES];
    uint16_t data_len;
} modbus_frame_t;
```

---

## 2. Public API

### 4.1 Data types

```c
/* modbus_master.h */

typedef enum {
    MODBUS_MASTER_ERR_OK              = 0,
    MODBUS_MASTER_ERR_NOT_INIT        = 1,
    MODBUS_MASTER_ERR_NULL_ARG        = 2,
    MODBUS_MASTER_ERR_TIMEOUT         = 3,   /* no response after 3 retries */
    MODBUS_MASTER_ERR_CRC             = 4,   /* response CRC mismatch       */
    MODBUS_MASTER_ERR_BAD_RESPONSE    = 5,   /* addr / FC mismatch          */
    MODBUS_MASTER_ERR_EXCEPTION       = 6,   /* slave returned exception PDU */
} modbus_master_err_t;

typedef struct {
    uint32_t transactions_sent;      /* total requests transmitted          */
    uint32_t transactions_ok;        /* responses received and valid        */
    uint32_t timeouts;               /* per-attempt timeouts                */
    uint32_t crc_errors;             /* responses with bad CRC              */
    uint32_t exception_responses;    /* slave returned exception code       */
    uint32_t bad_responses;          /* addr/FC mismatch                    */
} modbus_master_stats_t;
```

### 4.2 `IModbusMaster` — provided interface

```c
/**
 * @brief  Initialise ModbusMaster.
 *
 * Must be called after modbus_uart_driver_init(). No task handle required —
 * ModbusMaster runs entirely in the calling task's context (ModbusPollerTask).
 *
 * @param  response_timeout_ms  Response timeout per attempt (REQ-MB-050: 200 ms).
 * @param  max_retries          Max retry count (REQ-MB-060: 3).
 * @return MODBUS_MASTER_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
modbus_master_err_t modbus_master_init(uint32_t response_timeout_ms,
                                       uint8_t  max_retries);

/**
 * @brief  Execute one Modbus transaction with integrated retry.
 *
 * Builds the request frame, transmits it, waits for a response with timeout,
 * retries up to max_retries times on timeout or CRC error, validates the
 * final response, and returns the decoded payload.
 *
 * Blocking — runs the full retry loop before returning. Called exclusively
 * from ModbusPollerTask.
 *
 * @param  request   Encoded request (slave_addr, FC, data, data_len).
 * @param  response  Filled on success with slave_addr, FC, and payload.
 * @return MODBUS_MASTER_ERR_OK or an error code.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
modbus_master_err_t modbus_master_transact(const modbus_frame_t *request,
                                           modbus_frame_t       *response);
```

**Why `modbus_master_transact()` handles retries internally:** from
ModbusPoller's perspective, a poll is a single logical operation. Exposing
retry-level control would leak protocol mechanics into application code.
The retry loop (`state-machines.md` T6: AwaitingResponse → Transmitting)
is an implementation detail of ModbusMaster.

### 4.3 `IModbusMasterStats` — provided interface

```c
/**
 * @brief  Copy current stats snapshot.
 *
 * Polled by ModbusPoller on each health-report cycle (Metric Producer
 * Pattern). Thread-safe: stats are incremented only in ModbusPollerTask
 * context, so no mutex is required — atomicity is guaranteed by the
 * single-task caller model.
 * @return MODBUS_MASTER_ERR_OK on success; non-zero error code on failure.
 */
modbus_master_err_t modbus_master_get_stats(modbus_master_stats_t *stats_out);

/** @brief  Reset all counters to zero (triggered by CMD_RESET_METRICS).
 * @return MODBUS_MASTER_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
modbus_master_err_t modbus_master_reset_stats(void);
```

### 4.4 State machine implementation (Machine 4 protocol path)

ModbusMaster implements the Transmitting → AwaitingResponse →
ProcessingResponse cycle as a blocking loop inside
`modbus_master_transact()`. The FreeRTOS task notification mechanism
replaces the conceptual "wait for event" in each state:

```
Transmitting:
  modbus_uart_tx(request_frame)        /* triggers tx_complete notification on finish */
  ulTaskNotifyTake(TX_COMPLETE_BIT)    /* wait for tx_complete */

AwaitingResponse:
  modbus_uart_rx_start()               /* arm the RX path */
  xTaskNotifyWait(RX_OR_TIMEOUT, ...)  /* wait for rx_complete or response_timer_expired */

ProcessingResponse:
  validate CRC, address, FC
  if ok  → decode payload → return OK
  if bad → stats++ → retry or return error
```

The 200 ms timer is a FreeRTOS software timer created at init, started on
entering AwaitingResponse, and stopped on the first of `rx_complete` or
expiry. On expiry, the timer callback calls `xTaskNotifyFromISR()` with
the `TIMEOUT_BIT`.

### 4.5 Response validation sequence

```
1. CRC check: modbus_crc16(rx_buf, rx_len - 2) == appended CRC bytes.
   Fail → stats.crc_errors++; retry (T6) or return MODBUS_MASTER_ERR_CRC (T7).

2. Slave address: response[0] == request[0].
   Fail → stats.bad_responses++; treat as failure (not retried — a foreign
   response is a bus error, not a timeout; return ERR_BAD_RESPONSE).

3. Function code check:
   a. response[1] == request[1]             → normal response; proceed.
   b. response[1] == (request[1] | 0x80)   → exception response.
      Decode exception code; stats.exception_responses++;
      log exception (slave_addr, FC, code); return MODBUS_MASTER_ERR_EXCEPTION.
   c. other                                 → stats.bad_responses++;
      return MODBUS_MASTER_ERR_BAD_RESPONSE.
```

Exception responses count toward the hysteresis failure counter in
ModbusPoller (same as timeout) — a slave that consistently returns
exceptions is effectively unreachable for functional purposes.

---

## 5. ModbusPoller

### 5.1 Data types

```c
/* modbus_poller.h */

typedef enum {
    MODBUS_POLLER_ERR_OK          = 0,
    MODBUS_POLLER_ERR_NOT_INIT    = 1,
    MODBUS_POLLER_ERR_NULL_ARG    = 2,
    MODBUS_POLLER_ERR_QUEUE_FULL  = 3,
    MODBUS_POLLER_ERR_REJECTED    = 4,   /* slave not in allowlist */
} modbus_poller_err_t;

/**
 * @brief  Command posted to ModbusPoller by other application components.
 *
 * TimeService uses this to write the time register (SD-09 UML note 3).
 * UpdateService uses this for OTA-related writes.
 */
typedef struct {
    uint8_t          slave_addr;
    uint8_t          function_code;   /* FC06 or FC16 */
    uint16_t         start_reg;
    uint16_t         reg_count;
    uint16_t         data[125];       /* FC16 max: 123 registers; FC06: 1 */
    QueueHandle_t    response_queue;  /* caller provides; receives result  */
} modbus_command_t;

typedef struct {
    modbus_master_err_t status;
    uint16_t            data[125];
    uint16_t            data_len;
} modbus_command_response_t;

/* Per-slave runtime state — one entry per device profile */
typedef struct {
    uint8_t  slave_addr;
    uint8_t  consecutive_successes;
    uint8_t  consecutive_failures;
    bool     link_online;
} modbus_slave_state_t;
```

### 5.2 `IModbusPoller` — provided interface

```c
/**
 * @brief  Initialise ModbusPoller.
 *
 * Creates the command queue. Must be called after modbus_master_init()
 * and after DeviceProfileRegistry is loaded.
 *
 * @param  poll_period_ms  Default poll period in ms (REQ-NF-110: 1000 ms).
 */
modbus_poller_err_t modbus_poller_init(uint32_t poll_period_ms);

/**
 * @brief  Post a command for ModbusPoller to execute on behalf of the caller.
 *
 * Non-blocking. Enqueues the command; ModbusPoller executes it between
 * regular polls. The caller blocks on response_queue until the result
 * arrives. The slave address must be in the DeviceProfileRegistry allowlist.
 *
 * Used by: TimeService (time-push FC06/16), UpdateService, ConsoleService.
 * P1 compliance: application → application (same layer); ModbusPoller is
 * the single Modbus bus gatekeeper.
 *
 * @param  cmd  Heap-free; caller owns; must remain valid until response arrives.
 * @return MODBUS_POLLER_ERR_OK, MODBUS_POLLER_ERR_QUEUE_FULL, or
 *         MODBUS_POLLER_ERR_REJECTED (slave not in allowlist).
 */
modbus_poller_err_t modbus_poller_send_command(modbus_command_t *cmd);

/**
 * @brief  ModbusPoller main loop — called from ModbusPollerTask entry point.
 *
 * Runs the poll-and-command-drain cycle indefinitely. Never returns.
 */
void modbus_poller_run(void);
```

### 5.3 Poll cycle execution

`modbus_poller_run()` implements the following cycle on each
`poll_timer_tick`:

```
1. Drain command queue:
   while (xQueueReceive(cmd_queue, &cmd, 0) == pdTRUE):
       execute_command(&cmd)   /* calls modbus_master_transact() */
       post result to cmd.response_queue

2. For each slave in DeviceProfileRegistry allowlist (round-robin):
   a. Build FC04 read-input-register request for sensor data range.
   b. Call modbus_master_transact(&request, &response).
   c. record_poll_outcome(slave_addr, result):
      - On success: consecutive_successes++; consecutive_failures = 0.
        if consecutive_successes == 3 && !link_online: emit link_up(slave_id).
      - On failure: consecutive_failures++; consecutive_successes = 0.
        if consecutive_failures == 3 &&  link_online: emit node_offline(slave_id).
   d. If success: forward response data to downstream consumers
      (SensorService / HealthMonitor via registered callback or queue).
```

**Commands are drained before polls.** This ensures time-push writes
(TimeService) and config writes arrive before the next poll reads back
stale values.

### 5.4 Link-state hysteresis

This is the model-variable logic from `state-machines.md` Machine 4,
expressed as C:

```c
static void record_poll_outcome(modbus_slave_state_t *slave,
                                modbus_master_err_t   result)
{
    if (result == MODBUS_MASTER_ERR_OK) {
        slave->consecutive_failures  = 0U;
        slave->consecutive_successes = (slave->consecutive_successes < 3U)
                                       ? slave->consecutive_successes + 1U : 3U;
        if (slave->consecutive_successes == 3U && !slave->link_online) {
            slave->link_online = true;
            emit_link_up(slave->slave_addr);          /* REQ-NF-104 */
        }
    } else {
        slave->consecutive_successes = 0U;
        slave->consecutive_failures  = (slave->consecutive_failures < 3U)
                                       ? slave->consecutive_failures + 1U : 3U;
        if (slave->consecutive_failures == 3U && slave->link_online) {
            slave->link_online = false;
            emit_node_offline(slave->slave_addr);     /* REQ-NF-103, NF-215 */
        }
    }
}
```

The cap at 3 prevents counter overflow on permanently-offline slaves
without resetting on each failure (which would prevent the event firing).

`emit_link_up()` and `emit_node_offline()` post to a FreeRTOS event
group shared with HealthMonitor and CloudPublisher. They do not block.

### 5.5 Health metric reporting (Metric Producer Pattern)

On each health-report interval, ModbusPoller:

1. Calls `modbus_master_get_stats(&stats)`.
2. Maps stats fields to `IHealthReport` metric IDs and calls
   `health_report_update(metric_id, value)` for each.
3. Reports per-slave link state from its own `modbus_slave_state_t` table.

This keeps stats production in ModbusMaster (which observes them directly)
and reporting in ModbusPoller (which owns the application-level view of
bus health). ModbusMaster never touches `IHealthReport` directly.

---

## 6. CRC sharing

Both ModbusMaster (GW) and ModbusSlave (FD) use the same CRC-16/IBM
algorithm. The implementation lives in:

```
firmware/common/modbus_crc.h
firmware/common/modbus_crc.c
```

This is the only file shared across both board firmware trees. It has no
board-specific dependencies (pure arithmetic + a ROM table). The build
system includes it in both the `field-device` and `gateway` targets.

---

## 7. Command queue design

```c
#define MODBUS_CMD_QUEUE_DEPTH  4U   /* TBD — see MBM-O1 */

static QueueHandle_t s_cmd_queue;
```

Queue depth 4 is a provisional value. In steady-state operation, at most
two commands per poll cycle are expected (one time-push from TimeService
per NTP sync, occasional config writes from ConsoleService). Depth 4
provides margin without wasting RAM. Confirm at integration — see MBM-O1.

**No dynamic memory in the queue.** `modbus_command_t` is posted by value
(copied into the queue). The struct is 260 bytes; at depth 4 the queue
consumes ~1 KB of static RAM — within budget.

---

## 8. Init ordering

```
modbus_uart_driver_init()          ← driver layer
modbus_master_init(200, 3)         ← protocol library
[DeviceProfileRegistry loaded]     ← allowlist available
modbus_poller_init(1000)           ← creates command queue
[ModbusPollerTask created]         ← calls modbus_poller_run()
```

ModbusPoller does not register any ISR callbacks itself. The UART ISR is
owned by `ModbusUartDriver`. No two-phase init beyond the ordering above.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

Error codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns.

## 7. Unit-test plan

```c
#ifdef UNIT_TEST
/* ModbusMaster stubs */
#define modbus_uart_tx(buf, len)      stub_uart_tx(buf, len)
#define modbus_uart_rx_start()        stub_uart_rx_start()
/* Inject a synthetic response or timeout via stub before calling transact */

/* ModbusPoller stubs */
#define modbus_master_transact(q, r)  stub_master_transact(q, r)
#endif
```

Minimum ModbusMaster test cases:
- Successful FC04 round trip → response decoded, stats.transactions_ok++.
- Timeout on first attempt, success on second → stats.timeouts == 1, stats.transactions_ok == 1.
- Three consecutive timeouts → `MODBUS_MASTER_ERR_TIMEOUT` returned, stats.timeouts == 3.
- CRC error on response → stats.crc_errors++, retried.
- Exception response (0x81 + code 0x02) → `MODBUS_MASTER_ERR_EXCEPTION`, logged.
- Wrong slave address in response → `MODBUS_MASTER_ERR_BAD_RESPONSE`, not retried.

Minimum ModbusPoller test cases:
- Two consecutive successes → no link event emitted.
- Third consecutive success from OFFLINE state → `link_up` emitted exactly once.
- Third consecutive failure from ONLINE state → `node_offline` emitted exactly once.
- Command draining: command posted to queue → executed before next poll, result on response_queue.
- Slave not in allowlist → `MODBUS_POLLER_ERR_REJECTED`.

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| MBM-O1  | Command queue depth — 4 is provisional. Confirm at integration by measuring peak queue occupancy under worst-case concurrent callers (TimeService + ConsoleService + UpdateService). | Measure peak queue occupancy at integration under worst-case callers | Open |
| MBM-O2  | Multi-slave poll round-robin order — currently one slave (the FD). When a second slave is added, confirm that the round-robin does not starve slower slaves or exceed the poll period. | Address when second Modbus slave is added to the system | Open |
| MBM-O3  | Response timer implementation — FreeRTOS software timer vs hardware timer. Software timer has jitter of one tick period (1 ms at 1 kHz tick). At 200 ms timeout this is 0.5% jitter — acceptable. Confirm if hardware timer is required for tighter tolerance. | Confirm timing tolerance requirement at integration; software timer assumed sufficient | Open |
| MBM-O4  | FC03 (Read Holding Registers) support — ModbusMaster must support FC03 for config reads (modbus-register-map.md §3). `modbus_master_transact()` is FC-agnostic (frame is pre-built by caller); no additional API change needed. Confirm at implementation. | Confirm FC03 support at implementation — API is FC-agnostic by design | Open |
