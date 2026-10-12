# LLD Companion — ModbusMaster · ModbusPoller

**Layer:** ModbusMaster — Middleware · ModbusPoller — Application  
**Board:** Gateway (GW) only  
**Provides:**  
- `ModbusMaster` → `IModbusMaster`, `IModbusMasterStats`  
- `ModbusPoller` → `IModbusPoller`  
**Consumes:**  
- `ModbusMaster` → ModbusUartDriver (`modbus_uart.h` free-function API), `ILogger`  
- `ModbusPoller` → `IModbusMaster`, `IModbusMasterStats`, `IDeviceProfileProvider` (DeviceProfileRegistry), `IHealthReport`, `ILogger`  
**SRS traces:** REQ-MB-010, MB-020, MB-030, MB-040, MB-050, MB-060, MB-080, MB-090, MB-100, MB-110, MB-120, MB-130, MB-0E1; REQ-NF-103, NF-104, NF-105, NF-201, NF-215  
**HLD ref:** `components.md` §Middleware — ModbusMaster; §Application — ModbusPoller; `state-machines.md` Machine 4; `hld.md` §7.5; `sequence-diagrams.md` SD-02, SD-00b; `modbus-register-map.md` §2–§3  
**Version:** 1.1  
**Date:** October 2026  
**Status:** Released — ModbusMaster implemented and hardware-validated on the Gateway (2026-10-12, §17); ModbusPoller not yet implemented (MBM-O8)

**HLD anchor:** ModbusMaster + ModbusPoller in `components.md` (GW middleware + application layers)

---

## 1. Sources

ModbusMaster and ModbusPoller are separate components at different layers,
but they implement **one state machine together** (`state-machines.md`
Machine 4). ModbusMaster owns the protocol mechanics (frame encoding,
CRC, timeout, retry loop); ModbusPoller owns the scheduling, link-state
hysteresis, and routing. Documenting them separately would split the
state machine description across two files. The split point is made
explicit in every section below.

**Design decision MBMP-D1 (singleton, not ADT):** Both components use
the singleton pattern with free functions — consistent with
ModbusUartDriver (MBUART-D8). Rationale: all dependency arrows flow
downward (Application → Middleware → Driver). No DIP inversion requires a
vtable or opaque handle. CMock mocks singletons by replacing the `.c`
file at link time, so testability is unaffected.

**Design decision MBMP-D2 (combined document):** This companion covers
both components. Rationale: they share one state machine (Machine 4),
one task (ModbusPollerTask), and one UART channel. Separate documents
would force each to reference the other for the complete picture.

---

## 2. Shared type — `modbus_frame_t`

```c
/* firmware/common/modbus_frame.h */

#ifndef MODBUS_FRAME_H
#define MODBUS_FRAME_H

#include <stdint.h>

#define MODBUS_MAX_FRAME_BYTES  256U
#define MODBUS_MAX_DATA_BYTES   252U   /* 256 - addr(1) - FC(1) - CRC(2) */

typedef struct {
    uint8_t  slave_addr;
    uint8_t  function_code;
    uint8_t  data[MODBUS_MAX_DATA_BYTES];
    uint16_t data_len;
} modbus_frame_t;

#endif /* MODBUS_FRAME_H */
```

This type is the application-level Modbus PDU. Wire-format encoding
(prepend address, append CRC) and decoding are internal to ModbusMaster.
Both `request` and `response` parameters of `modbus_master_transact()`
use this type.

`modbus_frame_t` lives in `firmware/common/` because it is shared between
ModbusMaster (GW) and ModbusSlave (FD). It has no board-specific
dependencies.

---

## 3. Public API — ModbusMaster

### 3.1 Data types

```c
/* modbus_master.h */

#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#include <stdint.h>
#include "modbus_frame.h"

typedef enum {
    MODBUS_MASTER_ERR_OK              = 0,
    MODBUS_MASTER_ERR_NOT_INIT        = 1,
    MODBUS_MASTER_ERR_NULL_ARG        = 2,
    MODBUS_MASTER_ERR_TIMEOUT         = 3,   /* no response after max retries  */
    MODBUS_MASTER_ERR_CRC             = 4,   /* response CRC mismatch         */
    MODBUS_MASTER_ERR_BAD_RESPONSE    = 5,   /* addr / FC mismatch            */
    MODBUS_MASTER_ERR_EXCEPTION       = 6,   /* slave returned exception PDU   */
    MODBUS_MASTER_ERR_RX_ERROR        = 7,   /* UART framing/overrun during RX */
} modbus_master_err_t;

typedef struct {
    uint32_t transactions_sent;      /* total requests transmitted            */
    uint32_t transactions_ok;        /* responses received and valid          */
    uint32_t timeouts;               /* per-attempt timeouts (before retry)   */
    uint32_t crc_errors;             /* responses with bad CRC                */
    uint32_t rx_errors;              /* UART framing/overrun errors           */
    uint32_t exception_responses;    /* slave returned exception code         */
    uint32_t bad_responses;          /* addr/FC mismatch                      */
} modbus_master_stats_t;
```

### 3.2 `IModbusMaster` — provided interface

```c
/**
 * @brief  Initialise ModbusMaster (phase 1 — pre-scheduler).
 *
 * Stores configuration. Does NOT register UART callbacks or capture
 * the task handle — those require task context (see modbus_master_start).
 * Must be called after modbus_uart_init().
 *
 * @param  response_timeout_ms  Response timeout per attempt (REQ-MB-050: 200 ms).
 * @param  max_retries          Max retry count (REQ-MB-060: 3).
 * @return MODBUS_MASTER_ERR_OK on success.
 * @note   Threading: call before the scheduler starts.
 */
modbus_master_err_t modbus_master_init(uint32_t response_timeout_ms,
                                       uint8_t  max_retries);

/**
 * @brief  Start ModbusMaster (phase 2 — from task context).
 *
 * Captures the calling task's handle for xTaskNotifyFromISR delivery
 * and registers the UART RX callback via modbus_uart_attach_rx().
 * Must be called exactly once, from ModbusPollerTask, after the
 * scheduler has started.
 *
 * @return MODBUS_MASTER_ERR_OK on success;
 *         MODBUS_MASTER_ERR_NOT_INIT if init() was not called.
 * @note   Threading: task-context only. Call once from the owning task.
 */
modbus_master_err_t modbus_master_start(void);

/**
 * @brief  Execute one Modbus transaction with integrated retry.
 *
 * Builds the wire frame (addr + data + CRC), transmits via
 * modbus_uart_transmit(), waits for the response notification from
 * the UART RX ISR callback, validates the response, and retries
 * up to max_retries times on timeout or CRC error.
 *
 * Blocking — runs the full retry loop before returning. Called
 * exclusively from ModbusPollerTask.
 *
 * @param  request   Encoded request (slave_addr, FC, data, data_len).
 * @param  response  Filled on success with slave_addr, FC, and payload.
 * @return MODBUS_MASTER_ERR_OK or an error code.
 * @note   Threading: task-context only, blocking. Not ISR-safe.
 */
modbus_master_err_t modbus_master_transact(const modbus_frame_t *request,
                                           modbus_frame_t       *response);
```

