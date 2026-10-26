# LLD Companion — ModbusPoller

**Companion document to `hld.md`.** This artefact specifies the
**software design** of the `ModbusPoller` Application component on
the Gateway — its interface, internal scheduling, link-state
hysteresis, command routing, health reporting, and test plan.

ModbusPoller is the **application-level orchestrator** that drives
the Modbus polling cycle. It consumes `ModbusMaster` (Middleware) as
a reusable protocol transport — ModbusMaster knows nothing about
polling schedules, link state, or device profiles. This companion
documents only ModbusPoller; the ModbusMaster companion is a separate
document at its own layer.

**Layer:** Application
**Board:** Gateway (GW) only
**Provides:** `IModbusPoller`
**Consumes:** `IModbusMaster`, `IModbusMasterStats` (ModbusMaster),
`IDeviceProfileProvider` (DeviceProfileRegistry, placeholder — MBP-O4),
`IHealthReport` (HealthMonitor), `ILogger`
**SRS traces:** REQ-MB-010, MB-020, MB-080, MB-090, MB-100, MB-110,
MB-120, MB-130, MB-0E1; REQ-NF-103, NF-104, NF-110, NF-201, NF-215
**HLD ref:** `components.md` §Application — ModbusPoller;
`state-machines.md` Machine 4 (model variables);
`hld.md` §7.5, §8.4 (Metric Producer);
`sequence-diagrams.md` SD-02, SD-00b;
`task-breakdown.md` §ModbusPollerTask
**Version:** 1.0
**Date:** October 2026
**Status:** Pass H — Implementation ready (as built; see MBP-D6)

**HLD anchor:** ModbusPoller in `components.md` (GW application layer)

---

## 1. Purpose and scope

### 1.1 In scope

- The public interface `IModbusPoller` (`modbus_poller_init`,
  `modbus_poller_send_command`, `modbus_poller_run`,
  `modbus_poller_reset_for_test`).
- The poll-cycle algorithm: command drain → slave poll → health
  report → delay.
- Per-slave link-state hysteresis (3-consecutive threshold for
  `link_up` / `node_offline` events).
- Command queue: struct layout, static allocation, posting and
  draining semantics.
- Health metric reporting via `IHealthReport` (Metric Producer
  Pattern, P5).
- Interaction with `IDeviceProfileProvider` (DeviceProfileRegistry)
  for the slave allowlist and register-range configuration.
- Concurrency model, error codes, memory sizing, and test plan.

### 1.2 Out of scope

- **Wire-level frame encoding, CRC, response timeout, retry loop** —
  ModbusMaster companion (Middleware).
- **UART driver, DMA, RS-485 direction control** —
  ModbusUartDriver companion (Driver).
