# LLD Companion — ModbusSlave

**Layer:** Middleware  
**Board:** Field Device (FD) only  
**Provides:** `IModbusSlave`, `IModbusSlaveStats`  
**Consumes:** `IModbusUart` (ModbusUartDriver), `IModbusRegisterMap` *(DIP — see §3)*, `ILogger`  
**SRS traces:** REQ-MB-000, MB-010, MB-020, MB-030, MB-040, MB-080, MB-090, MB-100, MB-0E1  
**HLD ref:** `components.md` §Middleware — ModbusSlave; `state-machines.md` Machine 6; `hld.md` §7.7; `modbus-register-map.md` §2–§7
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** ModbusSlave in `components.md` (FD middleware layer)

---

## 1. Sources

ModbusSlave implements the Modbus RTU slave protocol on the Field Device.
It receives frames from the RS-485 bus via `ModbusUartDriver`, validates
them, dispatches to `IModbusRegisterMap`, builds responses, and transmits
them back.

**Reactive machine — no timers, no retries, no link-state tracking.**
Those concerns belong entirely to the master side. A bad frame is silently
dropped; the slave returns to Idle ready for the next. This asymmetry is
deliberate and documented in `state-machines.md` §Machine 6.

ModbusSlave is a passive singleton — it has no thread of its own. It runs
inside `ModbusTask` (FD), notified by `ModbusUartDriver` on frame completion.

---

## 2. Protocol parameters

All values sourced from `modbus-register-map.md` §2 and the SRS.

| Parameter | Value |
|-----------|-------|
| Baud rate | 9600 bps |
| Frame format | 8N1 |
| Inter-frame silence | 3.5 character times ≈ 3.65 ms at 9600 8N1 |
| Max frame size | 256 bytes (Modbus RTU maximum) |
| Supported FCs | 03, 04, 06, 16 |
| Default slave address | 1 (configurable, REQ-MB-100) |
| CRC algorithm | CRC-16/IBM (poly 0x8005, init 0xFFFF, reflect in/out) |

---

## 3. DIP interface — `IModbusRegisterMap`

ModbusSlave (Middleware) must not call `ModbusRegisterMap` (Application)
directly — this would violate P1 strict directional layering. The
`IModbusRegisterMap` interface is declared in the Application layer and
injected into `modbus_slave_init()` as a vtable pointer (P2 DIP).

```c
/* imodbus_register_map.h  — owned by Application layer */

typedef modbus_slave_err_t (*fn_read_input_reg_t)(uint16_t addr,
                                                   uint16_t *value_out);
typedef modbus_slave_err_t (*fn_read_holding_reg_t)(uint16_t addr,
                                                     uint16_t *value_out);
typedef modbus_slave_err_t (*fn_write_holding_reg_t)(uint16_t addr,
                                                      uint16_t value);

typedef struct {
    fn_read_input_reg_t   read_input;    /* FC04 dispatch */
    fn_write_holding_reg_t write_holding; /* FC06 / FC16 dispatch */
    fn_read_holding_reg_t  read_holding;  /* FC03 dispatch */
} IModbusRegisterMap;
```

ModbusSlave stores the injected pointer in its static state and calls
through it during `ProcessingRequest`. The concrete binding
(`modbus_register_map_read_input`, etc.) is set up in `LifecycleController`
during Init.

---

## 4. Data types

```c
/* modbus_slave.h */

typedef enum {
    MODBUS_SLAVE_ERR_OK            = 0,
    MODBUS_SLAVE_ERR_NOT_INIT      = 1,
    MODBUS_SLAVE_ERR_NULL_ARG      = 2,
    MODBUS_SLAVE_ERR_INVALID_ADDR  = 3,  /* maps to exception 0x02 */
    MODBUS_SLAVE_ERR_INVALID_VALUE = 4,  /* maps to exception 0x03 */
    MODBUS_SLAVE_ERR_DEVICE_FAIL   = 5,  /* maps to exception 0x04 */
} modbus_slave_err_t;

typedef struct {
    uint32_t valid_frames;          /* frames received with correct address + CRC */
    uint32_t crc_errors;            /* frames discarded due to CRC mismatch */
    uint32_t address_mismatches;    /* frames discarded — not our address */
    uint32_t exception_responses;   /* responses sent carrying an exception code */
    uint32_t unsupported_fc;        /* exception 0x01 triggered */
    uint32_t successful_responses;  /* normal (non-exception) responses sent */
} modbus_slave_stats_t;
```