**Why `modbus_master_transact()` handles retries internally:** from
ModbusPoller's perspective, a poll is a single logical operation.
Exposing retry-level control would leak protocol mechanics into
application code. The retry loop (`state-machines.md` T6:
AwaitingResponse → Transmitting) is an implementation detail of
ModbusMaster.

**Why two-phase init (MBMP-D3):** `modbus_master_init()` runs before the
scheduler to register the component in the init sequence. But UART RX
callback delivery requires a task handle (`xTaskNotifyFromISR` target),
and `xTaskGetCurrentTaskHandle()` only works from task context.
`modbus_master_start()` bridges this gap: called once from
ModbusPollerTask's entry point, after the scheduler has started.

### 3.3 `IModbusMasterStats` — provided interface

```c
/**
 * @brief  Copy current stats snapshot.
 *
 * Polled by ModbusPoller on each health-report cycle (Metric Producer
 * Pattern, P5). Thread-safe: stats are incremented only in
 * ModbusPollerTask context, so no mutex is required — atomicity is
 * guaranteed by the single-task caller model.
 *
 * @param  stats_out  Destination; must not be NULL.
 * @return MODBUS_MASTER_ERR_OK on success.
 */
modbus_master_err_t modbus_master_get_stats(modbus_master_stats_t *stats_out);

/**
 * @brief  Reset all counters to zero (triggered by CMD_RESET_METRICS).
 *
 * @return MODBUS_MASTER_ERR_OK on success.
 * @note   Threading: task-context only, non-blocking. Not ISR-safe.
 */
modbus_master_err_t modbus_master_reset_stats(void);

#ifdef TEST
/**
 * @brief  Reset internal state for unit-test isolation.
 *
 * Clears stats, config, init flag, and task handle. Each test case
 * calls this in setUp() before re-initialising.
 */
void modbus_master_reset_for_test(void);
#endif

#endif /* MODBUS_MASTER_H */
```

---

## 4. Public API — ModbusPoller

### 4.1 Data types

```c
/* modbus_poller.h */

#ifndef MODBUS_POLLER_H
#define MODBUS_POLLER_H

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "modbus_master.h"   /* modbus_master_err_t for command response */

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

/**
 * @brief  Per-slave runtime state — one entry per device profile.
 *
 * Managed internally by ModbusPoller. Exposed in the header for
 * test observability only.
 */
typedef struct {
    uint8_t  slave_addr;
    uint8_t  consecutive_successes;
    uint8_t  consecutive_failures;
    bool     link_online;
} modbus_slave_state_t;
```

### 4.2 `IModbusPoller` — provided interface

```c
/**
 * @brief  Initialise ModbusPoller.
 *
 * Creates the command queue. Must be called after modbus_master_init()
 * and after DeviceProfileRegistry is loaded.
 *
 * @param  poll_period_ms  Default poll period in ms (REQ-NF-110: 1000 ms).
 * @return MODBUS_POLLER_ERR_OK on success.
 * @note   Threading: call before the scheduler starts.
 */
modbus_poller_err_t modbus_poller_init(uint32_t poll_period_ms);

/**
 * @brief  Post a command for ModbusPoller to execute on behalf of the caller.
 *
 * Non-blocking. Enqueues the command; ModbusPoller executes it between
 * regular polls. The caller blocks on response_queue until the result
 * arrives. The slave address must be in the DeviceProfileRegistry
 * allowlist.
 *
 * Used by: TimeService (time-push FC06/16), UpdateService, ConsoleService.
 * P1 compliance: application -> application (same layer); ModbusPoller is
 * the single Modbus bus gatekeeper.
 *
 * @param  cmd  Copied into the queue by value. The struct itself may go
 *              out of scope after this call returns. The response_queue
 *              handle inside cmd must remain valid until the response
 *              arrives.
 * @return MODBUS_POLLER_ERR_OK, MODBUS_POLLER_ERR_QUEUE_FULL, or
 *         MODBUS_POLLER_ERR_REJECTED (slave not in allowlist).
 * @note   Threading: callable from any task. Queue provides mutual
 *         exclusion.
 */
modbus_poller_err_t modbus_poller_send_command(modbus_command_t *cmd);

/**
 * @brief  ModbusPoller main loop — called from ModbusPollerTask entry.
 *
 * Calls modbus_master_start() to complete two-phase init, then runs the
 * poll-and-command-drain cycle indefinitely. Never returns.
 *
 * @note   Threading: must be called from ModbusPollerTask. Never returns.
 */
void modbus_poller_run(void);

#ifdef TEST
/**
 * @brief  Reset internal state for unit-test isolation.
 *
 * Clears slave state table, deletes and recreates command queue, resets
 * config. Each test case calls this in setUp() before re-initialising.
 */
void modbus_poller_reset_for_test(void);
#endif

#endif /* MODBUS_POLLER_H */
```

---

## 5. Internal design — ModbusMaster

### 5.1 Responsibility split

| Concern | Owner |
|---------|-------|
| Frame encoding / decoding | ModbusMaster |
| CRC calculation | ModbusMaster (reuses `modbus_crc16()` from `modbus_crc.h`) |
| 200 ms response timeout | ModbusMaster |
| Retry loop (up to 3 retries) | ModbusMaster |
| Per-transaction stats counters | ModbusMaster (exposed via `IModbusMasterStats`) |
| UART RX callback (ISR → task notification) | ModbusMaster |
| Poll timer and scheduling | ModbusPoller |
| Per-slave link-state hysteresis | ModbusPoller |
| `node_offline` / `link_up` event emission | ModbusPoller |
| Command routing from application components | ModbusPoller (`IModbusPoller`) |
| Polling DeviceProfileRegistry for slave list | ModbusPoller |
| Reporting Modbus metrics via `IHealthReport` | ModbusPoller |

ModbusMaster is a **reusable protocol library**. It knows nothing about
polling schedules, link state, or device profiles. ModbusPoller is the
**application-level orchestrator** that uses ModbusMaster as a transport.

### 5.2 Two-phase initialisation

```c
/* Phase 1 — modbus_master_init() — before scheduler */
static struct {
    uint32_t       response_timeout_ms;
    uint8_t        max_retries;
    TaskHandle_t   owner_task;
    modbus_master_stats_t stats;
    bool           initialised;
    bool           started;
} s_mm;

modbus_master_err_t modbus_master_init(uint32_t response_timeout_ms,
                                       uint8_t  max_retries)
{
    s_mm.response_timeout_ms = response_timeout_ms;
    s_mm.max_retries         = max_retries;
    memset(&s_mm.stats, 0, sizeof(s_mm.stats));
    s_mm.owner_task  = NULL;
    s_mm.initialised = true;
    s_mm.started     = false;
    return MODBUS_MASTER_ERR_OK;
}

/* Phase 2 — modbus_master_start() — from task context */
modbus_master_err_t modbus_master_start(void)
{
    if (!s_mm.initialised) {
        return MODBUS_MASTER_ERR_NOT_INIT;
    }
    s_mm.owner_task = xTaskGetCurrentTaskHandle();
    modbus_uart_attach_rx(uart_rx_callback, NULL);
    s_mm.started = true;
    return MODBUS_MASTER_ERR_OK;
}
```