- **Register address layout, types, scaling** —
  `docs/modbus-register-map.md` (HLD Artefact #7).
- **DeviceProfileRegistry internals** — its own companion
  (Application).
- **HealthMonitor, CloudPublisher, LcdDisplay internals** — their
  own companions.
- **Modbus task structure and stack sizing rationale** —
  `task-breakdown.md` §ModbusPollerTask.

### 1.3 Terminology

- **Poll cycle** — one iteration of the `modbus_poller_run()` main
  loop: drain commands, poll all slaves, report health, delay.
- **Command** — a one-shot Modbus write (FC06 / FC16) posted by an
  application component (TimeService, UpdateService, ConsoleService)
  for ModbusPoller to execute on their behalf.
- **Hysteresis threshold** — 3 consecutive successes (or failures)
  required before a link-state transition fires.
- **Allowlist** — the set of slave addresses provisioned in
  DeviceProfileRegistry. Commands to addresses not in the allowlist
  are rejected.
- **Metric Producer Pattern** — ModbusPoller polls
  `IModbusMasterStats` (accumulated counters) and pushes through
  `IHealthReport`; events (`link_up`, `node_offline`) are pushed
  directly.

---

## 2. Sources

Per `components.md` (GW Application layer):

| Field | Value |
|---|---|
| **Name** | `ModbusPoller` |
| **Layer** | Application |
| **Board** | Gateway only |
| **Provides** | `IModbusPoller` |
| **Uses** | `ModbusMaster` (`IModbusMaster`, `IModbusMasterStats`), `DeviceProfileRegistry` (`IDeviceProfileProvider`), `IHealthReport`, `ILogger` |
| **Hosted in task** | `ModbusPollerTask` (priority 4, stack 512 words / 2 KB, per `task-breakdown.md`) |

**Design decision MBP-D1 (singleton, not ADT):** ModbusPoller uses
the singleton pattern with free functions — consistent with
ModbusUartDriver and ModbusMaster. Rationale: all dependency arrows
flow downward (Application → Middleware → Driver). No DIP inversion
requires a vtable or opaque handle. CMock mocks singletons by
replacing the `.c` file at link time, so testability is unaffected.

### 2.1 Patterns applied

- **Client-Server** — ModbusPoller (client) drives ModbusMaster
  (server) for each Modbus transaction. ModbusMaster knows nothing
  about polling schedules or slave state.
- **Metric Producer Pattern** (HLD §8.4, P5) — polls
  `IModbusMasterStats` (accumulated counters) and pushes through
  `IHealthReport`. Event-based signals (`link_up`, `node_offline`)
  pushed directly.
- **Pull-based data access** (P7) — polls ModbusMaster on its own
  task cadence; ModbusMaster does not push results unprompted.
  Stats are read by pull (`modbus_master_get_stats()`), not pushed.

### 2.2 Consumed interfaces

ModbusPoller calls the following external functions. Full API
documentation is in each component's own companion.

**ModbusMaster** (`modbus_master.h`) — functions called by
ModbusPoller:

```c
modbus_master_err_t modbus_master_start(void);
modbus_master_err_t modbus_master_transact(const modbus_frame_t *request,
                                           modbus_frame_t       *response);
modbus_master_err_t modbus_master_get_stats(modbus_master_stats_t *stats_out);
modbus_master_err_t modbus_master_reset_stats(void);
```

Types consumed from `modbus_master.h`:

```c
typedef enum {
    MODBUS_MASTER_ERR_OK              = 0,
    MODBUS_MASTER_ERR_NOT_INIT        = 1,
    MODBUS_MASTER_ERR_NULL_ARG        = 2,
    MODBUS_MASTER_ERR_TIMEOUT         = 3,
    MODBUS_MASTER_ERR_CRC             = 4,
    MODBUS_MASTER_ERR_BAD_RESPONSE    = 5,
    MODBUS_MASTER_ERR_EXCEPTION       = 6,
    MODBUS_MASTER_ERR_RX_ERROR        = 7,
} modbus_master_err_t;

typedef struct {
    uint32_t transactions_sent;
    uint32_t transactions_ok;
    uint32_t timeouts;
    uint32_t crc_errors;
    uint32_t rx_errors;
    uint32_t exception_responses;
    uint32_t bad_responses;
} modbus_master_stats_t;
```

**Shared type** (`firmware/common/modbus_frame.h`):

```c
#define MODBUS_MAX_FRAME_BYTES  256U
#define MODBUS_MAX_DATA_BYTES   252U   /* 256 - addr(1) - FC(1) - CRC(2) */

typedef struct {
    uint8_t  slave_addr;
    uint8_t  function_code;
    uint8_t  data[MODBUS_MAX_DATA_BYTES];
    uint16_t data_len;
} modbus_frame_t;
```

**DeviceProfileRegistry** (`device_profile_registry.h`) — functions
called by ModbusPoller:

```c
typedef struct {
    uint8_t  slave_addr;
    uint16_t input_reg_start;    /* FC04 range start */
    uint16_t input_reg_count;    /* FC04 range count */
    /* Additional profile fields defined in DeviceProfileRegistry companion */
} device_profile_t;

/**
 * @brief  Iterate the registered device profiles.
 *
 * Returns the next profile in the registry. Set *index = 0 and
 * *profile = NULL before the first call. Returns false when no
 * more profiles exist.
 */
bool device_profile_get_next(const device_profile_t **profile,
                             uint8_t *index);

/**
 * @brief  Check whether a slave address is in the registry.
 *
 * Used by modbus_poller_send_command() to validate command targets.
 */
bool device_profile_is_registered(uint8_t slave_addr);
```

**IHealthReport** (`health_monitor/health_monitor.h`, Gateway) — as built
(MBP-D6; the `health_report_update(metric_id, value)` / `METRIC_MB_*`
design in the first draft of this companion does not exist):

```c
health_monitor_err_t health_monitor_update_modbus_master_stats(
    health_monitor_handle_t handle, const modbus_master_stats_t *stats);
health_monitor_err_t health_monitor_push_event(
    health_monitor_handle_t handle, health_event_t event, uint32_t param);
/* events used: HEALTH_EVENT_MODBUS_LINK_UP / HEALTH_EVENT_MODBUS_LINK_DOWN,
 * param = slave_addr */
```

**DeviceProfileRegistry — placeholder.** The real module is a Draft with no
code. `firmware/gateway/application/device_profile_registry/` carries the two
functions above with weak defaults serving one hard-coded Field Device profile
(slave 1, FC04, start `0x0010`, count 3 — the temperature, humidity and
pressure registers). `device_profile_get_next()` returns the profile at
`*index` and then advances `*index`. The real registry replaces that `.c`
file (MBP-O4).

---

## 3. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Periodic Modbus polling cycle | REQ-MB-010 | UC-07 |
| Time push from gateway to field device | REQ-MB-020 | UC-13 |
| Command routing (write requests) | REQ-MB-080 | UC-14, UC-15 |
| Command response forwarding | REQ-MB-090 | UC-14, UC-15 |
| Multi-slave addressing | REQ-MB-100 | UC-10 |
| Poll period configuration | REQ-MB-110 | — |
| Link-state management | REQ-MB-120 | UC-10 |
| Health metric reporting | REQ-MB-130 | UC-19 |
| Command to non-existent slave rejected | REQ-MB-0E1 | — |
| Node-offline detection | REQ-NF-103, NF-215 | UC-10 |
| Link-up detection | REQ-NF-104 | UC-10 |
| Default poll period 1000 ms | REQ-NF-110 | — |
| No dynamic allocation post-init | REQ-NF-201 | — |

---

## 4. Public API — IModbusPoller

### 4.1 Data types

```c
/* modbus_poller.h */

#ifndef MODBUS_POLLER_H
#define MODBUS_POLLER_H

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "health_monitor/health_monitor.h"
#include "modbus_master/modbus_master.h"   /* modbus_master_err_t for command response */

typedef enum {
    MODBUS_POLLER_ERR_OK          = 0,
    MODBUS_POLLER_ERR_NOT_INIT    = 1,
    MODBUS_POLLER_ERR_NULL_ARG    = 2,
    MODBUS_POLLER_ERR_QUEUE_FULL  = 3,
    MODBUS_POLLER_ERR_REJECTED    = 4,   /* slave not in allowlist */
    MODBUS_POLLER_ERR_INVALID_ARG = 5,   /* bad FC / register count / period (MBP-D6) */
} modbus_poller_err_t;

/**
 * @brief  Command posted to ModbusPoller by other application components.
 *
 * TimeService uses this to write the time register (SD-09 UML note 3).
 * UpdateService uses this for OTA-related writes.
 * ConsoleService uses this for diagnostic writes.
 */
typedef struct {
    QueueHandle_t    response_queue;  /* caller provides; receives result  */
    uint8_t          slave_addr;      /* (response_queue first — MBP-D6)   */
    uint8_t          function_code;   /* FC06 or FC16 */
    uint16_t         start_reg;
    uint16_t         reg_count;       /* FC16: 1..123; FC06: ignored */
    uint16_t         data[125];       /* FC16 max: 123 registers; FC06: 1 */
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

### 4.2 IModbusPoller — provided interface

```c
/**
 * @brief  Initialise ModbusPoller.
 *
 * Creates the command queue (static allocation). Must be called
 * after modbus_master_init() and after DeviceProfileRegistry is
 * loaded.
 *
 * @param  poll_period_ms  Default poll period in ms (REQ-NF-110: 1000 ms); at least 1.
 * @param  health          HealthMonitor handle for stats and link events (MBP-D6).
 * @return MODBUS_POLLER_ERR_OK on success; ERR_NULL_ARG if health is NULL;
 *         ERR_INVALID_ARG if the period is 0.
 * @note   Threading: call before the scheduler starts.
 */
modbus_poller_err_t modbus_poller_init(uint32_t poll_period_ms,
                                       health_monitor_handle_t health);

/**
 * @brief  Post a command for ModbusPoller to execute on behalf of the caller.
 *
 * Non-blocking. Enqueues the command; ModbusPoller executes it between
 * regular polls (commands are drained before the next poll cycle —
 * MBP-D4). The caller blocks on response_queue until the result
 * arrives. The slave address must be in the DeviceProfileRegistry
 * allowlist.
 *
 * Used by: TimeService (time-push FC06/16), UpdateService, ConsoleService.
 * P1 compliance: application → application (same layer); ModbusPoller is
 * the single Modbus bus gatekeeper.
 *
 * @param  cmd  Copied into the queue by value. The struct itself may go
 *              out of scope after this call returns. The response_queue
 *              handle inside cmd must remain valid until the response
 *              arrives.
 * @return MODBUS_POLLER_ERR_OK, MODBUS_POLLER_ERR_QUEUE_FULL,
 *         MODBUS_POLLER_ERR_REJECTED (slave not in allowlist), or
 *         MODBUS_POLLER_ERR_INVALID_ARG (function code not 0x06/0x10, or an
 *         FC16 register count outside 1..123).
 * @note   Threading: callable from any task. Queue provides mutual
 *         exclusion.
 */
modbus_poller_err_t modbus_poller_send_command(const modbus_command_t *cmd);

/**
 * @brief  ModbusPoller main loop — called from ModbusPollerTask entry.
 *
 * Calls modbus_master_start() to complete ModbusMaster's two-phase
 * init (MBP-D3), then runs the poll-and-command-drain cycle
 * indefinitely. Never returns.
 *
 * @note   Threading: must be called from ModbusPollerTask. Never returns.
 */
void modbus_poller_run(void);

#ifdef TEST
/**
 * @brief  Reset internal state for unit-test isolation.
 *
 * Clears slave state table, deletes and recreates command queue,
 * resets config. Each test case calls this in setUp() before
 * re-initialising.
 */
void modbus_poller_reset_for_test(void);

/** One drain / poll / report cycle without the delay. */
void modbus_poller_cycle_for_test(void);

/** Slave state table slot 0..3, or NULL. */
const modbus_slave_state_t *modbus_poller_slave_state_for_test(uint8_t slot);
#endif

#endif /* MODBUS_POLLER_H */
```

---

## 5. Internal design

### 5.1 Singleton state

```c
/* modbus_poller.c */

#include "modbus_poller.h"
#include "modbus_master.h"
#include "modbus_frame.h"
#include "device_profile_registry.h"
#include "i_health_report.h"
#include "i_logger.h"
#include <string.h>

#define MODBUS_MAX_SLAVES       4U   /* max provisioned slaves */
#define MODBUS_CMD_QUEUE_DEPTH  4U   /* confirm at integration (MBP-O1) */

static struct {
    uint32_t                poll_period_ms;
    health_monitor_handle_t health;
    bool                    initialised;
} s_poller;

static modbus_slave_state_t s_slave_states[MODBUS_MAX_SLAVES];

/* Static queue storage — no heap allocation post-init (BARR-C, REQ-NF-201) */
static QueueHandle_t          s_cmd_queue;
static StaticQueue_t          s_cmd_queue_storage;
static uint8_t                s_cmd_queue_buf[MODBUS_CMD_QUEUE_DEPTH
                                              * sizeof(modbus_command_t)];
```

### 5.2 Poll cycle execution

**Design decision MBP-D2 (`vTaskDelayUntil` poll timer):**
`modbus_poller_run()` uses `vTaskDelayUntil()` for periodic timing,
not a FreeRTOS software timer with task notification. Rationale:
using task notifications for both poll timer and UART RX would create
notification collision — `xTaskNotifyWait()` inside
`modbus_master_transact()` could wake on a poll-timer bit instead of
an RX bit. `vTaskDelayUntil()` avoids this entirely and provides the
same jitter tolerance (±1 tick at 1 kHz = 0.1% at 1000 ms poll
period).

**Design decision MBP-D4 (commands drained before polls):** The
command queue is drained before polling. This ensures time-push
writes (TimeService) and config writes arrive before the next poll
reads back stale values.

```c
void modbus_poller_run(void)
{
    /* Complete ModbusMaster two-phase init — fatal if not ready (MBP-D3) */
    modbus_master_err_t start_err = modbus_master_start();
    configASSERT(start_err == MODBUS_MASTER_ERR_OK);

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {

        /* 1. Drain command queue (commands execute before polls — MBP-D4) */
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

        /* 3. Periodic health report (every poll cycle — MBP-D5) */
        report_health_metrics();

        /* 4. Wait for next poll period */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_poller.poll_period_ms));
    }
}
```

**As built (MBP-D6).** The pseudo-code above is the design intent. In the code the
body of the loop is `poll_cycle()` (`drain_commands()`, `poll_slaves()`,
`report_health_metrics()`); the slave state is located by address with
`find_or_claim_slot()` rather than by the iterator index; and the whole cycle runs
before `vTaskDelayUntil()`.

### 5.3 Link-state hysteresis

Model-variable logic from `state-machines.md` Machine 4. Three
consecutive successes transition from OFFLINE → ONLINE (`link_up`,
REQ-NF-104). Three consecutive failures transition from ONLINE →
OFFLINE (`node_offline`, REQ-NF-103, REQ-NF-215).

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
without resetting on each failure (which would prevent the event
firing).

Exception responses (`MODBUS_MASTER_ERR_EXCEPTION`) count as
failures for hysteresis purposes — a slave that consistently returns
exceptions is effectively unreachable for functional purposes.

### 5.4 Command queue design

```c
#define MODBUS_CMD_QUEUE_DEPTH  4U   /* confirm at integration (MBP-O1) */
```

Queue depth 4 is a provisional value. In steady-state operation, at
most two commands per poll cycle are expected (one time-push from
TimeService per NTP sync, occasional config writes from
ConsoleService). Depth 4 provides margin without wasting RAM.
Confirm at integration — see MBP-O1.

**No dynamic memory in the queue.** `modbus_command_t` is posted by
value (copied into the queue). The struct is 260 bytes; at depth 4
the queue consumes ~1 KB of static RAM — within budget.

### 5.5 Command execution

```c
static void execute_command(modbus_command_t *cmd)
{
    modbus_frame_t request;
    modbus_frame_t response;
    modbus_command_response_t cmd_response;

    /* Build Modbus frame from command struct */
    request.slave_addr    = cmd->slave_addr;
    request.function_code = cmd->function_code;

    if (cmd->function_code == 0x06U) {
        /* FC06: Write Single Register — [reg_hi][reg_lo][val_hi][val_lo] */
        request.data[0] = (uint8_t)(cmd->start_reg >> 8U);
        request.data[1] = (uint8_t)(cmd->start_reg & 0xFFU);
        request.data[2] = (uint8_t)(cmd->data[0] >> 8U);
        request.data[3] = (uint8_t)(cmd->data[0] & 0xFFU);
        request.data_len = 4U;
    } else {
        /* FC16: Write Multiple Registers —
         * [reg_hi][reg_lo][count_hi][count_lo][byte_count][val0_hi][val0_lo]... */
        request.data[0] = (uint8_t)(cmd->start_reg >> 8U);
        request.data[1] = (uint8_t)(cmd->start_reg & 0xFFU);
        request.data[2] = (uint8_t)(cmd->reg_count >> 8U);
        request.data[3] = (uint8_t)(cmd->reg_count & 0xFFU);
        request.data[4] = (uint8_t)(cmd->reg_count * 2U);  /* byte count */
        for (uint16_t i = 0U; i < cmd->reg_count; i++) {
            request.data[5U + (i * 2U)]      = (uint8_t)(cmd->data[i] >> 8U);
            request.data[5U + (i * 2U) + 1U] = (uint8_t)(cmd->data[i] & 0xFFU);
        }
        request.data_len = 5U + (cmd->reg_count * 2U);
    }

    /* Execute via ModbusMaster — blocking, includes retry loop */
    cmd_response.status = modbus_master_transact(&request, &response);

    if (cmd_response.status == MODBUS_MASTER_ERR_OK) {
        /* Decode response payload into uint16 register values */
        cmd_response.data_len = response.data_len / 2U;
        for (uint16_t i = 0U; i < cmd_response.data_len; i++) {
            cmd_response.data[i] =
                ((uint16_t)response.data[i * 2U] << 8U)
                | (uint16_t)response.data[(i * 2U) + 1U];
        }
    } else {
        cmd_response.data_len = 0U;
    }

    /* Post result to caller's response queue (non-blocking, best-effort) */
    (void)xQueueSend(cmd->response_queue, &cmd_response, 0);
}
```

### 5.6 Health metric reporting (Metric Producer Pattern)

On each poll cycle, ModbusPoller calls `modbus_master_get_stats()` and, if
that succeeds, hands the snapshot to
`health_monitor_update_modbus_master_stats()`. Link events are pushed when
hysteresis flips (`push_event(LINK_UP / LINK_DOWN, slave_addr)`). The
per-counter `health_report_update(METRIC_MB_*)` mapping of the first draft is
dropped (MBP-D6): HealthMonitor snapshots the counters it needs
(`transactions_ok`, `timeouts`) from the one struct, and the link flag comes
from the events. HealthMonitor's link flag is single-valued, so with several
slaves it follows the most recent transition (MBP-O2).

ModbusMaster never touches HealthMonitor — this preserves P1 layering
(Middleware does not depend on Application).

**Design decision MBP-D5 (health report interval):** Reports every
poll cycle by default. May decouple to a separate counter if
`IHealthReport` call volume is too high. Default retained; profile
at integration.

### 5.7 Internal helpers

| Helper | Signature | Responsibility |
|---|---|---|
| `build_poll_request` | `static void build_poll_request(const device_profile_t *profile, modbus_frame_t *out)` | Constructs an FC04 (Read Input Registers) request from the device profile's register range (`input_reg_start`, `input_reg_count`). |
| `execute_command` | `static void execute_command(modbus_command_t *cmd)` | Builds a `modbus_frame_t` from the command struct, calls `modbus_master_transact()`, posts the result to `cmd->response_queue`. See §5.5. |
| `forward_response_to_consumers` | `static void forward_response_to_consumers(const device_profile_t *profile, const modbus_frame_t *response)` | Writes decoded register values to the shared data store for consumption by CloudPublisher and LcdDisplay. See MBP-O3. |
| `record_poll_outcome` | `static void record_poll_outcome(modbus_slave_state_t *slave, modbus_master_err_t result)` | Updates per-slave link-state hysteresis counters and emits events. See §5.3. |
| `emit_link_up` | `static void emit_link_up(uint8_t slave_addr)` | `health_monitor_push_event(LINK_UP, slave_addr)`. Logs at INFO via ILogger. |
| `emit_node_offline` | `static void emit_node_offline(uint8_t slave_addr)` | `health_monitor_push_event(LINK_DOWN, slave_addr)`. Logs at WARN via ILogger. |
| `find_or_claim_slot` | `static modbus_slave_state_t *find_or_claim_slot(uint8_t slave_addr)` | Looks a slave up in the state table by address, claiming a free slot on first sight; NULL when the table is full (that slave is skipped). Replaces the draft's `s_slave_states[index]`. |
| `drain_commands`, `poll_slaves` | `static void f(void)` | The two halves of `poll_cycle()`, separate so their frames are never live together. |
| `transact_command`, `post_command_result` | `static` | `execute_command()` split in three for the same reason: the 256-byte request, response and result buffers are never all on the stack at once. |
| `report_health_metrics` | `static void report_health_metrics(void)` | Polls `modbus_master_get_stats()`, maps stats to `IHealthReport` metric IDs, reports per-slave link state. See §5.6. |

**`build_poll_request` implementation:**

```c
static void build_poll_request(const device_profile_t *profile,
                               modbus_frame_t         *out)
{
    out->slave_addr    = profile->slave_addr;
    out->function_code = 0x04U;   /* FC04: Read Input Registers */
    out->data[0]       = (uint8_t)(profile->input_reg_start >> 8U);
    out->data[1]       = (uint8_t)(profile->input_reg_start & 0xFFU);
    out->data[2]       = (uint8_t)(profile->input_reg_count >> 8U);
    out->data[3]       = (uint8_t)(profile->input_reg_count & 0xFFU);
    out->data_len      = 4U;
}
```

---

## 6. Synchronisation

Caller serialises. ModbusPoller holds no internal FreeRTOS
synchronisation primitives (no mutex, no semaphore). The poll loop,
command execution, hysteresis tracking, and health reporting all run
exclusively in ModbusPollerTask context. No additional locking is
required provided the component is not shared across task boundaries.

`modbus_poller_send_command()` is the one function callable from
other tasks. Thread safety is provided by the FreeRTOS queue
(`xQueueSend` / `xQueueReceive` are internally thread-safe).

`emit_link_up()` and `emit_node_offline()` call HealthMonitor's
`push_event`, which serialises every caller with its own mutex.

| Code path | Task | Frequency |
|---|---|---|
| `modbus_poller_run()` (poll loop, command drain, health report) | `ModbusPollerTask` (prio 4) | Periodic, every `poll_period_ms` |
| `modbus_poller_send_command()` | Any task (TimeService, UpdateService, ConsoleService) | On demand |
| `modbus_poller_init()` | Init context (single-threaded boot) | Once |

---

## 7. Init ordering

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

**Design decision MBP-D3 (`modbus_master_start()` called from
`modbus_poller_run()`):** Guarantees the task handle captured by
`xTaskGetCurrentTaskHandle()` inside `modbus_master_start()` belongs
to ModbusPollerTask. Keeps the init sequence clean —
`modbus_poller_run()` is the task entry point.

### 7.1 `modbus_poller_init()` implementation

Rejects a NULL `health` (`ERR_NULL_ARG`) and a zero period
(`ERR_INVALID_ARG`) before touching any state. Then
`xQueueCreateStatic(MODBUS_CMD_QUEUE_DEPTH, sizeof(modbus_command_t), ...)`
with `configASSERT(s_cmd_queue != NULL)`, clears the slave table, stores the
period and handle, and sets `initialised`.

### 7.2 `modbus_poller_send_command()` implementation

Check order: `ERR_NOT_INIT`, `ERR_NULL_ARG`, `ERR_INVALID_ARG` (function code
not `0x06`/`0x10`, or FC16 count outside 1..123 — the 125-word array and the
252-byte frame would otherwise overflow), `ERR_REJECTED`
(`device_profile_is_registered()`), then `xQueueSend(..., 0)` →
`ERR_QUEUE_FULL` on failure. `execute_command()` re-validates what it
dequeues and answers `MODBUS_MASTER_ERR_INVALID_ARG` without a transaction,
because the queue is the trust boundary. A NULL `response_queue` is allowed
(fire and forget). Decoded response words are clamped to 125.

### 7.3 `modbus_poller_reset_for_test()` implementation

```c
#ifdef TEST
void modbus_poller_reset_for_test(void)
{
    if (s_cmd_queue != NULL) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
    memset(s_slave_states, 0, sizeof(s_slave_states));
    memset(&s_poller, 0, sizeof(s_poller));
}
#endif
```

---

## 8. Sequence integration

See the HLD sequence diagrams for inter-component flows. ModbusPoller
calls ModbusMaster synchronously within ModbusPollerTask; no
additional task-level sequencing is required beyond the HLD.

### SD trace

| SD | Component role | Key function |
|---|---|---|
| SD-00b | ModbusPoller calls `modbus_master_transact()` to probe each slave during Gateway boot link establishment; validates device-ID against DeviceProfileRegistry profile | `modbus_poller_run()` (probe phase), `modbus_master_transact()` |
| SD-02 | Polling cycle: ModbusPoller drives the periodic register-read loop; ModbusMaster encodes and transmits each Modbus frame | `modbus_poller_run()` (poll loop), `modbus_master_transact()` |
| SD-06d | ModbusPoller accepts a command from UpdateService to write a maintenance holding register on the FD before OTA reboot (quiesce the field device) | `modbus_poller_send_command()` |
| SD-07 | ConfigService posts a command to ModbusPoller (via `IModbusPoller`) to push a configuration parameter to the FD holding register after a remote configuration update | `modbus_poller_send_command()` |
| SD-09 | TimeService posts the FC06/16 time-write command to ModbusPoller; ModbusMaster encodes and sends the frame | `modbus_poller_send_command()`, `modbus_master_transact()` |

---

## 9. Error and fault behaviour

All public functions return `modbus_poller_err_t`; callers must not
ignore non-OK returns.

### ModbusPoller error codes

| Error value | Cause | Local behaviour | Retry | Observability |
|---|---|---|---|---|
| `ERR_NOT_INIT` | Called before `modbus_poller_init()` | Return error | No — programming error | Caller logs at ERROR |
| `ERR_NULL_ARG` | Null pointer argument | Return error | No — programming error | Caller logs at ERROR |
| `ERR_QUEUE_FULL` | `send_command()` when command queue is full | Return error; command dropped | No — caller should retry after delay | Caller logs at WARN |
| `ERR_REJECTED` | Command addressed to slave not in allowlist | Return error; command dropped | No — configuration error | Caller logs at WARN |

### Handling of ModbusMaster errors in the poll loop

ModbusMaster error codes (`modbus_master_err_t`) received from
`modbus_master_transact()` are handled internally by
`record_poll_outcome()`:

- `MODBUS_MASTER_ERR_OK` → poll success; counted toward link-up
  hysteresis.
- All other codes (`TIMEOUT`, `CRC`, `BAD_RESPONSE`, `EXCEPTION`,
  `RX_ERROR`) → poll failure; counted toward node-offline
  hysteresis. The specific error code is not distinguished for
  hysteresis purposes — all non-OK results count equally as failures.

For commands (via `execute_command()`), the full error code is
forwarded to the caller via `modbus_command_response_t.status`.

---

## 10. Memory and sizing

### RAM budget

| Item | Size | Notes |
|---|---|---|
| Command queue (4 × `modbus_command_t`) | 1,040 B | FreeRTOS static queue + items (260 B × 4) |
| Slave state table (4 × `modbus_slave_state_t`) | 20 B | Max 4 slaves provisioned |
| `s_poller` struct | 8 B | `poll_period_ms` (4 B) + `initialised` (1 B) + padding |
| `s_cmd_queue` handle | 4 B | Queue handle pointer |
| **Total static RAM** | **~1.1 KB** | Excludes task stack |
| ModbusPollerTask stack | 2,048 B | `task-breakdown.md`. Calculated worst path (`-fstack-usage`) ≈ 1.5 KB; measured high-water on the bench ≈ 1.8 KB (MBP-O5) |
| **Total with stack** | **~3.1 KB** | |

### ROM budget

| Item | Size | Notes |
|---|---|---|
| `modbus_poller_run()` + poll logic | ~300 B | Poll loop, command drain |
| `execute_command()` | ~150 B | Frame building + queue post |
| `record_poll_outcome()` + event helpers | ~100 B | Hysteresis + emit |
| `report_health_metrics()` | ~50 B | Stats map + per-slave report |
| **Total estimated ROM** | **~600 B** | |

---

## 11. File layout

```
firmware/gateway/app/modbus_poller/
├── modbus_poller.h              /* IModbusPoller — public API */
└── modbus_poller.c              /* Implementation */