---

## 2. Public API

### 5.1 `IModbusSlave`

```c
/**
 * @brief  Initialise ModbusSlave.
 *
 * Registers the frame-complete callback on ModbusUartDriver. ModbusTask
 * must be created before this call so the callback has a valid task handle
 * to notify.
 *
 * @param  reg_map     Injected IModbusRegisterMap vtable (DIP binding).
 * @param  slave_addr  Initial slave address (1..247, REQ-MB-100).
 * @param  task_handle FreeRTOS task handle for direct-to-task notification.
 * @return MODBUS_SLAVE_ERR_OK or MODBUS_SLAVE_ERR_NULL_ARG.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
modbus_slave_err_t modbus_slave_init(const IModbusRegisterMap *reg_map,
                                     uint8_t                   slave_addr,
                                     TaskHandle_t              task_handle);

/**
 * @brief  Process one received frame.
 *
 * Called from ModbusTask after receiving a direct-to-task notification from
 * the frame-complete callback. Executes the full state machine cycle:
 * address filter → CRC check → FC dispatch → build response → transmit.
 *
 * Blocking: waits for tx_complete notification before returning.
 *
 * @return MODBUS_SLAVE_ERR_OK (silent drop is not an error at this level).
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
modbus_slave_err_t modbus_slave_process(void);

/**
 * @brief  Update the slave address filter.
 *
 * Called when the Modbus address is changed via ConfigService
 * (cross-machine event `modbus_address_changed` — state-machines.md I2).
 * Thread-safe.
 *
 * @param  new_addr  New slave address (1..247).
 * @return MODBUS_SLAVE_ERR_OK or MODBUS_SLAVE_ERR_INVALID_ADDR.
 */
modbus_slave_err_t modbus_slave_set_address(uint8_t new_addr);
```

### 5.2 `IModbusSlaveStats`

```c
/**
 * @brief  Copy the current stats snapshot.
 *
 * Polled by ModbusRegisterMap to expose reliability counters via Modbus
 * (Metric Producer Pattern — components.md §Metric Producer Pattern).
 * Thread-safe (atomic copy under critical section).
 *
 * @param[out] stats_out  Filled with counters snapshot.
 * @return MODBUS_SLAVE_ERR_OK or MODBUS_SLAVE_ERR_NULL_ARG.
 */
modbus_slave_err_t modbus_slave_get_stats(modbus_slave_stats_t *stats_out);

/**
 * @brief  Reset all counters to zero.
 *
 * Triggered by CMD_RESET_METRICS register write (REQ-LD-070).
 * @return MODBUS_SLAVE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
modbus_slave_err_t modbus_slave_reset_stats(void);
```

---

## 6. Frame buffers and memory

No dynamic allocation. Both buffers are static module-level arrays, sized
to the Modbus RTU maximum.

```c
/* modbus_slave.c — static module state */

#define MODBUS_MAX_FRAME_BYTES  256U

typedef struct {
    bool                  initialised;
    uint8_t               slave_addr;
    uint8_t               rx_buf[MODBUS_MAX_FRAME_BYTES];
    uint16_t              rx_len;
    uint8_t               tx_buf[MODBUS_MAX_FRAME_BYTES];
    uint16_t              tx_len;
    const IModbusRegisterMap *reg_map;
    TaskHandle_t          task_handle;
    modbus_slave_stats_t  stats;
} ModbusSlaveState;

static ModbusSlaveState s_slave;
```

---

## 7. CRC-16 implementation

Modbus uses CRC-16/IBM: polynomial 0x8005, initial value 0xFFFF, input and
output bytes reflected.