`modbus_master_start()` is called from `modbus_poller_run()` on its
first iteration, before entering the poll loop. This ensures the
task handle belongs to ModbusPollerTask, which is the only task that
calls `modbus_master_transact()`.

### 5.3 UART RX callback

```c
/* modbus_master.c — ISR context */

#define NOTIFY_RX_DONE   (1U << 0)
#define NOTIFY_RX_ERROR  (1U << 1)

static void uart_rx_callback(modbus_uart_event_t event, void *context)
{
    (void)context;
    BaseType_t higher_woken = pdFALSE;

    uint32_t bits = (event == MODBUS_UART_EVENT_RX_DONE)
                    ? NOTIFY_RX_DONE
                    : NOTIFY_RX_ERROR;

    xTaskNotifyFromISR(s_mm.owner_task, bits, eSetBits, &higher_woken);
    portYIELD_FROM_ISR(higher_woken);
}
```

This callback is registered once by `modbus_master_start()` via
`modbus_uart_attach_rx()`. It maps ModbusUartDriver's event enum to
task notification bits:

| UART event | Notification bit | Meaning |
|---|---|---|
| `MODBUS_UART_EVENT_RX_DONE` | `NOTIFY_RX_DONE` (bit 0) | Complete frame received (IDLE-line detected) |
| `MODBUS_UART_EVENT_RX_ERROR` | `NOTIFY_RX_ERROR` (bit 1) | Framing/overrun error during reception |

RX is always armed after `modbus_uart_attach_rx()`. There is no
"start listening" step — the driver continuously receives into its
internal DMA buffer and fires the callback on each IDLE-line event.

### 5.4 State machine implementation (Machine 4 protocol path)

ModbusMaster implements the Transmitting → AwaitingResponse →
ProcessingResponse cycle as a blocking loop inside
`modbus_master_transact()`. The four Machine 4 states map to code
positions — no explicit state enum is needed:

| Machine 4 state | Code position |
|---|---|
| Idle | `vTaskDelayUntil()` in `modbus_poller_run()` outer loop |
| Transmitting | `modbus_uart_transmit()` call (synchronous, blocks until TC) |
| AwaitingResponse | `xTaskNotifyWait()` call (blocks until RX notification or timeout) |
| ProcessingResponse | Validation sequence after `modbus_uart_get_rx_frame()` |

```c
modbus_master_err_t modbus_master_transact(const modbus_frame_t *request,
                                           modbus_frame_t       *response)
{
    if (!s_mm.started)            { return MODBUS_MASTER_ERR_NOT_INIT; }
    if (!request || !response)    { return MODBUS_MASTER_ERR_NULL_ARG;  }

    uint8_t  wire_buf[MODBUS_MAX_FRAME_BYTES];
    uint16_t wire_len = build_wire_frame(request, wire_buf);

    modbus_master_err_t last_err = MODBUS_MASTER_ERR_TIMEOUT;

    for (uint8_t attempt = 0; attempt <= s_mm.max_retries; attempt++) {

        s_mm.stats.transactions_sent++;

        /* ---- Transmitting (T2/T3 → T4) ---- */

        /* Clear stale notification bits from any previous transaction */
        xTaskNotifyStateClear(NULL);

        modbus_uart_err_t tx_err = modbus_uart_transmit(wire_buf, wire_len);
        if (tx_err != MODBUS_UART_ERR_OK) {
            /* TX failure is treated as a timeout — retry */
            s_mm.stats.timeouts++;
            last_err = MODBUS_MASTER_ERR_TIMEOUT;
            continue;
        }

        /* ---- AwaitingResponse (T4 → T5/T6/T7) ---- */

        uint32_t bits = 0;
        BaseType_t got = xTaskNotifyWait(
            0,                                          /* ulBitsToClearOnEntry  */
            NOTIFY_RX_DONE | NOTIFY_RX_ERROR,           /* ulBitsToClearOnExit   */
            &bits,
            pdMS_TO_TICKS(s_mm.response_timeout_ms)
        );

        if (got == pdFALSE) {
            /* Timeout — no notification received (T6/T7) */
            s_mm.stats.timeouts++;
            last_err = MODBUS_MASTER_ERR_TIMEOUT;
            continue;
        }

        if (bits & NOTIFY_RX_ERROR) {
            /* UART-level RX error (framing/overrun) — retry */
            s_mm.stats.rx_errors++;
            last_err = MODBUS_MASTER_ERR_RX_ERROR;
            continue;
        }

        /* ---- ProcessingResponse (T5 → T8/T9/T10) ---- */

        uint8_t  rx_buf[MODBUS_MAX_FRAME_BYTES];
        uint16_t rx_len = 0;

        modbus_uart_err_t rx_err = modbus_uart_get_rx_frame(rx_buf, &rx_len);
        if (rx_err != MODBUS_UART_ERR_OK || rx_len < 4U) {
            s_mm.stats.bad_responses++;
            last_err = MODBUS_MASTER_ERR_BAD_RESPONSE;
            continue;
        }

        /* --- Response validation (§5.5) --- */

        /* 1. CRC check */
        uint16_t calc_crc = modbus_crc16(rx_buf, rx_len - 2U);
        uint16_t recv_crc = (uint16_t)rx_buf[rx_len - 2U]
                          | ((uint16_t)rx_buf[rx_len - 1U] << 8U);
        if (calc_crc != recv_crc) {
            s_mm.stats.crc_errors++;
            last_err = MODBUS_MASTER_ERR_CRC;
            continue;   /* CRC errors are retried */
        }

        /* 2. Slave address match */
        if (rx_buf[0] != request->slave_addr) {
            s_mm.stats.bad_responses++;
            return MODBUS_MASTER_ERR_BAD_RESPONSE;   /* not retried */
        }

        /* 3. Function code check */
        if (rx_buf[1] == (request->function_code | 0x80U)) {
            /* Exception response */
            s_mm.stats.exception_responses++;
            response->slave_addr    = rx_buf[0];
            response->function_code = rx_buf[1];
            response->data[0]       = rx_buf[2];   /* exception code */
            response->data_len      = 1U;
            return MODBUS_MASTER_ERR_EXCEPTION;   /* not retried */
        }

        if (rx_buf[1] != request->function_code) {
            s_mm.stats.bad_responses++;
            return MODBUS_MASTER_ERR_BAD_RESPONSE;   /* not retried */
        }

        /* 4. Success — decode payload */
        response->slave_addr    = rx_buf[0];
        response->function_code = rx_buf[1];
        response->data_len      = rx_len - 4U;   /* minus addr + FC + CRC(2) */
        memcpy(response->data, &rx_buf[2], response->data_len);

        s_mm.stats.transactions_ok++;
        return MODBUS_MASTER_ERR_OK;
    }

    /* All retries exhausted */
    return last_err;
}
```