tests/
└── test_modbus_poller.c         /* CMock-based unit tests */
```

No internal header is needed. Helper functions and notification bit
defines are `static` within `modbus_poller.c`.

---

## 12. Unit-test plan

**Framework:** Unity + CMock (project standard). Tests run on the
host machine. Hardware dependencies are mocked by CMock replacing
`.c` files at link time.

**Mocked dependencies:** `modbus_master.c` (provides
`modbus_master_init`, `modbus_master_start`,
`modbus_master_transact`, `modbus_master_get_stats`,
`modbus_master_reset_stats`), DeviceProfileRegistry
(`device_profile_get_next`, `device_profile_is_registered`),
HealthMonitor (`health_monitor_push_event`, `health_monitor_update_modbus_master_stats`), ILogger, FreeRTOS
queue/delay stubs.

**Test file:** `tests/test_modbus_poller.c`

| TC ID | Scenario | Expected result |
|---|---|---|
| TC-MP-001 | `modbus_poller_init()` with valid period | Returns `ERR_OK`; command queue created; `s_poller.initialised == true` |
| TC-MP-002 | Two consecutive poll successes from OFFLINE state | No link event emitted; `consecutive_successes == 2`; `link_online == false` |
| TC-MP-003 | Third consecutive success from OFFLINE state | `emit_link_up()` called exactly once with correct `slave_addr`; `link_online == true` |
| TC-MP-004 | Third consecutive failure from ONLINE state | `emit_node_offline()` called exactly once with correct `slave_addr`; `link_online == false` |
| TC-MP-005 | Success after 2 consecutive failures (ONLINE state) | `consecutive_failures` reset to 0; no event; `link_online` unchanged |
| TC-MP-006 | Failure after 2 consecutive successes (OFFLINE state) | `consecutive_successes` reset to 0; no event; `link_online` unchanged |
| TC-MP-007 | Command posted → `modbus_poller_run()` drains and executes before next poll | `modbus_master_transact()` called with correct FC and register data; result posted to `cmd.response_queue` |
| TC-MP-008 | `send_command()` with slave not in allowlist | Returns `ERR_REJECTED`; `modbus_master_transact()` NOT called |
| TC-MP-009 | `send_command()` when command queue is full | Returns `ERR_QUEUE_FULL` |
| TC-MP-010 | `modbus_poller_init()` then `send_command()` before `run()` | Command queued successfully (returns `ERR_OK`); not executed until `run()` drains |
| TC-MP-011 | Health report: stats polled and forwarded via IHealthReport | `modbus_master_get_stats()` called; `health_monitor_update_modbus_master_stats()` called once with the same struct and the init handle |
| TC-MP-012 | `MODBUS_MASTER_ERR_EXCEPTION` counted as failure for hysteresis | `consecutive_failures` incremented; `consecutive_successes` reset to 0 |
| TC-MP-013 | `send_command()` before `modbus_poller_init()` | Returns `ERR_NOT_INIT` |
| TC-MP-014 | `send_command()` with NULL `cmd` pointer | Returns `ERR_NULL_ARG` |
| TC-MP-015 | Fourth consecutive failure (already OFFLINE after 3) | No duplicate `node_offline` event; `consecutive_failures` capped at 3 |
| TC-MP-016 | Fourth consecutive success (already ONLINE after 3) | No duplicate `link_up` event; `consecutive_successes` capped at 3 |
| TC-MP-017 | FC06 command: `execute_command()` builds correct wire frame | `modbus_master_transact()` receives `function_code == 0x06`, `data_len == 4`, correct register address and value encoding |
| TC-MP-018 | FC16 command: `execute_command()` builds correct wire frame | `modbus_master_transact()` receives `function_code == 0x10`, correct byte count, register address, and multi-register value encoding |
| TC-MP-019 | `init()` with NULL health handle | `ERR_NULL_ARG`; no queue created |
| TC-MP-020 | `init()` with period 0 | `ERR_INVALID_ARG`; no queue created |
| TC-MP-021 | Each non-OK `modbus_master_err_t` (TIMEOUT, CRC, BAD_RESPONSE, EXCEPTION, RX_ERROR) | Counts as a failure; three in a row from ONLINE emit one `LINK_DOWN` |
| TC-MP-022 | Poll request encoding | FC04, slave and span from the profile, `data_len == 4` |
| TC-MP-023 | Two slaves | Independent hysteresis; events carry the right address |
| TC-MP-024 | Five profiles, table of four | Fifth skipped; no overrun |
| TC-MP-025 | Empty registry | No transactions; health still reported |
| TC-MP-026 | `get_stats()` fails | No `update_modbus_master_stats()` call |
| TC-MP-028 | `reset_for_test()` | Queue deleted, state cleared, `send_command()` → `ERR_NOT_INIT` |
| TC-MP-029..033 | `modbus_poller_is_ready()`: false before init, true after, false after a rejected init, true with the link down, false after reset (MBP-D7) |
| TC-MP-029 | `send_command()` with FC03 | `ERR_INVALID_ARG`; nothing queued |
| TC-MP-030 | FC16 count 0, 124, 123 | `ERR_INVALID_ARG`, `ERR_INVALID_ARG`, `ERR_OK` |
| TC-MP-031 | Command whose transaction fails | Response carries the error status and `data_len == 0` |
| TC-MP-032 | Command with NULL `response_queue` | Executed; nothing posted |
| TC-MP-033 | Failed command followed by a successful poll | Command does not touch the hysteresis counters |
| TC-MP-034 | Response decode | Big-endian words, `data_len == payload / 2` |
| TC-MP-035 | `modbus_poller_run()` (left via `longjmp` from the transact spy) | `modbus_master_start()` once; `vTaskDelayUntil()` with the poll period |
| TC-MP-036 | `slave_state_for_test(4)` | `NULL` |
| TC-MP-037 | Corrupt queued command (FC16, count 124) | Answered `ERR_INVALID_ARG`, no transaction |

---

## 13. Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| MBP-O1 | Command queue depth — 4 is provisional. Confirm at integration by measuring peak queue occupancy under worst-case concurrent callers (TimeService + ConsoleService + UpdateService). | Measure peak queue occupancy at integration | Open |
| MBP-O2 | Multi-slave poll round-robin order — currently one slave (the FD). When a second slave is added, confirm that the round-robin does not starve slower slaves or exceed the poll period. | Address when second Modbus slave is added | Open |
| MBP-O4 | DeviceProfileRegistry is a placeholder: weak defaults serve one hard-coded Field Device profile. | Replace `device_profile_registry.c` when the real module is built; reconcile the `idevice_profile_provider_t` vtable with `device_profile_get_next()` / `device_profile_is_registered()` | Open |
| MBP-O5 | Stack headroom — measured on the bench 2026-10-13 (TC-HW-MBP-009): 65 of 512 words free, so about 1.8 KB of the 2 KB budget is used (the `-fstack-usage` estimate of ~1.5 KB missed the FreeRTOS/ISR context frames). Passes, but with only ~260 B margin. | Re-measure with real traffic and the logger active; raise the stack to 640 words if the margin shrinks | Open |
| MBP-O3 | `forward_response_to_consumers()` target interface — writes decoded register values to a shared data store for CloudPublisher and LcdDisplay consumption. Exact data-store interface TBD in the Gateway data-flow design. | Define when Gateway data-flow companion is written | Open |

---

## 14. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| MBP-D1 | Singleton with free functions (not ADT/vtable) | All dependencies flow downward. No DIP inversion needed. Consistent with ModbusUartDriver and ModbusMaster. CMock mocks by replacing `.c` at link time. |
| MBP-D2 | Poll timer via `vTaskDelayUntil()` (not software timer + notification) | Software timer notification would collide with UART RX notification bits inside `modbus_master_transact()`. `vTaskDelayUntil()` avoids this entirely. |
| MBP-D3 | `modbus_master_start()` called from `modbus_poller_run()` | Guarantees the task handle belongs to ModbusPollerTask. Keeps the init sequence clean — `modbus_poller_run()` is the task entry point. |
| MBP-D4 | Commands drained before polls in each cycle | Ensures time-push writes and config writes arrive before the next poll reads back stale values. Prevents one-cycle data staleness. |
| MBP-D6 | As-built deviations from the first draft: (1) HealthMonitor is fed through `update_modbus_master_stats()` and `push_event(LINK_UP/DOWN)`, not `health_report_update(METRIC_MB_*)` and an event group; `init()` takes the handle. (2) Slave state is found by address, not by iterator index. (3) `modbus_command_t` has `response_queue` first and `send_command()` takes `const`. (4) `ERR_INVALID_ARG` added; commands are validated on the way in and again on the way out. (5) `poll_cycle()` and `execute_command()` are split to keep stack frames apart; `poll_cycle()` is also the unit-test seam. | The draft's API did not exist on `main`. The rest keep a corrupt command from overrunning `modbus_frame_t`, keep the task inside 2 KB, and let the host FreeRTOS mock capture the whole command. |
| MBP-D5 | Health report every poll cycle (default) | Simplest approach. Profile at integration; add a down-counter if `IHealthReport` call volume is too high. |
| MBP-D7 | `modbus_poller_is_ready()` returns true once `modbus_poller_init()` has succeeded. It does not require an online link or a started task. | LifecycleController's SelfChecking probe (LC-O6): a Field Device may be absent at boot, and ModbusPollerTask waits on the start gate that opens only after the probe passes, so "started" would deadlock boot. Hardware: TC-HW-MBP-001 checks it before and after init (PASS); TC-HW-MBP-002..009 need the D1-D0 loopback jumper and were not re-run. |

---

## 15. Principles applied

- **P1 (Strict directional layering).** ModbusPoller depends on
  ModbusMaster (Middleware), DeviceProfileRegistry (same Application
  layer), IHealthReport, and Logger — all at the same or lower layer.
  No upward dependency. ModbusPoller is the single Modbus bus
  gatekeeper — other application components route writes through
  `modbus_poller_send_command()` rather than calling ModbusMaster
  directly.
- **P3 (Interface Segregation).** ModbusMaster splits into
  `IModbusMaster` (transaction execution — consumed by ModbusPoller)
  and `IModbusMasterStats` (statistics read-back — polled by
  ModbusPoller for health reporting). These serve non-overlapping
  concerns: protocol transport vs operational visibility.
- **P4 (Cross-cutting concern exception).** Logger referenced
  concretely per the cross-cutting exception.
- **P5 (Metric Producer Pattern).** ModbusMaster exposes accumulated
  counters via `IModbusMasterStats` (poll-based). ModbusPoller polls
  `modbus_master_get_stats()` periodically and pushes to
  `IHealthReport`. Event-based signals (`link_up`, `node_offline`)
  are pushed directly by ModbusPoller. This split follows P5:
  counters → poll, events → push.
- **P6 (Responsibility traces to requirements).** Polling cycle
  traces to REQ-MB-010. Link-state hysteresis traces to REQ-NF-103,
  NF-104, NF-215. Command routing traces to REQ-MB-080, MB-090.
  Multi-slave support traces to REQ-MB-100. Poll period traces to
  REQ-NF-110.
- **P7 (Pull-based data access over push).** ModbusPoller polls
  ModbusMaster on its own task cadence (`vTaskDelayUntil()`).
  ModbusMaster does not push results unprompted. Stats are read by
  pull (`modbus_master_get_stats()`), not pushed.
- **P10 (Naming conventions).** Prefix `modbus_poller_`. Error enum
  `MODBUS_POLLER_ERR_*`. Static state `s_poller`, `s_slave_states`,
  `s_cmd_queue`.

**P2 (Dependency Inversion) does not apply.** All dependencies flow
naturally downward: Application (ModbusPoller) → Middleware
(ModbusMaster) → Driver (ModbusUartDriver). No lower-layer component
needs to call upward. ModbusMaster's health metrics reach
HealthMonitor via P5 (Metric Producer Pattern): ModbusPoller polls
`IModbusMasterStats` and pushes to `IHealthReport` — this is
standard layered design, not DIP.

**P8, P9 do not apply.** P8 (Infrastructure traces to pain) — no
helper infrastructure is introduced. P9 (Multi-view diagrams) — not
a diagram document.

**Coding standard compliance (BARR-C:2018).** Fixed-width types
throughout: function codes `uint8_t`, register addresses and counts
`uint16_t`, stats counters `uint32_t`. No floating-point. All
functions return typed `_err_t` — no silent failures. Static
allocation only: command queue via `xQueueCreateStatic()`,
`modbus_frame_t` buffers stack-local in `modbus_poller_run()`. No
heap after init.

---

## 16. References

- `docs/hld.md` §7.5 (Modbus master sub-machine), §8.4 (Metric
  Producer Pattern).
- `docs/components.md` (GW application layer, ModbusPoller entry).
- `docs/state-machines.md` Machine 4 (Modbus Master state machine —
  model variables `link_state`, `consecutive_successes`,
  `consecutive_failures`).
- `docs/sequence-diagrams.md` SD-02 (Modbus polling cycle), SD-00b
  (Gateway boot), SD-06d (OTA quiesce), SD-07 (config push), SD-09
  (time push).
- `docs/task-breakdown.md` §ModbusPollerTask (priority 4, stack
  512 words / 2 KB).
- `docs/modbus-register-map.md` (register data contract —
  ModbusPoller as Sink for sensor, state, and metrics registers).
- `docs/lld/companions/modbus-master.md` (ModbusMaster Middleware
  companion — defines `IModbusMaster`, `IModbusMasterStats`,
  `modbus_master_err_t`, and the `modbus_frame_t` shared type).
- `docs/lld/companions/device-profile-registry.md`
  (DeviceProfileRegistry companion — defines
  `IDeviceProfileProvider`, `IDeviceProfileManager`, and
  `device_profile_t`).
- `docs/architecture-principles.md` (P1, P3, P4, P5, P6, P7, P10).

---

## Phase H checklist

| # | Check | Status |
|---|---|---|
| H-01 | Every public function has a Doxygen block with `@brief`, `@param`, `@return`, `@note Threading` | Done (§4.2, §7.1, §7.2, §7.3) |
| H-02 | Every error code documented in an error table with cause, behaviour, retry policy, and observability | Done (§9) |
| H-03 | Init ordering documented with pre-conditions | Done (§7) |
| H-04 | Synchronisation strategy documented | Done (§6) |
| H-05 | `_reset_for_test()` hook present, guarded by `#ifdef TEST` | Done (§4.2, §7.3) |
| H-06 | File layout matches project directory structure | Done (§11) |
| H-07 | Unit-test plan uses CMock with TC-NNN identifiers | Done (§12) |
| H-08 | Open items have structured IDs and resolution paths | Done (§13) |
| H-09 | Principles section contains no false claims | Done (§15) — P2/P8/P9 explicitly noted as not applicable; coding standard items separated from P-numbered principles |
| H-10 | SD trace table uses correct function names matching §4 API | Done (§8) |
| H-11 | State machine mapping to code documented | Done (§5.3) — Machine 4 model variables mapped to `record_poll_outcome()` |
| H-12 | Sizing estimates provided (RAM + ROM) | Done (§10) |
| H-13 | All SRS traces from header verified against section content | Done — traces checked against §3, §5, §9 |
| H-14 | Decisions logged with rationale | Done (§14) |