Use a 256-entry `uint16_t` lookup table stored in flash (`const` in ROM
segment). At 9600 8N1 with a maximum 256-byte frame, the table approach
completes in under 10 µs on an 80 MHz Cortex-M4 — well within the
inter-frame budget before the master times out.

```c
/* modbus_crc.h */
uint16_t modbus_crc16(const uint8_t *buf, uint16_t len);

/* modbus_crc.c */
static const uint16_t s_crc_table[256] = { /* 256 × uint16_t = 512 B flash */ };

uint16_t modbus_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    while (len--) {
        crc = (crc >> 8U) ^ s_crc_table[(crc ^ *buf++) & 0xFFU];
    }
    return crc;
}
```

CRC bytes are appended **low byte first** per Modbus RTU convention.

---

## 3. Internal design — FC dispatch logic

The dispatch sequence inside `modbus_slave_process()` follows the
processing order specified in `state-machines.md` Machine 6:

```
1. Address filter: frame[0] == slave_addr? No → silent drop (stats.address_mismatches++).
2. CRC check: modbus_crc16(rx_buf, rx_len - 2) == rx_buf[rx_len-2 .. rx_len-1]?
   No → silent drop (stats.crc_errors++).
3. stats.valid_frames++.
4. FC dispatch:
   FC 0x03 → build_read_holding_response()
   FC 0x04 → build_read_input_response()
   FC 0x06 → build_write_single_response()
   FC 0x10 → build_write_multiple_response()
   other   → build_exception_response(0x01)  (stats.unsupported_fc++)
5. If response built → transmit (Responding state) → stats.successful_responses++
                        or stats.exception_responses++.
   If silent drop   → return without transmitting.
```

**FC16 (Write Multiple Registers):** validate that the byte count field
equals `register_count × 2`. Mismatch → exception 0x03.

**Exception 0x03 on destructive commands:** `CMD_SOFT_RESTART` (address
0x0202) requires write value `0xA5A5`; any other value → exception 0x03
(`modbus-register-map.md` §6).

All calls into `IModbusRegisterMap` that return `MODBUS_SLAVE_ERR_INVALID_ADDR`
map to exception 0x02; `MODBUS_SLAVE_ERR_INVALID_VALUE` maps to exception 0x03;
`MODBUS_SLAVE_ERR_DEVICE_FAIL` maps to exception 0x04.

---

### Principles applied