**Key design points:**

1. **`xTaskNotifyStateClear(NULL)` before each TX.** Clears any stale
   notification from a previous transaction's RX path. Without this, a
   late-arriving IDLE-line interrupt from the previous frame could cause
   `xTaskNotifyWait()` to return immediately with stale data.

2. **TX is synchronous.** `modbus_uart_transmit()` polls TXE and waits
   for TC before returning. There is no TX-complete notification — the
   function returns when the last bit is on the wire and DE has
   de-asserted. This is simpler and correct for half-duplex RS-485.

3. **RX notification race.** Between `modbus_uart_transmit()` returning
   and `xTaskNotifyWait()` starting, the slave may have already
   responded (fast slave on a short bus). The ISR sets the notification
   bits via `xTaskNotifyFromISR(eSetBits)`, and `xTaskNotifyWait()`
   returns immediately when a notification is already pending. No data
   is lost.

4. **`ulBitsToClearOnEntry = 0`.** We do NOT clear bits on entry because
   stale bits were already cleared by `xTaskNotifyStateClear()` before
   TX. Any bits present at this point are from the current transaction's
   ISR callback (fast-slave case) and must be preserved.

5. **Retried vs non-retried errors.** Timeout, CRC, and UART RX errors
   are retried (T6) — all are transient physical-layer issues.
   Bad-response (address/FC mismatch) and exception responses are NOT
   retried — they indicate bus integrity issues or slave-side logic
   errors respectively.

### 5.5 Response validation sequence

```
1. CRC check: modbus_crc16(rx_buf, rx_len - 2) == appended CRC bytes.
   Fail → stats.crc_errors++; retry (T6) or return MODBUS_MASTER_ERR_CRC (T7).

2. Slave address: rx_buf[0] == request->slave_addr.
   Fail → stats.bad_responses++; return ERR_BAD_RESPONSE immediately
   (not retried — a foreign response is a bus error, not a timeout).

3. Function code check:
   a. rx_buf[1] == request->function_code         → normal response; proceed.
   b. rx_buf[1] == (request->function_code | 0x80) → exception response.
      Decode exception code; stats.exception_responses++;
      log exception (slave_addr, FC, code); return MODBUS_MASTER_ERR_EXCEPTION.
   c. other                                        → stats.bad_responses++;
      return MODBUS_MASTER_ERR_BAD_RESPONSE.
```

Exception responses count toward the hysteresis failure counter in
ModbusPoller (same as timeout) — a slave that consistently returns
exceptions is effectively unreachable for functional purposes.

### 5.6 Internal helper — `build_wire_frame`

```c
/**
 * @brief  Encode a modbus_frame_t into wire format: [addr][FC][data...][CRC_lo][CRC_hi].
 *
 * @param  frame   Application-level frame.
 * @param  buf     Output buffer; must be at least MODBUS_MAX_FRAME_BYTES.
 * @return Wire-frame length in bytes (data_len + 4: addr + FC + CRC×2).
 */
static uint16_t build_wire_frame(const modbus_frame_t *frame, uint8_t *buf);
```

This is the only internal helper in ModbusMaster. All other logic
(CRC calculation, notification handling, stats bookkeeping) is inline
in `modbus_master_transact()`.

---

## 6. Internal design — ModbusPoller

### 6.1 Poll cycle execution

**Design decision MBMP-D4 (vTaskDelayUntil poll timer):**
`modbus_poller_run()` uses `vTaskDelayUntil()` for periodic timing, not
a FreeRTOS software timer with task notification. Rationale: using task
notifications for both poll timer and UART RX would create notification
collision — `xTaskNotifyWait()` inside `modbus_master_transact()` could
wake on a poll-timer bit instead of an RX bit, complicating the inner
loop. `vTaskDelayUntil()` is simpler and provides the same jitter
tolerance (±1 tick at 1 kHz = 0.1% at 1000 ms poll period).

```c
void modbus_poller_run(void)
{
    /* Complete two-phase init — fatal if ModbusMaster not ready */
    modbus_master_err_t start_err = modbus_master_start();
    configASSERT(start_err == MODBUS_MASTER_ERR_OK);

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {

        /* 1. Drain command queue (commands execute before polls) */
        modbus_command_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            execute_command(&cmd);
        }

        /* 2. Poll each slave in DeviceProfileRegistry allowlist */
        const device_profile_t *profile = NULL;
        uint8_t index = 0U;
        while (device_profile_get_next(&profile, &index)) {
            modbus_frame_t request;
            modbus_frame_t response;

            build_poll_request(profile, &request);  /* FC04 input registers */

            modbus_master_err_t result =
                modbus_master_transact(&request, &response);

            record_poll_outcome(
                &s_slave_states[index],
                result
            );

            if (result == MODBUS_MASTER_ERR_OK) {
                forward_response_to_consumers(profile, &response);
            }
        }

        /* 3. Periodic health report (every poll cycle; may reduce after profiling) */
        report_health_metrics();

        /* 4. Wait for next poll period */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_poll_period_ms));
    }
}
```

**Commands are drained before polls.** This ensures time-push writes
(TimeService) and config writes arrive before the next poll reads back
stale values.

### 6.2 Link-state hysteresis

Model-variable logic from `state-machines.md` Machine 4:

```c
static void record_poll_outcome(modbus_slave_state_t *slave,
                                modbus_master_err_t   result)
{
    if (result == MODBUS_MASTER_ERR_OK) {
        slave->consecutive_failures  = 0U;
        slave->consecutive_successes = (slave->consecutive_successes < 3U)
                                       ? slave->consecutive_successes + 1U
                                       : 3U;
        if (slave->consecutive_successes == 3U && !slave->link_online) {
            slave->link_online = true;
            emit_link_up(slave->slave_addr);          /* REQ-NF-104 */
        }
    } else {
        slave->consecutive_successes = 0U;
        slave->consecutive_failures  = (slave->consecutive_failures < 3U)
                                       ? slave->consecutive_failures + 1U
                                       : 3U;
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

### 6.3 Health metric reporting (Metric Producer Pattern)

On each health-report interval, ModbusPoller:

1. Calls `modbus_master_get_stats(&stats)`.
2. Maps stats fields to `IHealthReport` metric IDs and calls
   `health_report_update(metric_id, value)` for each.
3. Reports per-slave link state from its own `modbus_slave_state_t` table.

This keeps stats production in ModbusMaster (which observes them
directly) and reporting in ModbusPoller (which owns the application-level
view of bus health). ModbusMaster never touches `IHealthReport` directly —
this preserves P1 layering (Middleware does not depend on Application).

### 6.4 Command queue design

```c
#define MODBUS_CMD_QUEUE_DEPTH  4U   /* confirm at integration (MBM-O1) */

