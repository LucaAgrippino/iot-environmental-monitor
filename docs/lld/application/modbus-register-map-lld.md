# LLD Companion — ModbusRegisterMap

**Companion document to `hld.md`.** This artefact specifies the
**software design** of the `ModbusRegisterMap` Application component on
the Field Device — its interfaces, internal structure, dispatch
algorithms, concurrency model, error handling, and test plan.

It is **distinct from** `docs/modbus-register-map.md` (HLD Artefact #7),
which defines the **data contract** (register addresses, types,
encoding, exception responses). This companion *references* that
contract — it does not duplicate the per-register tables.

**Version:** 1.0
**Date:** June 2026
**Status:** Pass H — Implementation ready

**HLD anchor:** ModbusRegisterMap in `components.md` (FD application layer)

---

## 1. Purpose and scope

### 1.1 In scope

- The DIP-compliant interface `IModbusRegisterMap` consumed by
  `ModbusSlave`.
- The internal **slot table** structure that maps each defined register
  address to its read / write / validate handlers.
- Dispatch algorithms for FC03, FC04, FC06, FC16.
- The Mediator role between `ConfigService` and `ModbusSlave` for
  protocol-affecting configuration writes.
- Command-register handling (magic-value validation, downstream
  dispatch, read-after-write storage).
- The stats-polling path that polls `IModbusSlaveStats` and reports
  through `IHealthReport`.
- Concurrency model, error mapping, memory sizing, and test plan.

### 1.2 Out of scope

- **Register address layout, types, scaling, exception semantics** —
  `docs/modbus-register-map.md` (HLD Artefact #7).
- **Wire-level frame parsing, CRC, RS-485 direction control** —
  `modbus-slave.md` companion (Middleware).
- **Modbus task structure, priorities, IPC primitives** —
  `task-breakdown.md` §4.
- **`SensorService`, `AlarmService`, `ConfigService`, `HealthMonitor`
  internals** — their own companion documents.

### 1.3 Terminology

- **Slot** — a single entry in the dispatch table, representing one
  defined register address.
- **Provider** — the component that owns the source data for a read
  slot (e.g. `SensorService` for `TEMPERATURE`, `TimeProvider` for
  `UPTIME_*`).
- **Sink** — the component that receives the effect of a write
  (e.g. `ConfigService` for threshold writes, `AlarmService` for
  `CMD_ACK_ALARM`).
- **Pull-on-access** — read handlers fetch the live value from the
  provider at the moment of the FC03 / FC04 request, rather than
  maintaining an internal cache.

---

## 2. Sources

Per `components.md` (FD Application layer):

| Field | Value |
|---|---|
| **Name** | `ModbusRegisterMap` |
| **Layer** | Application |
| **Board** | Field Device only |
| **Provides (upward)** | *(none — top of the stack from the Application perspective)* |
| **Provides (downward, via DIP)** | `IModbusRegisterMap` (consumed by `ModbusSlave`) |
| **Uses** | `ModbusSlave`, `IModbusSlaveStats`, `ISensorService`, `IAlarmService`, `IConfigProvider`, `IConfigManager`, `IHealthSnapshot`, `IHealthReport`, `ITimeProvider`, `ILogger` |
| **Hosted in task** | `ModbusSlaveTask` (priority 4, per `task-breakdown.md` §4.2) |

### 2.1 Patterns applied

- **Mediator** (HLD §8.6) — keeps `ModbusSlave` ignorant of
  project-specific data sources and configuration semantics.
- **Dependency Inversion** (P2) — `IModbusRegisterMap` is an
  Application-owned abstraction implemented by this component and
  depended on by Middleware (`ModbusSlave`). The component diagram
  shows the ball on `ModbusRegisterMap`, the socket on `ModbusSlave`.
- **Interface Segregation** (P3) — consumes `IConfigProvider` for reads
  and `IConfigManager` for writes; `IHealthSnapshot` for reads and
  `IHealthReport` for writes.
- **Pull-based access** (P7) — read handlers query providers live;
  no internal cache duplicates producer state.
- **Metric Producer Pattern** (HLD §8.4) — polls `IModbusSlaveStats`
  (accumulated counters) and pushes through `IHealthReport`.

---

## 3. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Register update on new measurement | REQ-MB-000 | UC-07 |
| Time push from gateway | REQ-MB-020 | UC-13 |
| Function code dispatch | REQ-MB-040 | UC-06, UC-07 |
| Register map specification | REQ-MB-070 | — |
| Remote commands (write side) | REQ-MB-080 | UC-15 |
| Command execution result (read side) | REQ-MB-090 | UC-15 |
| Multi-slave addressing | REQ-MB-100 | UC-06 |
| Device profile identity registers | REQ-MB-111 | UC-06 |
| Unrecognised remote command | REQ-MB-0E1 | UC-15 |
| Alarm flags exposure | REQ-AM-020 | UC-08 |
| Metrics counters exposure | REQ-LD-070 | UC-04 |
| Alarm detection latency (governs read path) | REQ-NF-101 | UC-08 |

---

## 4. Public API — `IModbusRegisterMap`

The DIP contract consumed by `ModbusSlave`. Defined in
`application/include/i_modbus_register_map.h` (Application owns the
header, satisfying P2).

```c
#ifndef I_MODBUS_REGISTER_MAP_H
#define I_MODBUS_REGISTER_MAP_H

#include <stdint.h>

/** Modbus exception codes returned across this interface.
 *  0 means success; non-zero is the exception code to encode in the
 *  Modbus response frame per the Modbus RTU specification.            */
typedef enum {
    MB_EXC_NONE                = 0x00,
    MB_EXC_ILLEGAL_FUNCTION    = 0x01,  /* never returned by MRM       */
    MB_EXC_ILLEGAL_DATA_ADDR   = 0x02,
    MB_EXC_ILLEGAL_DATA_VALUE  = 0x03,
} modbus_exception_t;

typedef struct IModbusRegisterMap IModbusRegisterMap;

struct IModbusRegisterMap {
    void *ctx;  /* concrete modbus_register_map_t * */

    modbus_exception_t (*read_input_regs)    (void           *ctx,
                                              uint16_t        addr,
                                              uint16_t        count,
                                              uint16_t       *out_buf);

    modbus_exception_t (*read_holding_regs)  (void           *ctx,
                                              uint16_t        addr,
                                              uint16_t        count,
                                              uint16_t       *out_buf);

    modbus_exception_t (*write_single_reg)   (void           *ctx,
                                              uint16_t        addr,
                                              uint16_t        value);

    modbus_exception_t (*write_multiple_regs)(void           *ctx,
                                              uint16_t        addr,
                                              uint16_t        count,
                                              const uint16_t *values);
};

#endif /* I_MODBUS_REGISTER_MAP_H */
```

### 4.1 Contract

| Method | Returns `MB_EXC_NONE` when | Returns `MB_EXC_ILLEGAL_DATA_ADDR` when | Returns `MB_EXC_ILLEGAL_DATA_VALUE` when |
|---|---|---|---|
| `read_input_regs` | All `count` addresses in range are defined input registers | Any address in the span is reserved or outside the input range (0x0000–0x004F) | *(not produced on read)* |
| `read_holding_regs` | All `count` addresses are defined holding registers | Any address is reserved or outside the holding range (0x0100–0x02FF) | *(not produced on read)* |
| `write_single_reg` | Address is a defined RW register and `value` passes the slot's validator | Address is read-only, reserved, or unmapped | `value` fails range check (or magic-value check for destructive commands) |
| `write_multiple_regs` | All addresses are defined RW registers, **and** all values pass their validators (pre-validation phase succeeds) | Any address in the span is read-only, reserved, or unmapped | Any value fails its validator |

### 4.2 Binding

The concrete instance is constructed at init and exposed as an
`IModbusRegisterMap *` to `modbus_slave_bind_register_map()`.
ModbusSlave holds the interface pointer for the lifetime of the system;
no rebinding.

### 4.3 Internal error type

A separate enum is used for the auxiliary functions (init, stats
polling) that do not return Modbus exception codes:

```c
typedef enum {
    MRM_ERR_OK             = 0,
    MRM_ERR_NULL_ARG       = 1,
    MRM_ERR_NOT_INITIALISED= 2,
    MRM_ERR_INVARIANT      = 3,  /* slot-table sort broken — debug assert */
    MRM_ERR_PROVIDER_FAIL  = 4,  /* downstream provider returned non-OK   */
} modbus_register_map_err_t;
```

`modbus_register_map_err_t` is returned by `modbus_register_map_init()`
and `modbus_register_map_poll_stats()`. The handler vtable functions
defined in §4 always return `modbus_exception_t` — they convert any
internal error into the appropriate Modbus exception code at the
interface boundary.

---

## 5. Internal design

### 5.1 Public type — opaque

```c
/* application/include/modbus_register_map.h */
typedef struct modbus_register_map modbus_register_map_t;
```

### 5.2 Internal definition

```c
/* application/src/modbus_register_map.c */
struct modbus_register_map {
    bool initialised;

    /* Provider handles (set at init) */
    const ISensorService    *sensors;
    const IAlarmService     *alarms;
    const IConfigProvider   *cfg_read;
    IConfigManager          *cfg_write;
    const IHealthSnapshot   *health_read;
    IHealthReport           *health_write;
    const ITimeProvider     *time;
    const IModbusSlaveStats *mb_stats;
    IModbusSlave            *mb_slave;     /* for Mediator role */
    ILogger                 *log;

    /* Command-register last-written cells (read-after-write semantics
     * per data-spec §6.5). One uint16 per command register.           */
    uint16_t cmd_ack_alarm_last;
    uint16_t cmd_reset_metrics_last;
    uint16_t cmd_soft_restart_last;

    /* Stats-polling cache — last snapshot pushed to IHealthReport.
     * Used to detect deltas.                                          */
    modbus_slave_stats_t last_stats_snapshot;
};
```

### 5.3 The slot table

A single `static const` array of slot descriptors covering every
defined register address. Stored in `.rodata` (no RAM cost beyond the
function-pointer values).

```c
typedef enum {
    REG_TYPE_UINT16,
    REG_TYPE_INT16,
    REG_TYPE_UINT32_HI,   /* high word of a 32-bit pair */
    REG_TYPE_UINT32_LO,   /* low word                   */
    REG_TYPE_BITFIELD16,
    REG_TYPE_ENUM16,
} reg_type_t;

typedef enum {
    REG_ACCESS_R,    /* FC04 only (input range)           */
    REG_ACCESS_RW,   /* FC03 / FC06 / FC16 (holding range)*/
} reg_access_t;

/* Handlers operate on the MRM context so they can reach providers. */
typedef modbus_exception_t (*reg_read_fn_t)
    (const modbus_register_map_t *self, uint16_t *out_value);

typedef modbus_exception_t (*reg_write_fn_t)
    (modbus_register_map_t *self, uint16_t value);

typedef modbus_exception_t (*reg_validate_fn_t)
    (const modbus_register_map_t *self, uint16_t value);

typedef struct {
    uint16_t          addr;
    reg_type_t        type;
    reg_access_t      access;
    reg_read_fn_t     read_fn;       /* NULL → not readable */
    reg_write_fn_t    write_fn;      /* NULL → not writable */
    reg_validate_fn_t validate_fn;   /* NULL → no value check */
} reg_slot_t;

static const reg_slot_t k_slots[] = {
    /* Identity and version (0x0000–0x000F) */
    { 0x0000, REG_TYPE_UINT16, REG_ACCESS_R, read_map_version,  NULL, NULL },
    { 0x0001, REG_TYPE_UINT16, REG_ACCESS_R, read_device_class, NULL, NULL },

    /* Sensor readings (0x0010–0x002F) */
    { 0x0010, REG_TYPE_INT16,  REG_ACCESS_R, read_temperature,  NULL, NULL },
    { 0x0011, REG_TYPE_UINT16, REG_ACCESS_R, read_humidity,     NULL, NULL },
    { 0x0012, REG_TYPE_UINT16, REG_ACCESS_R, read_pressure,     NULL, NULL },

    /* Device state and metrics (0x0030–0x004F) */
    { 0x0030, REG_TYPE_ENUM16,     REG_ACCESS_R, read_device_state, NULL, NULL },
    { 0x0031, REG_TYPE_BITFIELD16, REG_ACCESS_R, read_alarm_flags,  NULL, NULL },
    { 0x0032, REG_TYPE_UINT32_HI,  REG_ACCESS_R, read_uptime_hi,    NULL, NULL },
    { 0x0033, REG_TYPE_UINT32_LO,  REG_ACCESS_R, read_uptime_lo,    NULL, NULL },

    /* Configuration — temperature (0x0110–0x0112) */
    { 0x0110, REG_TYPE_INT16,  REG_ACCESS_RW, read_temp_alarm_lo,
                                              write_temp_alarm_lo,
                                              validate_temp_alarm_lo },
    /* ... per data-spec §6.4 */

    /* Commands (0x0200–0x0202) */
    { 0x0200, REG_TYPE_UINT16, REG_ACCESS_RW, read_cmd_ack_alarm,
                                              write_cmd_ack_alarm,
                                              NULL /* magic in write_fn */ },
    { 0x0201, REG_TYPE_UINT16, REG_ACCESS_RW, read_cmd_reset_metrics,
                                              write_cmd_reset_metrics,
                                              NULL },
    { 0x0202, REG_TYPE_UINT16, REG_ACCESS_RW, read_cmd_soft_restart,
                                              write_cmd_soft_restart,
                                              NULL /* magic 0xA5A5 in write_fn */ },
};

#define K_SLOTS_COUNT  (sizeof k_slots / sizeof k_slots[0])
```

**Sort invariant** — the table is sorted by ascending `addr`. Enforced
at init by a debug-only assertion (`MODBUS_REG_ASSERT_SORTED()`); the
cost is one boot-time pass and is zero in release builds.

### 5.4 Lookup strategy

**Linear scan**. Justification:

- Table size ~40 entries at present; expected to grow to ~60 over
  project life. Worst-case 60 comparisons per access.
- Lookup runs in `ModbusSlaveTask` at priority 4 with REQ-MB-050 budget
  of 200 ms. A 60-entry scan is in the microsecond range — three
  orders of magnitude below the budget.
- A linear scan trivially supports the "any unmapped address inside a
  span returns `ILLEGAL_DATA_ADDR`" requirement: walk the requested
  span, look up each address, stop on first miss.
- Binary search would gain perhaps 50 cycles per access at the cost of
  more complex code and a less obvious failure mode for unmapped
  addresses. Not worth it at this size.

If the table grows past ~120 entries, revisit. Tracked as **MRM-O4**
below.

### 5.5 Synchronisation

Caller serialises. This component holds no internal FreeRTOS
synchronisation primitives. Every entry point (vtable handlers, init,
stats poll) runs in `ModbusSlaveTask` context; no additional locking
required.

### 5.6 Principles applied

- **P1 (Strict directional layering).** Application layer; dispatches
  to middleware and driver services via their interfaces; no layer
  skipped.
- **P2 (Dependency Inversion).** Implements `IModbusRegisterMap`
  (DIP — application owns the header) and consumes provider
  interfaces; concrete dependencies injected at init via vtable
  pointers.
- **P3 (Interface Segregation).** Consumes `IConfigProvider` and
  `IConfigManager` separately, `IHealthSnapshot` and `IHealthReport`
  separately — read and write surfaces remain distinct.
- **P4 (Cross-cutting concern exception).** Logger referenced
  concretely.
- **P5 (Bounded resources, no dynamic allocation post-init).** Slot
  table is `const` in flash; context struct statically allocated; no
  heap.
- **P6 (Responsibility traces to requirements).** Every register-group
  handler traces to the register-address definitions in
  `modbus-register-map.md` (HLD Artefact #7), which trace to REQ-MB-*
  and REQ-SA-* requirements (see §3).
- **P7 (Pull-based access).** Read handlers query providers live; no
  internal cache duplicates producer state.
- **P8 (Total error propagation, no silent failures).** Handler vtable
  functions return `modbus_exception_t`; auxiliary functions return
  `modbus_register_map_err_t`; no silent drops.
- **P9 (BARR-C coding standard).** Register addresses, counts, and
  raw register values are `uint16_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `modbus_register_map_`;
  interface struct `IModbusRegisterMap` (PascalCase per the project's
  vtable convention); internal errors `MRM_ERR_*`; Modbus exceptions
  `MB_EXC_*`.

---

## 6. Dispatch algorithms

### 6.1 FC04 — Read Input Registers

```
read_input_regs(addr, count, out_buf):
    if count == 0 or count > MAX_REGS_PER_READ:
        return ILLEGAL_DATA_VALUE     /* defensive — ModbusSlave should filter */
    for i in 0..count-1:
        slot = find_slot(addr + i)
        if slot == NULL or slot.access != REG_ACCESS_R or slot.read_fn == NULL:
            return ILLEGAL_DATA_ADDR
        rc = slot.read_fn(self, &out_buf[i])
        if rc != NONE:
            return rc                 /* propagates sensor sentinels as data,
                                         not exceptions — see §6.1.1 */
    return NONE
```

#### 6.1.1 Sensor sentinel handling

Per data-spec §5.4, a sensor I/O failure surfaces as a **sentinel value
in the data**, not as a Modbus exception. The read handler for
`TEMPERATURE` queries `ISensorService.get_latest()`; if the result's
`valid` flag is false, the handler returns `0x8000` for `int16` (or
the type-appropriate sentinel) and `MB_EXC_NONE`. This keeps the
protocol contract uniform — the master always gets a successful FC04
response — and pushes failure semantics into the data, where masters
can test for the sentinel before using it. *(D34.)*

### 6.2 FC03 — Read Holding Registers

Same algorithm as FC04 but limited to slots with `access == REG_ACCESS_RW`
within the holding range (0x0100–0x02FF). A slot in the input range
returns `ILLEGAL_DATA_ADDR` even if defined — the function code defines
the range.

### 6.3 FC06 — Write Single Register

```
write_single_reg(addr, value):
    slot = find_slot(addr)
    if slot == NULL or slot.access != REG_ACCESS_RW or slot.write_fn == NULL:
        return ILLEGAL_DATA_ADDR
    if slot.validate_fn != NULL:
        rc = slot.validate_fn(self, value)
        if rc != NONE:
            return rc                 /* typically ILLEGAL_DATA_VALUE */
    return slot.write_fn(self, value)
```

The write handler is responsible for any further validation (e.g.,
magic values for destructive commands) that the simple range-check
validator cannot express.

### 6.4 FC16 — Write Multiple Registers (atomic block write)

Two-phase: **pre-validate all, then apply all.** No rollback path
because nothing is applied during phase 1.

```
write_multiple_regs(addr, count, values):
    if count == 0 or count > MAX_REGS_PER_WRITE:
        return ILLEGAL_DATA_VALUE

    /* Phase 1 — pre-validation. No side effects. */
    for i in 0..count-1:
        slot = find_slot(addr + i)
        if slot == NULL or slot.access != REG_ACCESS_RW or slot.write_fn == NULL:
            return ILLEGAL_DATA_ADDR
        if slot.validate_fn != NULL:
            rc = slot.validate_fn(self, values[i])
            if rc != NONE:
                return rc
        /* Commands without a validator perform their magic-value check
         * inside write_fn — see §6.4.1.                                 */

    /* Phase 2 — apply. By this point every value passed its validator. */
    for i in 0..count-1:
        slot = find_slot(addr + i)
        rc = slot.write_fn(self, values[i])
        if rc != NONE:
            /* Should not happen — phase 1 passed. Log and report. */
            log_error("FC16 phase-2 write_fn failure at 0x%04X", addr + i)
            return rc
    return NONE
```

#### 6.4.1 Mixed-content FC16 blocks

The block-write atomicity guarantee is **pre-validation atomicity** —
either every value passes its validator and is applied, or none is
applied. Command magic-value checks happen inside `write_fn` and are
not covered by phase 1. In practice the Gateway issues FC16 only
across contiguous configuration blocks (e.g., the four temperature
thresholds at 0x0110–0x0113), not across mixed config + command
ranges; this matches the data spec's "atomic configuration block
write" purpose. Resolved as **MRM-O2** below.

### 6.5 Exception mapping summary

| Condition | Returned exception | Where detected |
|---|---|---|
| Function code unsupported | `0x01` | `ModbusSlave` (before MRM) |
| Address unmapped / reserved | `0x02` | Slot lookup miss |
| Address read-only on write FC | `0x02` | `slot.access != RW` |
| Address write-only on read FC | `0x02` | `slot.read_fn == NULL` |
| Value out of range | `0x03` | `slot.validate_fn` |
| Destructive command without magic | `0x03` | `slot.write_fn` |
| Count = 0 or > max per PDU | `0x03` | First lines of read / write |

---

## 7. Mediator role — protocol-affecting configuration

Some configuration writes affect `ModbusSlave`'s own behaviour (slave
address, in principle baud rate though that is fixed for this
project). The Mediator pattern applies: MRM coordinates the write
between `ConfigService` (the configuration authority) and
`ModbusSlave` (the protocol stack).

### 7.1 Slave-address change flow

```
write_modbus_slave_addr(value):
    /* slot.validate_fn already checked 1 ≤ value ≤ 247 */
    rc = cfg_write->set_modbus_slave_addr(cfg_write, (uint8_t)value)
    if rc != CFG_OK:
        return ILLEGAL_DATA_VALUE
    /* ConfigService has persisted the change.
     * Push to ModbusSlave so subsequent frames are filtered on the new
     * address. The current FC06 response goes out on the OLD address
     * because ModbusSlave's response path is already in progress —
     * matches MRM-O3 (resolved).                                     */
    mb_slave->set_slave_address(mb_slave, (uint8_t)value)
    log_info("Modbus slave address changed to %u", value)
    return NONE
```

The Gateway side handles the addressing transition by retrying on the
new address (REQ-MB-060 retry behaviour) after observing a single
response on the old address.

---

## 8. Command-register handling

Per data-spec §6.5, command registers have **read-after-write**
semantics: a read returns the last value written, useful for confirming
receipt. Destructive commands require a magic value; any other value
returns `ILLEGAL_DATA_VALUE`.

### 8.1 `CMD_ACK_ALARM` (0x0200, trigger `0x0001`)

```
write_cmd_ack_alarm(value):
    self->cmd_ack_alarm_last = value         /* always cache the literal */
    if value != 0x0001:
        return ILLEGAL_DATA_VALUE
    /* LLD-D14: ack_all() bulk force-clears all alarm flags. Non-latched
     * semantic — flags re-raise on the next evaluation cycle if the
     * condition persists. See alarm-service.md §6.                     */
    alarm_service->ack_all()
    return NONE

read_cmd_ack_alarm(out):
    *out = self->cmd_ack_alarm_last
    return NONE
```

### 8.2 `CMD_RESET_METRICS` (0x0201, trigger `0x0001`)

```
write_cmd_reset_metrics(value):
    self->cmd_reset_metrics_last = value
    if value != 0x0001:
        return ILLEGAL_DATA_VALUE
    /* LLD-D15: routed via LifecycleController, which dispatches to
     * health_admin->reset_metrics(). MRM does not call HealthMonitor
     * directly — see lifecycle-controller.md §3.                       */
    lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS)
    /* MRM must also zero its own stats snapshot so the next poll does
     * not compute a negative delta (see §9.3).                          */
    memset(&self->last_stats_snapshot, 0, sizeof self->last_stats_snapshot)
    return NONE
```

### 8.3 `CMD_SOFT_RESTART` (0x0202, trigger `0xA5A5`)

```
write_cmd_soft_restart(value):
    self->cmd_soft_restart_last = value
    if value != 0xA5A5:
        log_warn("CMD_SOFT_RESTART rejected: wrong magic 0x%04X", value)
        return ILLEGAL_DATA_VALUE
    /* LLD-D15: route via LifecycleController, which posts
     * LC_EVENT_RESTART_REQUESTED to LifecycleTask. The two-step
     * confirmation requirement (REQ-DM-020) is enforced there.
     * MRM does not call NVIC_SystemReset() directly.                    */
    lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART)
    return NONE
```

The Modbus response to the FC06 write completes before LifecycleTask
processes the restart event — the queue decouples the write handler
from the reset execution.

### 8.4 Read-after-write cells

The three `cmd_*_last` fields in the MRM context are written **only**
in `ModbusSlaveTask` context (the write paths above) and read **only**
in the same task context (FC03 reads via `read_cmd_*`). Single-task
access → no mutex required.

---

## 9. Stats polling — Metric Producer Pattern

`ModbusSlave` exposes `IModbusSlaveStats` (CRC errors, transaction
counts, RX / TX frame counts). MRM polls this interface periodically
and pushes deltas through `IHealthReport`.

### 9.1 Trigger

`ModbusSlaveTask` listens on a task notification with two bits
(per `task-breakdown.md` §4.4 IPC primitives — extended for stats):

| Bit | Source | Meaning |
|---|---|---|
| 0 | `ModbusUartDriver` ISR | UART RX frame complete |
| 1 | FreeRTOS software timer (1 Hz) | Stats poll tick |

On bit 1, the task calls `modbus_register_map_poll_stats(mrm)`.
Resolves session-summary open item **HM-O1** in favour of co-locating
the poll with the protocol stack rather than placing it in
`LifecycleTask`.

### 9.2 Algorithm

```
modbus_register_map_err_t
modbus_register_map_poll_stats(modbus_register_map_t *self):
    if self == NULL or !self->initialised:
        return MRM_ERR_NOT_INITIALISED

    modbus_slave_stats_t now
    self->mb_stats->snapshot(self->mb_stats, &now)

    /* Compute deltas against the last snapshot to feed HealthMonitor's
     * counter-style fields, and push absolute values where appropriate. */
    health_report_modbus(self->health_write, &now, &self->last_stats_snapshot)

    self->last_stats_snapshot = now
    return MRM_ERR_OK
```

`health_report_modbus()` is one of HealthMonitor's typed update
functions per the HealthMonitor LLD.

### 9.3 Reset-on-command interaction

When `CMD_RESET_METRICS` (§8.2) executes, the underlying counters are
reset. MRM's `last_stats_snapshot` is zeroed in the same write handler
so the next poll does not compute a negative delta.

---

## 10. Concurrency model

### 10.1 Task contexts

| Code path | Task | Frequency |
|---|---|---|
| `read_input_regs`, `read_holding_regs`, `write_single_reg`, `write_multiple_regs` | `ModbusSlaveTask` (prio 4) | On UART RX, bounded by master poll period |
| `poll_stats` | `ModbusSlaveTask` (prio 4) | 1 Hz timer notify |
| `init` | Init context (single-threaded boot) | Once |

**Single-task access** — every MRM entry point runs in
`ModbusSlaveTask`.

### 10.2 What MRM owns and how it is protected

| State | Owner | Concurrency |
|---|---|---|
| `cmd_*_last` cells | `ModbusSlaveTask` only | No mutex needed |
| `last_stats_snapshot` | `ModbusSlaveTask` only | No mutex needed |
| Provider / sink pointers | Set at init, immutable after | No mutex needed |

### 10.3 What MRM does **not** own

Sensor readings, alarm state, configuration values, time, health
metrics. Each is owned by its respective provider. MRM accesses them
through their interfaces, which carry their own internal
synchronisation. MRM holds no lock across these calls.

### 10.4 Why pull-on-access is safe

Read handlers call provider getters. The getter is responsible for
atomic snapshot semantics — `SensorService.get_latest()` returns a
fully consistent reading struct under its own internal mutex. The
Modbus response frame is built from a sequence of such getter calls;
the response is therefore a *composite* of consistent snapshots taken
at slightly different microseconds. This is consistent with Modbus
RTU semantics — no part of the spec requires a multi-register read to
be a single atomic snapshot.

### 10.5 Reconciliation with state-machine Internal action I3

Machine 5 (FD lifecycle), Operational state, Internal action I3 reads:

> on `new_processed_reading` → `update_modbus_registers(); update_lcd_buffer()`

Under pull-on-access, `update_modbus_registers()` is implicit — the
next FC04 read returns the new value automatically, with no explicit
write into MRM. The action description holds at the *conceptual*
level (the registers reflect the new reading) but does not map to a
function call in this component. The LCD buffer update remains an
explicit call into `GraphicsLibrary` because LVGL requires widget
invalidation.

REQ-MB-000 ("update Modbus register when new measurement is
available") is satisfied: the register's *exposed value* reflects the
new reading on the next read, with no observable staleness beyond the
natural propagation through `SensorService`.

---

## 11. Sequence integration

| SD | Component role | Key function(s) |
|---|---|---|
| SD-02 | `ModbusSlave` dispatches each read / write request to `IModbusRegisterMap`. MRM walks the slot table and calls provider getters or sink setters | `read_input_regs`, `read_holding_regs`, `write_single_reg`, `write_multiple_regs` |
| SD-05 | `AlarmService` updates its own internal alarm state. The Modbus master reads alarm flags via FC04 at register 0x0031, which under pull-on-access invokes `read_alarm_flags()` → `IAlarmService.get_active_flags()` | `read_alarm_flags()` (internal handler) |
| SD-09 | `ModbusSlave` dispatches the FC06 / 16 time-write to `write_holding_regs`. The time-register handler calls `ITimeProvider.set_synchronised_time()` | `write_single_reg`, `write_multiple_regs` |

There is no explicit `set_alarm_bit()` function on MRM. Alarm state is
held by `AlarmService`; MRM exposes it via the read path
(pull-on-access).

---

## 12. Error and fault behaviour

The vtable handler functions defined in §4 return `modbus_exception_t`
— internal errors are mapped to the appropriate Modbus exception at
the interface boundary. The auxiliary functions (`init`, `poll_stats`)
return `modbus_register_map_err_t`.

| Error value | Cause | Where returned | Local behaviour | Observability |
|---|---|---|---|---|
| `MRM_ERR_NULL_ARG` | Null pointer argument | `init`, `poll_stats` | Return error; assert in debug | Caller logs at ERROR via ILogger |
| `MRM_ERR_NOT_INITIALISED` | Handler called before `init()` | `poll_stats`, vtable handlers (mapped to `MB_EXC_ILLEGAL_DATA_VALUE` at the boundary) | Return error; assert in debug | Logged at ERROR via ILogger; assert halts in debug |
| `MRM_ERR_INVARIANT` | Slot-table sort invariant broken (internal corruption) | `init` | Assert in debug; log and continue in release | Logged at ERROR; assert halts in debug |
| `MRM_ERR_PROVIDER_FAIL` | Downstream provider call returned non-OK | Internal — surfaces as a sensor sentinel in the response (§6.1.1) when produced by a read handler | Sentinel value in data, `MB_EXC_NONE` returned | Logged at WARN via ILogger |
| `MB_EXC_ILLEGAL_DATA_ADDR` | Slot lookup miss, read-only on write, write-only on read | Vtable handlers | Return exception code to ModbusSlave | Counted by `ModbusSlave` stats |
| `MB_EXC_ILLEGAL_DATA_VALUE` | Value out of range; missing magic value | Vtable handlers | Return exception code | Counted by `ModbusSlave` stats |

---

## 13. Memory and sizing

### 13.1 Static footprint

| Item | Size (estimate) | Storage |
|---|---|---|
| Slot table (~40 entries × 16 B) | ~640 B | `.rodata` (flash) |
| MRM context (`modbus_register_map_t`) | ~80 B | `.bss` (RAM) |
| Single static instance | ×1 | — |

No dynamic allocation post-init (P5 / project rule). The MRM instance
is declared as `static modbus_register_map_t g_mrm;` and its
`IModbusRegisterMap` vtable is passed by pointer to
`modbus_slave_bind_register_map()`.

### 13.2 Stack usage per call

| Path | Local frame |
|---|---|
| Single-register read or write | < 64 B (slot pointer + temporaries) |
| FC04 max-span read (up to 125 regs per Modbus PDU) | Caller-provided `out_buf`; MRM adds no per-register stack |
| FC16 max-span write (up to 123 regs per PDU) | Same; phase 1 and phase 2 share the caller's `values` buffer |

No recursion, no VLAs.

---

## 14. Initialisation

```c
modbus_register_map_err_t
modbus_register_map_init(modbus_register_map_t        *self,
                         const ISensorService          *sensors,
                         const IAlarmService           *alarms,
                         const IConfigProvider         *cfg_read,
                         IConfigManager                *cfg_write,
                         const IHealthSnapshot         *health_read,
                         IHealthReport                 *health_write,
                         const ITimeProvider           *time,
                         const IModbusSlaveStats       *mb_stats,
                         IModbusSlave                  *mb_slave,
                         ILogger                       *log);
```

Initialisation order (called from system init):

1. All providers initialised first (`SensorService`, `ConfigService`,
   `HealthMonitor`, `TimeProvider`, `ModbusSlave`).
2. `modbus_register_map_init()` validates all pointers (NULL check),
   stores them, zeroes command cells and stats snapshot, asserts the
   slot-table sort invariant in debug, sets `initialised = true`.
3. The system installs the `IModbusRegisterMap` interface into
   `ModbusSlave` via `modbus_slave_bind_register_map()`.
4. `ModbusSlaveTask` is created and starts processing frames.

The start-gate event group (per `lifecycle-controller.md`, **LC-O3**)
ensures `ModbusSlaveTask` does not begin until step 3 completes.

---

## 15. Unit-test plan

Unity host-side tests. All providers mocked. Test file:
`tests/field-device/application/modbus_register_map/test_modbus_register_map.c`.

| Test ID | Description | Verifies |
|---|---|---|
| TC-MRM-001 | `modbus_register_map_init()` with any NULL provider pointer → `MRM_ERR_NULL_ARG` | Argument validation |
| TC-MRM-002 | `modbus_register_map_init()` happy path → `MRM_ERR_OK`, `initialised == true` | Init success |
| TC-MRM-003 | Slot-table sort invariant — corrupt the table in test, init returns `MRM_ERR_INVARIANT` (debug build) | Sort check |
| TC-MRM-004 | Vtable handler called before init → `MB_EXC_ILLEGAL_DATA_VALUE` | Init guard at interface boundary |
| TC-MRM-005 | FC04 read each defined sensor register → correct value from mock provider, `MB_EXC_NONE` | FC04 happy path per slot |
| TC-MRM-006 | FC04 read address 0x0050 (reserved) → `MB_EXC_ILLEGAL_DATA_ADDR` | FC04 reserved address |
| TC-MRM-007 | FC04 span straddling reserved address (0x002F..0x0031) → `MB_EXC_ILLEGAL_DATA_ADDR` | FC04 span check |
| TC-MRM-008 | FC04 of `TEMPERATURE` when sensor `valid == false` → response value is `0x8000` sentinel, `MB_EXC_NONE` | Sentinel handling |
| TC-MRM-009 | FC03 read each holding register → correct value, `MB_EXC_NONE` | FC03 happy path |
| TC-MRM-010 | FC03 on input register range (e.g. 0x0010) → `MB_EXC_ILLEGAL_DATA_ADDR` | Range enforcement |
| TC-MRM-011 | FC06 write valid value to `TEMP_ALARM_LOW` → validator + writer called; `IConfigManager.set_*` invoked | FC06 happy path |
| TC-MRM-012 | FC06 with out-of-range value → `MB_EXC_ILLEGAL_DATA_VALUE`, writer NOT called | Value validation |
| TC-MRM-013 | FC06 to input register → `MB_EXC_ILLEGAL_DATA_ADDR` | Access enforcement |
| TC-MRM-014 | FC16 atomic block-write across `TEMP_ALARM_LOW`..`TEMP_HYSTERESIS`, all valid → all writers called in order | FC16 happy path |
| TC-MRM-015 | FC16 where one value fails validation → ZERO writer calls (phase 2 not reached) | FC16 atomicity |
| TC-MRM-016 | FC16 spanning a reserved address → `MB_EXC_ILLEGAL_DATA_ADDR`, no writers called | FC16 span check |
| TC-MRM-017 | FC06 to `CMD_ACK_ALARM` with `0x0001` → `alarm_service.ack_all()` called; `cmd_ack_alarm_last == 0x0001` | Command happy path |
| TC-MRM-018 | FC06 to `CMD_ACK_ALARM` with `0x1234` → `MB_EXC_ILLEGAL_DATA_VALUE`; `cmd_ack_alarm_last == 0x1234` (cached anyway); `ack_all()` NOT called | Read-after-write semantics; rejection |
| TC-MRM-019 | FC03 of `CMD_ACK_ALARM` after TC-MRM-018 → returns `0x1234` | Read-after-write |
| TC-MRM-020 | FC06 to `CMD_SOFT_RESTART` with `0xA5A5` → `lifecycle_controller.handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART)` called | Magic-value happy path |
| TC-MRM-021 | FC06 to `CMD_SOFT_RESTART` with any value ≠ `0xA5A5` → `MB_EXC_ILLEGAL_DATA_VALUE`, no lifecycle call | Magic-value rejection |
| TC-MRM-022 | Slave-address change: FC06 to `MODBUS_SLAVE_ADDR` slot → both `cfg_write.set_modbus_slave_addr()` and `mb_slave.set_slave_address()` called in that order | Mediator role |
| TC-MRM-023 | `poll_stats()` called → `mb_stats.snapshot()` invoked once; `health_report_modbus()` invoked once with current and previous snapshots | Stats poll |
| TC-MRM-024 | After `CMD_RESET_METRICS` execution, `poll_stats()` does not produce a negative delta → `last_stats_snapshot` was zeroed | Reset-snapshot interaction |
| TC-MRM-025 | FC04 with `count == 0` → `MB_EXC_ILLEGAL_DATA_VALUE` | Defensive count check |
| TC-MRM-026 | FC04 with `count > MAX_REGS_PER_READ` → `MB_EXC_ILLEGAL_DATA_VALUE` | Defensive count check |
| TC-MRM-027 | All vtable functions called with NULL `ctx` → `MB_EXC_ILLEGAL_DATA_VALUE` | Defensive NULL ctx |

### 15.1 Integration tests — on target

| Test | Setup |
|---|---|
| End-to-end FC04 sensor read | Gateway polls; verify response matches `SensorService` latest |
| End-to-end FC16 config write | Gateway writes a temperature threshold block; verify `ConfigStore` persists across reboot |
| Soft-restart via Modbus | Gateway issues `CMD_SOFT_RESTART = 0xA5A5`; verify response is sent, then the FD reboots |
| Slave-address change | Gateway writes new slave address; verify FD ignores the old address and responds on the new one after the changeover frame |

---

## 16. Open items

| ID | Item | Resolution | Status |
|---|---|---|---|
| MRM-O1 | Schema-version migration when `MAP_VERSION` bumps — handled by ConfigService / ConfigStore. MRM only needs a compatibility check at init if the on-disk schema version differs from the compiled `MAP_VERSION`. Cross-reference **CS-O1**. | Validation added to init: read `MAP_VERSION` from ConfigService and log a WARN if it differs from the compiled constant; behaviour unchanged at this stage. Migration logic deferred to ConfigService. | Resolved (deferred to ConfigService) |
| MRM-O2 | FC16 block-write transaction span across config + command registers. | Confirmed: Gateway issues FC16 only across contiguous configuration ranges. Command magic-value checks remain in `write_fn`. Rule frozen in §6.4.1. | Resolved |
| MRM-O3 | Slave-address change effect timing. | Confirmed: `MODBUS_SLAVE_ADDR` write takes effect immediately; the response to the changeover FC06 goes out on the old address; the master retries on the new address. Rule frozen in §7.1. | Resolved |
| MRM-O4 | Lookup-strategy revisit if slot table grows past ~120 entries (binary search becomes worthwhile). | Linear scan retained at this size; revisit at the integration-time measurement gate. | Open (deferred — measurement gate) |
| MRM-O5 | FC16 maximum-block boundary handling at the Modbus PDU layer. | Enforced upstream by `ModbusSlave` frame-length check before reaching MRM; MRM also enforces a defensive `count > MAX_REGS_PER_WRITE → ILLEGAL_DATA_VALUE` check (TC-MRM-026 covers it). | Resolved |

---

## 17. References

- `docs/hld.md` §5.6 (FD configuration and persistence view), §8.6
  (Mediator pattern), §12 (Modbus register map overview).
- `docs/components.md` (FD application layer, `ModbusRegisterMap` entry).
- `docs/modbus-register-map.md` (HLD Artefact #7 — data contract).
- `docs/state-machines.md` Machines 5 (FD lifecycle) and 6 (Modbus Slave).
- `docs/sequence-diagrams.md` SD-02 (Modbus polling cycle).
- `docs/task-breakdown.md` §4 (FD task structure, IPC primitives).
- `docs/lld/companions/modbus-slave.md` (Middleware companion — defines
  the `IModbusSlave` and `IModbusSlaveStats` interfaces consumed here).
- `docs/lld/companions/config-service.md` (defines `IConfigProvider` /
  `IConfigManager` consumed here).
- `docs/lld/companions/health-monitor.md` (defines `IHealthSnapshot` /
  `IHealthReport` and the typed update functions).
- `docs/architecture-principles.md` (P2 DIP, P3 ISP, P7 pull, P8
  infrastructure traces to pain).