- **P1 (Strict directional layering).** Depends on IModbusUart (driver layer) and IModbusRegisterMap (DIP — interface injected at init); Logger is a cross-cutting exception (P4).
- **P2 (Dependency Inversion).** Exposes `imodbus_slave_t` vtable; consumes IModbusRegisterMap via P2 inversion — the interface is owned by the application layer and injected into this middleware component at init, so no application header is included.
- **P3 (Interface Segregation).** `IModbusSlave` (protocol execution) and `IModbusSlaveStats` (statistics read-back) are separate interfaces because LifecycleController manages the slave lifecycle while HealthMonitor reads stats — distinct, non-overlapping consumer sets.
- **P4 (Cross-cutting concern exception).** Logger referenced concretely per the cross-cutting exception.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static Modbus ADU buffer; stats counter struct statically allocated; no heap.
- **P6 (Responsibility traces to requirements).** FC01/02/03/04/05/06/15/16 dispatch traces to REQ-MB-000-040/080-100 slave function-code requirements.
- **P8 (Total error propagation, no silent failures).** All public functions return `modbus_slave_err_t`; invalid FC generates exception response 01; I/O errors propagated.
- **P9 (BARR-C coding standard).** Modbus addresses `uint16_t`; exception codes `uint8_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `modbus_slave_`; interfaces `IModbusSlave` -> `imodbus_slave_t`, `IModbusSlaveStats` -> `imodbus_slave_stats_t`; errors `MODBUS_SLAVE_ERR_*`.


## 9. RS-485 direction control

Direction control (RE/DE pin) is owned by `ModbusUartDriver`, not
ModbusSlave. The slave calls:

```c
modbus_uart_transmit(tx_buf, tx_len); /* enables TX driver, transmits, disables TX driver */
```

`modbus_uart_transmit()` is responsible for:
1. Asserting the DE/RE line (TX enable).
2. Transmitting all bytes via UART.
3. Waiting for the last byte to shift out (TC flag, not TXE).
4. De-asserting DE/RE (RX enable).
5. Notifying `ModbusTask` via `tx_complete` callback.

This keeps RS-485 timing concerns in the driver, where they belong (P1).

---

## 10. Frame-complete notification path

```
UART RX ISR   → captures byte into rx_buf; resets inter-frame timer
Timer ISR     → inter-frame silence expired → xTaskNotifyFromISR(task_handle)
ModbusTask    → wakes; calls modbus_slave_process()
```

The inter-frame silence timer is a hardware timer configured by
`ModbusUartDriver` (not ModbusSlave). ModbusSlave only reacts to the
`frame_complete` notification; it does not manage the timer itself. This
is consistent with the project convention: "ISRs perform only acknowledge /
capture / notify" (`hld.md` §14.1).

---

## 11. Init ordering and boot sequence

```
modbus_uart_driver_init()     ← driver layer up
[ModbusTask created]          ← task handle valid
modbus_slave_init(...)        ← callback registered; task_handle stored
```

No two-phase init in the Logger/ExtiDriver sense — ModbusSlave does not
register an ISR itself. The UART ISR is owned by `ModbusUartDriver`. The
only ordering constraint is that `ModbusTask` exists before
`modbus_slave_init()` so the stored task handle is valid.

---

## 12. Thread safety

`modbus_slave_process()` and `modbus_slave_set_address()` both run in
`ModbusTask` context — no concurrent access between them.

`modbus_slave_get_stats()` and `modbus_slave_reset_stats()` may be called
from `SensorTask` (via ModbusRegisterMap) while `ModbusTask` is updating
the counters. Guard with `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()`
around the counter increments and the stats copy — the critical section is
short (a handful of counter reads or a `memcpy`), so the overhead is
acceptable.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

Error codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns.

## 7. Unit-test plan

```c
#ifdef UNIT_TEST

/* Replace ModbusUartDriver with a loopback stub */
#define modbus_uart_transmit(buf, len)  stub_uart_tx(buf, len)
#define modbus_uart_receive(buf, len)   stub_uart_rx(buf, len)

#endif
```

Minimum test cases:
- Address mismatch → silent drop, `address_mismatches` incremented.
- CRC error → silent drop, `crc_errors` incremented.
- FC04 read valid address → correct response frame built.
- FC04 read reserved address → exception 0x02 response.
- FC06 write valid address + valid value → register write called, ACK response.
- FC06 write valid address + invalid value → exception 0x03.
- FC06 write `CMD_SOFT_RESTART` with `0xA5A5` → accepted.
- FC06 write `CMD_SOFT_RESTART` with wrong value → exception 0x03.
- Unsupported FC (e.g. 0x01) → exception 0x01, `unsupported_fc` incremented.
- `modbus_slave_reset_stats()` → all counters zero.
- CRC bytes appended low-byte-first in response frame.

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| MBS-O1  | FC16 spanning address boundaries — confirm behaviour when the requested register range crosses a category boundary (e.g. 0x01FE–0x0201 spanning config into commands). Current decision: return exception 0x02 if any address in the range is invalid. Validate against `modbus-register-map.md` §4 during implementation. | Validate boundary behaviour against modbus-register-map.md §4 at implementation | Open |
| MBS-O2  | `modbus_uart_transmit()` TC-flag wait strategy — confirm whether `ModbusUartDriver` uses DMA + TC interrupt or polling. Affects whether `tx_complete` notification comes from an ISR or inline. Resolved at `ModbusUartDriver` LLD companion. | Resolve at ModbusUartDriver LLD companion — DMA vs polling TC-flag strategy | Open |