static QueueHandle_t s_cmd_queue;
```

Queue depth 4 is a provisional value. In steady-state operation, at most
two commands per poll cycle are expected (one time-push from TimeService
per NTP sync, occasional config writes from ConsoleService). Depth 4
provides margin without wasting RAM. Confirm at integration — see MBM-O1.

**No dynamic memory in the queue.** `modbus_command_t` is posted by value
(copied into the queue). The struct is 260 bytes; at depth 4 the queue
consumes ~1 KB of static RAM — within budget.

### 6.5 `modbus_poller_init()` implementation

```c
#define MODBUS_MAX_SLAVES  4U   /* max provisioned slaves (sizing §11) */

static struct {
    uint32_t       poll_period_ms;
    bool           initialised;
} s_poller;

static modbus_slave_state_t s_slave_states[MODBUS_MAX_SLAVES];

/* Static queue storage — no heap allocation post-init (BARR-C) */
static StaticQueue_t          s_cmd_queue_storage;
static uint8_t                s_cmd_queue_buf[MODBUS_CMD_QUEUE_DEPTH
                                              * sizeof(modbus_command_t)];

modbus_poller_err_t modbus_poller_init(uint32_t poll_period_ms)
{
    s_cmd_queue = xQueueCreateStatic(
        MODBUS_CMD_QUEUE_DEPTH,
        sizeof(modbus_command_t),
        s_cmd_queue_buf,
        &s_cmd_queue_storage
    );
    configASSERT(s_cmd_queue != NULL);

    s_poller.poll_period_ms = poll_period_ms;
    memset(s_slave_states, 0, sizeof(s_slave_states));
    s_poller.initialised = true;
    return MODBUS_POLLER_ERR_OK;
}
```

### 6.6 Internal helpers — ModbusPoller

| Helper | Signature | Responsibility |
|---|---|---|
| `build_poll_request` | `static void build_poll_request(const device_profile_t *profile, modbus_frame_t *out)` | Constructs an FC04 (Read Input Registers) request from the device profile's register range (start register, count). |
| `execute_command` | `static void execute_command(modbus_command_t *cmd)` | Builds a `modbus_frame_t` from the command struct, calls `modbus_master_transact()`, and posts the result to `cmd->response_queue`. |
| `forward_response_to_consumers` | `static void forward_response_to_consumers(const device_profile_t *profile, const modbus_frame_t *response)` | Writes decoded register values to the shared data store (SensorDataStore or equivalent) for consumption by CloudPublisher and LcdDisplay. |
| `emit_link_up` | `static void emit_link_up(uint8_t slave_addr)` | Sets the `LINK_UP` bit for the given slave in a FreeRTOS event group shared with HealthMonitor. Logs at INFO. Non-blocking. |
| `emit_node_offline` | `static void emit_node_offline(uint8_t slave_addr)` | Sets the `NODE_OFFLINE` bit for the given slave in a FreeRTOS event group shared with HealthMonitor. Logs at WARN. Non-blocking. |
| `report_health_metrics` | `static void report_health_metrics(void)` | Calls `modbus_master_get_stats()`, maps each stats field to an `IHealthReport` metric ID, and calls `health_report_update()` for each. Also reports per-slave link state from `s_slave_states`. |

---

## 7. Synchronisation

Caller serialises. Neither ModbusMaster nor ModbusPoller holds internal
FreeRTOS synchronisation primitives (no mutex, no semaphore). Both are
accessed exclusively from ModbusPollerTask; no additional locking is
required provided neither component is shared across task boundaries.

The only ISR-context code path is `uart_rx_callback()` in ModbusMaster,
which calls `xTaskNotifyFromISR()` — a FreeRTOS-provided atomic
operation that does not require a mutex.

`modbus_poller_send_command()` is the one function callable from other
tasks. Thread safety is provided by the FreeRTOS queue
(`xQueueSend` / `xQueueReceive` are internally thread-safe).

---

## 8. Init ordering

```
modbus_uart_init()                 ← driver layer
modbus_master_init(200, 3)         ← protocol library (phase 1)
[DeviceProfileRegistry loaded]     ← allowlist available
modbus_poller_init(1000)           ← creates command queue
[Scheduler started]
[ModbusPollerTask created]
  └→ modbus_poller_run()           ← calls modbus_master_start() (phase 2),
                                     then enters poll loop
```

`modbus_master_start()` is called from within `modbus_poller_run()`,
not directly from the application init sequence. This ensures the task
handle captured by `xTaskGetCurrentTaskHandle()` belongs to
ModbusPollerTask.

---

## 9. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component
pair is called synchronously within ModbusPollerTask; no additional
task-level sequencing is required beyond the HLD.

### SD trace

| SD | Component role | Key function |
|---|---|---|
| SD-00b | `ModbusPoller` calls `modbus_master_transact()` to probe each slave during Gateway boot link establishment; validates device-ID against `DeviceProfileRegistry` profile | `modbus_poller_run()` (probe phase), `modbus_master_transact()` |
| SD-02 | Polling cycle: `ModbusPoller` drives the periodic register-read loop; `ModbusMaster` encodes and transmits each Modbus frame | `modbus_poller_run()` (poll loop), `modbus_master_transact()` |
| SD-06d | `ModbusPoller` accepts a command from `UpdateService` to write a maintenance holding register on the FD before OTA reboot (quiesce the field device) | `modbus_poller_send_command()` |
| SD-07 | `ConfigService` posts a command to `ModbusPoller` (via `IModbusPoller`) to push a configuration parameter to the FD holding register after a remote configuration update | `modbus_poller_send_command()` |
| SD-09 | `TimeService` posts the FC06/16 time-write command to `ModbusPoller`; `ModbusMaster` encodes and sends the frame | `modbus_poller_send_command()`, `modbus_master_transact()` |

---

## 10. Error and fault behaviour

All public functions return `modbus_master_err_t` or
`modbus_poller_err_t`; callers must not ignore non-OK returns.

### modbus_master_err_t — ModbusMaster error codes

ModbusMaster implements REQ-MB-060: retries on TIMEOUT and CRC errors
up to `max_retries` before surfacing failure. EXCEPTION and
BAD_RESPONSE after the first occurrence are surfaced directly (not retried).

| Error value | Cause | Local behaviour | Retry | Observability |
|---|---|---|---|---|
| `ERR_NOT_INIT` | Called before `modbus_master_start()` | Return error; no UART interaction | No — programming error | Caller logs at ERROR |
| `ERR_NULL_ARG` | Null pointer argument | Return error | No — programming error | Caller logs at ERROR |
| `ERR_TIMEOUT` | No response within `response_timeout_ms` for all attempts | Return error after final attempt | Yes — up to `max_retries` per REQ-MB-060 | `stats.timeouts` incremented per attempt; logged at WARN |
| `ERR_CRC` | Response CRC mismatch | Return error after final attempt | Yes — up to `max_retries` | `stats.crc_errors` incremented; logged at WARN |
| `ERR_BAD_RESPONSE` | Slave address or FC mismatch in response | Return immediately | No — bus integrity issue | `stats.bad_responses` incremented; logged at ERROR |
| `ERR_EXCEPTION` | Slave returned exception PDU (0x80 + FC) | Return immediately; exception code in response | No — slave logic error | `stats.exception_responses` incremented; logged at WARN |
| `ERR_INVALID_ARG` | Zero `response_timeout_ms` in `init()`, or `request->data_len` > 252 (Gateway, MBMP-D9) | Return error; no UART interaction | No — programming error | Caller logs at ERROR |
| `ERR_ALREADY_STARTED` | `modbus_master_start()` called twice (Gateway, MBMP-D9) | Return error; UART callback not re-registered | No — programming error | Caller logs at ERROR |
| `ERR_RX_ERROR` | UART framing/overrun error (NOTIFY_RX_ERROR from ISR) | Return error after final attempt | Yes — up to `max_retries` (transient physical-layer issue) | `stats.rx_errors` incremented; logged at WARN |

### modbus_poller_err_t — ModbusPoller error codes

| Error value | Cause | Local behaviour | Retry | Observability |
|---|---|---|---|---|
| `ERR_NOT_INIT` | Called before `modbus_poller_init()` | Return error | No — programming error | Caller logs at ERROR |
| `ERR_NULL_ARG` | Null pointer argument | Return error | No — programming error | Caller logs at ERROR |
| `ERR_QUEUE_FULL` | `send_command()` when command queue is full | Return error; command dropped | No — caller should retry after delay | Caller logs at WARN |
| `ERR_REJECTED` | Command addressed to slave not in allowlist | Return error; command dropped | No — configuration error | Caller logs at WARN |

---

## 11. Sizing

### RAM budget

| Item | Size | Notes |
|---|---|---|
| `s_mm` struct (ModbusMaster singleton) | 44 B | config + stats (7 × uint32_t) + task handle + flags |
| Wire TX buffer (stack-local in `transact()`) | 256 B | On ModbusPollerTask stack |
| Wire RX buffer (stack-local in `transact()`) | 256 B | On ModbusPollerTask stack |
| Command queue (4 × `modbus_command_t`) | 1,040 B | FreeRTOS static queue + items |
| Slave state table (4 × `modbus_slave_state_t`) | 20 B | Max 4 slaves provisioned |
| `s_poller` struct (ModbusPoller singleton) | 16 B | config + flags + queue handle |
| **Total static RAM** | **~1.1 KB** | Excludes stack |
| ModbusPollerTask stack | 2,048 B | `task-breakdown.md` — includes local buffers |
| **Total with stack** | **~3.1 KB** | |

### ROM budget

| Item | Size | Notes |
|---|---|---|
| CRC-16 lookup table | 512 B | 256 × 2-byte entries in `modbus_crc.c` |
| `modbus_master_transact()` | ~400 B | Estimated; largest function |
| Other ModbusMaster functions | ~200 B | init, start, stats |
| `modbus_poller_run()` + helpers | ~600 B | Poll loop, command exec, hysteresis |
| **Total estimated ROM** | **~1.7 KB** | |

---

## 12. File layout

```
firmware/gateway/
├── middleware/
│   └── modbus_master/
│       ├── modbus_master.h              /* IModbusMaster + IModbusMasterStats */
│       └── modbus_master.c
├── app/
│   └── modbus_poller/
│       ├── modbus_poller.h              /* IModbusPoller */
│       └── modbus_poller.c

firmware/common/
├── modbus_frame.h                       /* modbus_frame_t (shared GW + FD) */
├── modbus_crc.h                         /* modbus_crc16() prototype */
└── modbus_crc.c                         /* CRC-16/IBM table implementation */

tests/
├── test_modbus_master.c                 /* CMock-based unit tests */
└── test_modbus_poller.c                 /* CMock-based unit tests */
```

No internal header is needed for either component. Notification bit
defines and wire-frame helpers are `static` within the `.c` files.

---

## 13. Unit-test plan

**Framework:** Unity + CMock (project standard). Tests run on the host
machine. Hardware dependencies are mocked by CMock replacing `.c` files
at link time.

### ModbusMaster test cases

**Mocked dependencies:** `modbus_uart.c` (provides `modbus_uart_transmit`,
`modbus_uart_attach_rx`, `modbus_uart_get_rx_frame`), `modbus_crc.c`,
FreeRTOS task-notification stubs.

| TC ID | Scenario | Expected result |
|---|---|---|
| TC-MM-001 | `modbus_master_init()` then `modbus_master_start()` | Both return `ERR_OK`; `modbus_uart_attach_rx` called once |
| TC-MM-002 | `modbus_master_start()` without prior `init()` | Returns `ERR_NOT_INIT` |
| TC-MM-003 | `modbus_master_transact()` before `start()` | Returns `ERR_NOT_INIT` |
| TC-MM-004 | Successful FC04 round trip | Response decoded; `stats.transactions_ok == 1`; `stats.transactions_sent == 1` |
| TC-MM-005 | Timeout on first attempt, success on second | `stats.timeouts == 1`; `stats.transactions_ok == 1`; `stats.transactions_sent == 2` |
| TC-MM-006 | Three consecutive timeouts (max_retries=3) | Returns `ERR_TIMEOUT`; `stats.timeouts == 4` (initial + 3 retries); `stats.transactions_sent == 4` |
| TC-MM-007 | CRC error on response, success on retry | `stats.crc_errors == 1`; `stats.transactions_ok == 1` |
| TC-MM-008 | Exception response (0x84, code 0x02) | Returns `ERR_EXCEPTION`; `stats.exception_responses == 1`; exception code in `response.data[0]` |
| TC-MM-009 | Wrong slave address in response | Returns `ERR_BAD_RESPONSE` immediately (no retry); `stats.bad_responses == 1` |
| TC-MM-010 | Wrong FC in response (not exception) | Returns `ERR_BAD_RESPONSE` immediately; `stats.bad_responses == 1` |
| TC-MM-011 | Null `request` pointer | Returns `ERR_NULL_ARG` |
| TC-MM-012 | Null `response` pointer | Returns `ERR_NULL_ARG` |
| TC-MM-013 | `modbus_master_get_stats()` returns current counters | Snapshot matches internal state |
| TC-MM-014 | `modbus_master_reset_stats()` zeroes all counters | All fields zero after reset |
| TC-MM-015 | `modbus_uart_transmit()` returns `ERR_TIMEOUT` | Treated as timeout; retried |
| TC-MM-016 | `NOTIFY_RX_ERROR` bit received | Returns `ERR_RX_ERROR` after max retries; `stats.rx_errors` incremented per attempt |
| TC-MM-017 | FC03 (Read Holding Registers) round trip | Response decoded correctly (API is FC-agnostic) |

Gateway additions (v1.1), all passing:

| TC ID | Scenario | Expected result |
|---|---|---|
| TC-MM-018 | `init()` with zero timeout | `ERR_INVALID_ARG`; `start()` still `ERR_NOT_INIT` |
| TC-MM-019 | Second `start()` | `ERR_ALREADY_STARTED`; callback registered once |
| TC-MM-020 | Wire frame for FC04 read of 1 register | `01 04 00 00 00 01 31 CA` |
| TC-MM-021 | Persistent CRC error | `ERR_CRC` after 4 attempts; `crc_errors == 4` |
| TC-MM-022 | `max_retries == 0` | One attempt only |
| TC-MM-023 | Exception flag with no exception code (runt) | `ERR_BAD_RESPONSE` |
| TC-MM-024 | Request `data_len` of 253 | `ERR_INVALID_ARG`; nothing transmitted |
| TC-MM-025 | Maximum request and response payload (252 B) | OK; 256-byte wire frame |
| TC-MM-026 | Runt response (3 bytes), then success | Retried; `bad_responses == 1` |
| TC-MM-027 | Stale-state flush | Notify flush and UART drain before every TX |
| TC-MM-028 | UART callback | RX_DONE → bit 0, RX_ERROR → bit 1, via `xTaskNotifyFromISR` |
| TC-MM-029 | RX error, then success | Retried; `rx_errors == 1` |

### ModbusPoller test cases

**Mocked dependencies:** `modbus_master.c` (provides `modbus_master_init`,
`modbus_master_start`, `modbus_master_transact`, `modbus_master_get_stats`,
`modbus_master_reset_stats`), DeviceProfileRegistry, IHealthReport, ILogger,
FreeRTOS queue/delay stubs.

| TC ID | Scenario | Expected result |
|---|---|---|
| TC-MP-001 | `modbus_poller_init()` with valid period | Returns `ERR_OK`; command queue created |
| TC-MP-002 | Two consecutive poll successes | No link event emitted; `consecutive_successes == 2` |
| TC-MP-003 | Third consecutive success from OFFLINE state | `link_up` emitted exactly once |
| TC-MP-004 | Third consecutive failure from ONLINE state | `node_offline` emitted exactly once |
| TC-MP-005 | Success after 2 consecutive failures | `consecutive_failures` reset to 0; no event |
| TC-MP-006 | Failure after 2 consecutive successes | `consecutive_successes` reset to 0; no event |
| TC-MP-007 | Command posted → executed before next poll | Result posted to `cmd.response_queue` |
| TC-MP-008 | Slave not in allowlist | `send_command()` returns `ERR_REJECTED` |
| TC-MP-009 | Command queue full | `send_command()` returns `ERR_QUEUE_FULL` |
| TC-MP-010 | `modbus_poller_init()` then `send_command()` before `run()` | Command queued successfully; not executed until `run()` drains |
| TC-MP-011 | Health report: stats polled and forwarded via IHealthReport | `modbus_master_get_stats()` called; `health_report_update()` called with mapped values |
| TC-MP-012 | Exception response counted as failure for hysteresis | `consecutive_failures` incremented |

---

## 14. Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| MBM-O1 | Command queue depth — 4 is provisional. Confirm at integration by measuring peak queue occupancy under worst-case concurrent callers (TimeService + ConsoleService + UpdateService). | Measure peak queue occupancy at integration | Open |
| MBM-O2 | Multi-slave poll round-robin order — currently one slave (the FD). When a second slave is added, confirm that the round-robin does not starve slower slaves or exceed the poll period. | Address when second Modbus slave is added | Open |
| MBM-O3 | Response timeout implementation — `xTaskNotifyWait()` timeout parameter provides the 200 ms window. At 1 kHz tick rate, granularity is 1 ms (0.5% jitter at 200 ms). Confirm timing tolerance at integration. | Confirm with oscilloscope at integration | Open |
| MBM-O4 | FC03 (Read Holding Registers) support — `modbus_master_transact()` is FC-agnostic (frame is pre-built by caller). | TC-MM-017 validates FC03 round-trip; API is FC-agnostic by design — no special handling needed. | Closed |
| MBM-O6 | CRC duplication: the Gateway master carries a private CRC (MBMP-D10) while the Field Device keeps `modbus_crc.c`. | Move one implementation to `firmware/shared/` and update both boards, in a Field Device pass | Open |
| MBM-O7 | `modbus_uart_driver` does not set the UART4 NVIC priority (its companion leaves that to the system configurator). The ISR calls `xTaskNotifyFromISR()`, so priority 0 would trip FreeRTOS. The bring-up harness sets 6 (ceiling is 5). | The real Gateway `main()` must set it | Open |
| MBM-O8 | ModbusPoller is not implemented. It needs DeviceProfileRegistry (Draft), `IHealthReport` and the link-event group API; §4 and §6 are unreviewed against them. | Design pass on DeviceProfileRegistry first | Open |
| MBM-O5 | Health-report interval — reports every poll cycle by default. May decouple to a separate counter if `IHealthReport` call volume is too high. | Default: every cycle. Profile at integration; add counter if needed. | Closed (default set) |

---

## 15. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| MBMP-D1 | Singleton with free functions (not ADT/vtable) | All dependencies flow downward. No DIP inversion needed. Consistent with ModbusUartDriver (MBUART-D8). CMock mocks by replacing `.c` at link time. |
| MBMP-D2 | Combined companion document for both components | One state machine (Machine 4), one task (ModbusPollerTask), one UART channel. Separate docs would cross-reference constantly. |
| MBMP-D3 | Two-phase init: `init()` + `start()` | `init()` runs pre-scheduler for ordering. `start()` runs from task context to capture task handle for `xTaskNotifyFromISR`. |
| MBMP-D4 | Poll timer via `vTaskDelayUntil()` (not software timer + notification) | Software timer notification would collide with UART RX notification bits inside `modbus_master_transact()`. `vTaskDelayUntil()` avoids this entirely. |
| MBMP-D5 | Flush stale RX state before each TX with a non-blocking `xTaskNotifyWait(0, 0xFFFFFFFF, &bits, 0)` plus a drain of the UART buffer (v1.1; the Released v1.0 specified `xTaskNotifyStateClear()`) | `xTaskNotifyStateClear()` resets only the pending state, not bits accumulated by `eSetBits`, so a late RX_DONE from a timed-out attempt would wake the next wait. A late frame left in the driver buffer would also be prepended to the next response. |
| MBMP-D6 | CRC errors and UART RX errors retried; BAD_RESPONSE and EXCEPTION not retried | CRC and RX errors are transient physical-layer issues (noise, framing). BAD_RESPONSE indicates bus collision (retrying won't help). EXCEPTION is a slave logic error (retrying is pointless). |
| MBMP-D7 | TX failure treated as timeout (retried) | A failed `modbus_uart_transmit()` is indistinguishable from a lost frame at the protocol level. |
| MBMP-D8 | `modbus_master_start()` called from `modbus_poller_run()` | Guarantees the task handle belongs to ModbusPollerTask. Keeps the init sequence clean — `modbus_poller_run()` is the task entry point. |
| MBMP-D9 | Added `MODBUS_MASTER_ERR_INVALID_ARG` (= 8) and `MODBUS_MASTER_ERR_ALREADY_STARTED` (= 9) | `data_len` > 252 would overflow the wire buffer and had no code to report it; a silent second `start()` would leave a stale task handle. Appended after RX_ERROR so v1.0 values are unchanged. |
| MBMP-D10 | Gateway-private bitwise CRC-16 inside `modbus_master.c` instead of `modbus_crc16()` | `modbus_crc16()` exists only in the Field Device tree, and a second `modbus_crc.c` would collide in Ceedling's flat source pool. Saves the 512 B table (MBM-O6). |
| MBMP-D11 | `modbus_frame.h` lives in `firmware/shared/`, not `firmware/common/` | `firmware/shared/` is the existing cross-board folder on both include paths. |
| MBMP-D12 | Tests include the Gateway `modbus_uart_driver.h` through `modbus_master.c` and supply the driver functions inline | Ceedling links only what the test TU includes, so the driver `.c` stays out. Tests run in `tests/project_gateway.yml` so the real header resolves and the enum cannot drift from the driver. |

---

## 16. Principles applied

- **P1 (Strict directional layering).** ModbusMaster depends on ModbusUartDriver (driver layer) and Logger. ModbusPoller depends on ModbusMaster, IDeviceProfileProvider, IHealthReport, and Logger — all at the same or lower layer. No upward dependency.
- **P3 (Interface Segregation).** ModbusMaster splits into `IModbusMaster` (transaction execution — consumed by ModbusPoller) and `IModbusMasterStats` (statistics read-back — polled by ModbusPoller for health reporting). These serve non-overlapping concerns: protocol transport vs operational visibility.
- **P4 (Cross-cutting concern exception).** Logger referenced concretely per the cross-cutting exception; documented in §1.
- **P5 (Metric Producer Pattern).** ModbusMaster exposes accumulated counters via `IModbusMasterStats` (poll-based). ModbusPoller polls `modbus_master_get_stats()` periodically and pushes to `IHealthReport`. Event-based signals (`link_up`, `node_offline`) are pushed directly by ModbusPoller. This split follows P5: counters → poll, events → push.
- **P6 (Responsibility traces to requirements).** Request/response cycle traces to REQ-MB-010..130. Link-state hysteresis traces to REQ-NF-103, NF-104, NF-215. Response timeout traces to REQ-MB-050, NF-105. Retry loop traces to REQ-MB-060.
- **P7 (Pull-based data access over push).** ModbusPoller polls ModbusMaster on its own task cadence (`vTaskDelayUntil()`). ModbusMaster does not push results unprompted. Stats are read by pull (`modbus_master_get_stats()`), not pushed.
- **P10 (Naming conventions).** Prefixes `modbus_master_` / `modbus_poller_`. Error enums `MODBUS_MASTER_ERR_*` / `MODBUS_POLLER_ERR_*`. Notification bits `NOTIFY_RX_*` (file-scope `static`).

**P2 (Dependency Inversion) does not apply.** All dependencies flow
naturally downward: Application (ModbusPoller) → Middleware (ModbusMaster)
→ Driver (ModbusUartDriver). No lower-layer component needs to call
upward. ModbusMaster's health metrics reach HealthMonitor via P5
(Metric Producer Pattern): ModbusPoller polls `IModbusMasterStats` and
pushes to `IHealthReport` — this is standard layered design, not DIP.

**P8, P9 do not apply.** P8 (Infrastructure traces to pain) — no helper
infrastructure is introduced. P9 (Multi-view diagrams) — not a diagram
document.

**Coding standard compliance (BARR-C:2018).** Fixed-width types
throughout: function codes `uint8_t`, register addresses and counts
`uint16_t`, stats counters `uint32_t`. No floating-point. All functions
return typed `_err_t` — no silent failures. Static allocation only: PDU
buffers stack-local, stats struct file-scope, command queue via
`xQueueCreateStatic()`. No heap after init.

---

## 17. Gateway implementation record (v1.1)

- Source: `firmware/gateway/middleware/modbus_master/modbus_master.{h,c}`; `firmware/shared/modbus_frame.h`. ModbusPoller is not implemented (MBM-O8).
- Tests: `tests/gateway/middleware/modbus_master/test_modbus_master.c`, 29 tests, 0 ignored, in the Gateway Ceedling project: `CEEDLING_MAIN_PROJECT_FILE=project_gateway.yml ceedling test:test_modbus_master`.
- Hardware harness: `firmware/gateway/integration-tests/modbus_master/main_test_modbus_master.c`. Jumper Arduino D1 to D0: the master hears its own request, which is a well-formed frame, so the whole path (TX, IDLE RX, ISR, notification, CRC, validation) runs on silicon.

| ID | Check |
|---|---|
| TC-HW-MM-001 | init / start; second start rejected |
| TC-HW-MM-002 | FC04 request loops back, payload identical |
| TC-HW-MM-003 | 100-byte payload loops back |
| TC-HW-MM-004 | 252-byte payload (256-byte ADU) loops back |
| TC-HW-MM-005 | 20 consecutive transactions |
| TC-HW-MM-006 | Counters match; `reset_stats()` zeroes them |
| TC-HW-MM-007 | Receiver disabled: TIMEOUT after 4 attempts in ~800 ms |
| TC-HW-MM-008 | Receiver re-enabled: next transaction succeeds |
| TC-HW-MM-009 | Invalid arguments rejected |

**Bench result (2026-10-12, B-L475E-IOT01A, D1–D0 jumper):** TC-HW-MM-001..009 all PASS. The 4-attempt timeout took 832 ms against an ideal 800 ms plus four frame times.

---

## Phase H checklist

| # | Check | Status |
|---|---|---|
| H-01 | Every public function has a Doxygen block with `@brief`, `@param`, `@return`, `@note Threading` | Done (§3, §4) |
| H-02 | Every error code documented in an error table with cause, behaviour, retry policy, and observability | Done (§10) |
| H-03 | Init ordering documented with pre-conditions | Done (§8) |
| H-04 | Synchronisation strategy documented | Done (§7) |
| H-05 | `_reset_for_test()` hook present, guarded by `#ifdef TEST` | Done (§3.3, §4.2) |
| H-06 | File layout matches project directory structure | Done (§12) |
| H-07 | Unit-test plan uses CMock with TC-NNN identifiers | Done (§13) |
| H-08 | Open items have structured IDs and resolution paths | Done (§14) |
| H-09 | Principles section contains no false claims | Done (§16) — P2/P8/P9 explicitly noted as not applicable; coding standard items separated from P-numbered principles |
| H-10 | SD trace table uses correct function names matching §3/§4 API | Done (§9) |
| H-11 | State machine mapping to code documented | Done (§5.4) |
| H-12 | Sizing estimates provided (RAM + ROM) | Done (§11) |
| H-13 | All SRS traces from header verified against section content | Done — traces checked against §5, §6, §10 |
| H-14 | Decisions logged with rationale | Done (§15) |
