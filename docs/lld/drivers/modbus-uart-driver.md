# ModbusUartDriver — LLD Companion

**Document:** `docs/lld/drivers/modbus-uart-driver.md`
**Version:** 0.1 (draft — pending Luca review)
**Board scope:** Field Device (STM32F469) and Gateway (B-L475E-IOT01A)
**Layer:** Driver
**Status:** Draft
**Date:** May 2026

**HLD anchor:** ModbusUartDriver in `components.md` (FD + GW driver layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Sends and receives byte streams over the UART configured for the Modbus RS-485 link | `components.md` |
| PROVIDES (upward) | `IModbusUart` | `components.md` |
| USES (downward) | CMSIS | `components.md` |
| Root requirement | REQ-MB-030 | `SRS.md` §2.5 |
| Hardware | UARTx + RS-485 transceiver (MBUART-O1 — see §8) | `components.md` hardware sweep |

**Consumers:**

| Board | Consumer | Task context |
|---|---|---|
| Field Device | `ModbusSlave` (middleware) | `ModbusSlaveTask` (priority 4) |
| Gateway | `ModbusMaster` (middleware) | `ModbusPollerTask` (priority 4) |

Both are single consumers on their respective boards — one task owns the driver exclusively.

**ISR contract (task-breakdown.md §6.1):**
- FD: `USART_modbus_IRQHandler` → `ModbusSlaveTask` → `«notify»`
- GW: `USART_modbus_IRQHandler` → `ModbusPollerTask` → `«notify»` (two distinct notification bits: RX done, RX error)

**Sequence diagram surface:** SD-02 messages 3–10 traverse this driver. TX is synchronous (step 3 "sync, write frame" and step 8 "sync, write response frame"); RX is asynchronous (steps 5 and 10 "async, frame received").

**Protocol parameters (SRS §2.5, modbus-register-map.md §2):**

| Parameter | Value |
|---|---|
| Baud rate | 9600 bps |
| Frame format | 8 data bits, no parity, 1 stop bit (8N1) |
| Physical layer | RS-485 half-duplex |
| Inter-frame silence | 3.5 character times ≈ 3.65 ms at 9600 8N1 |
| Maximum frame length | 256 bytes (Modbus RTU ADU) |

---

## 2. Public API

### 2.1 Dependency-conformance check

`modbus_uart_driver.h` includes only CMSIS device headers and `stdint.h`. No FreeRTOS headers, no `gpio_driver.h`. The RS-485 DE pin is driven by the UART peripheral itself in hardware DE mode (§3.3); no GpioDriver dependency is introduced. Confirmed clean.

### 2.2 P3 consideration

`ModbusMaster` (GW) and `ModbusSlave` (FD) are both the sole consumer on their respective board. The TX/RX split is asymmetric per transaction role (master always initiates TX; slave always initiates RX), but the driver interface is symmetric — both operations are exposed on both boards. No ISP split is warranted; the interface is already narrow.

### 2.3 Two-phase init

The ISR callback requires the consuming task to exist before it can post a notification. `modbus_uart_init()` configures the peripheral and enables TX; `modbus_uart_attach_rx()` registers the callback and enables the RX interrupt. `modbus_uart_attach_rx()` is called from the consuming task during its startup prologue, after `vTaskStartScheduler()`. This is the established two-phase pattern from `debug-uart-driver.md`.

### 2.4 Data types

```c
/**
 * @brief Error codes returned by ModbusUartDriver operations.
 *
 * Naming follows the cross-cutting convention in lld.md §3.2.
 */
typedef enum {
    MODBUS_UART_ERR_OK      = 0, /**< Operation succeeded. */
    MODBUS_UART_ERR_TIMEOUT = 1, /**< TXE or TC flag did not assert within timeout. */
    MODBUS_UART_ERR_BUSY    = 2, /**< Transmit called while a TX is already in progress. */
} modbus_uart_err_t;

/**
 * @brief Events delivered to the RX callback from the ISR.
 *
 * Two distinct events correspond to the two notification bits documented
 * in task-breakdown.md §5.4.
 */
typedef enum {
    MODBUS_UART_EVENT_RX_DONE  = 0, /**< IDLE detected; complete frame in buffer. */
    MODBUS_UART_EVENT_RX_ERROR = 1, /**< Overrun, framing error, or noise error. */
} modbus_uart_event_t;

/**
 * @brief RX callback type registered by the consumer.
 *
 * Called from ISR context. The consumer must not call any FreeRTOS API
 * that is not ISR-safe. Typically the consumer calls xTaskNotifyFromISR()
 * to wake its owning task, then reads the frame from the buffer at task
 * level via modbus_uart_get_rx_frame().
 *
 * @param event    What triggered the callback (RX_DONE or RX_ERROR).
 * @param context  Opaque pointer registered at attach time (typically the
 *                 task handle or a middleware context struct).
 */
typedef void (*modbus_uart_rx_cb_t)(modbus_uart_event_t event, void *context);
```

### 2.5 Public API (`modbus_uart_driver.h`)

```c
/**
 * @brief Initialise the Modbus UART peripheral.
 *
 * Configures TX and RX pins as alternate-function outputs.
 * Configures the UART for 9600 8N1 (REQ-MB-030).
 * Enables hardware RS-485 DE mode on the RTS pin (§3.3).
 * Enables TX and the UART peripheral. Does NOT enable the RX interrupt
 * (that is done in modbus_uart_attach_rx, after the consumer task exists).
 *
 * Must be called once from main() before the FreeRTOS scheduler starts.
 *
 * @return MODBUS_UART_ERR_OK on success.
 */
modbus_uart_err_t modbus_uart_init(void);

/**
 * @brief Register the RX callback and enable the RX interrupt.
 *
 * Must be called from the consuming task's startup prologue
 * (after the scheduler has started and the task exists).
 * Must be called exactly once. Calling again overwrites the prior
 * registration without disabling the interrupt first — do not call
 * more than once in normal operation.
 *
 * @param callback  Function to call from the ISR on RX_DONE or RX_ERROR.
 *                  Must not be NULL.
 * @param context   Opaque pointer passed unchanged to the callback.
 */
void modbus_uart_attach_rx(modbus_uart_rx_cb_t callback, void *context);

/**
 * @brief Transmit a Modbus RTU frame over RS-485.
 *
 * Asserts DE via hardware RS-485 mode, transmits all bytes, waits for
 * Transmission Complete (TC) before returning so the caller may safely
 * turn around to receive. De-assertion of DE is handled automatically
 * by the hardware after TC.
 *
 * Blocks in the calling task until transmission is complete.
 * Caller serialises calls (ModbusMaster / ModbusSlave each call this
 * from a single task, so no concurrency concern in practice).
 *
 * @param frame  Pointer to frame bytes (must not be NULL).
 * @param len    Number of bytes to transmit (1..256).
 * @return MODBUS_UART_ERR_OK on success; MODBUS_UART_ERR_TIMEOUT if
 *         TXE or TC does not assert within the timeout; MODBUS_UART_ERR_BUSY
 *         if a prior transmit is still in progress (should not occur in
 *         normal single-task operation).
 */
modbus_uart_err_t modbus_uart_transmit(const uint8_t *frame, uint16_t len);

/**
 * @brief Copy the most recently received frame from the internal buffer.
 *
 * Must be called from task context (not ISR context) after the RX_DONE
 * callback has fired. The internal buffer is not protected by a mutex;
 * the caller must not call this while an RX is in progress (guaranteed
 * by the single-task consumer model).
 *
 * @param[out] buf      Destination buffer (must be at least 256 bytes).
 * @param[out] len      Number of bytes in the received frame.
 * @return MODBUS_UART_ERR_OK on success.
 */
modbus_uart_err_t modbus_uart_get_rx_frame(uint8_t *buf, uint16_t *len);
```

---

## 3. Internal design

### 3.1 Module-level state

```c
/* Static RX buffer — maximum Modbus RTU ADU is 256 bytes. No heap use. */
static uint8_t             s_rx_buf[256];
static volatile uint16_t   s_rx_len     = 0;
static modbus_uart_rx_cb_t s_rx_cb      = NULL;
static void               *s_rx_ctx     = NULL;
static volatile bool       s_tx_busy    = false;
```

`s_rx_len` and `s_tx_busy` are `volatile` because they are written in ISR context and read in task context. No additional synchronisation primitive is needed — the single-consumer, single-ISR model guarantees that the task reads `s_rx_len` only after the callback has fired, by which point the ISR has already written it.

### 3.2 RX path — IDLE Line interrupt

Frame-end detection uses the UART **IDLE Line interrupt** (IDLEIE in CR1, IDLE flag in ISR/SR). The hardware sets IDLE after the RX line has been idle for one complete character time (10 bit periods at 8N1 ≈ 1.04 ms) following the last received byte.

**ISR sequence:**
```
USART_modbus_IRQHandler:
  if RXNE set:
    s_rx_buf[s_rx_len++] = UART_DR / RXDR   (store received byte)
    if s_rx_len == 256: set RX_ERROR, discard further bytes
    clear RXNE (implicit on L4 by reading RXDR; explicit on F4)
  if IDLE set:
    clear IDLE flag (write 1 to ICR.IDLECF on L4; read SR then DR on F4)
    call s_rx_cb(MODBUS_UART_EVENT_RX_DONE, s_rx_ctx)
  if (ORE or FE or NE) set:
    clear error flags
    call s_rx_cb(MODBUS_UART_EVENT_RX_ERROR, s_rx_ctx)
    reset s_rx_len = 0
```

The IDLE interrupt fires at ~1.04 ms of bus silence. At 9600 baud, the strict Modbus RTU inter-frame minimum is 3.65 ms. Using IDLE is a deliberate deviation (MBUART-D2 — §9): on a controlled single-master bus at 9600 baud, the actual gap between response and next request is always many tens of milliseconds (limited by the master's 200 ms timeout + software overhead), so the stricter timing is irrelevant in practice.

**F469 vs L475 IDLE clear sequence:** On F4 (legacy USART), clearing IDLE requires a read of SR followed by a read of DR — a two-step sequence that may inadvertently consume the first byte of the next frame if not done carefully. On L4 (improved USART), IDLE is cleared by writing `1` to `ICR.IDLECF`, which is atomic and safe. The ISR implementations differ on this point (MBUART-O2 — §8).

### 3.3 RS-485 DE — hardware mode

Both F469 and L475 UARTs support a hardware RS-485 Driver Enable mode. When enabled:

- `CR3.DEM = 1` activates DE mode.
- The RTS pin (configured as alternate function) is automatically asserted at the start of transmission and de-asserted after TC (Transmission Complete).
- The de-assertion occurs after the stop bit of the last byte has been clocked out — guaranteed before any bus turnaround.
- No software GPIO toggling. No risk of truncating the last bit by de-asserting too early.

This keeps `ModbusUartDriver USES CMSIS` clean — no GpioDriver dependency for DE/RE management.

Optional `DEAT` and `DEDT` registers (driver enable assertion/de-assertion time, measured in sample clock units) can add guard time around DE transitions. For 9600 baud, the default (zero guard time) is acceptable; the hardware transition is faster than one bit period.

### 3.4 TX path — synchronous polling

```
modbus_uart_transmit:
  set s_tx_busy = true
  (hardware DE asserts automatically on first TXE write)
  for each byte in frame:
    poll TXE until set   → MODBUS_UART_ERR_TIMEOUT if expired
    write byte to DR / TDR
  poll TC until set       → MODBUS_UART_ERR_TIMEOUT if expired
  (hardware DE de-asserts automatically after TC)
  set s_tx_busy = false
  return MODBUS_UART_ERR_OK
```

The TC wait at the end is mandatory for RS-485 half-duplex. TXE indicates the shift register has accepted the byte from the data register, not that it has been clocked out onto the wire. Only TC guarantees the last bit is on the bus and DE can safely de-assert. Hardware RS-485 mode handles the de-assertion automatically on TC, but the function must still wait for TC before returning so the caller (ModbusMaster/ModbusSlave) does not begin listening before the bus is released.

### 3.5 Timeout values

TX polling uses a generous timeout: at 9600 baud, transmitting 256 bytes takes 256 × 1.04 ms ≈ 266 ms. The per-byte TXE timeout is set to 5 ms (5× one character time, accommodating clock inaccuracies). TC timeout is set to 10 ms after the last byte.

---

### 3.6 Principles applied

- **P1 (Strict directional layering).** Depends only on CMSIS UART/DMA peripheral headers; no RTOS task dependencies in the ISR path.
- **P2 (Dependency Inversion).** Exposes `imodbus_uart_t` vtable; ModbusSlave (FD) and ModbusMaster (GW) depend on the interface.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static RX/TX buffers sized at compile time; one DMA descriptor; no heap.
- **P6 (Responsibility traces to requirements).** IDLE-line RX and RS-485 DE control trace to REQ-MB-010/020 frame-exchange requirements.
- **P8 (Total error propagation, no silent failures).** `modbus_uart_err_t` on all calls; framing errors set a sticky flag returned on the next receive.
- **P9 (BARR-C coding standard).** Buffer sizes `uint16_t`; DMA count register `uint16_t`; no implicit widening.
- **P10 (Naming conventions).** Prefix `modbus_uart_`; interface `IModbusUart` -> `imodbus_uart_t`; errors `MODBUS_UART_ERR_*`.


## 4. Hardware contract

### 4.1 Peripheral identification (open item — MBUART-O1)

The specific UART peripheral used for Modbus RS-485 on each board is not yet confirmed. It must be verified against the board schematics and the RS-485 transceiver connection. Tracked as **MBUART-O1** (§8).

Candidate constraints:
- Must have an RTS pin routable as DE (all STM32 USART peripherals with RS-485 support meet this).
- Must not conflict with the debug UART (UART1 on both boards per the hardware sweep).

### 4.2 Baud rate register value

At 9600 bps, `BRR = f_PCLK / 9600`. The exact value depends on the APB clock feeding the selected UART, which is unresolved pending `clock-config.md`. Tracked as **MBUART-O2** (§8), sharing the same root dependency as DUART-O2, I2CD-O1/O2, and SPID-O1.

At 9600 baud the tolerance is generous — even a ±5% PCLK error gives well under ±2% baud error, within RS-485 margin. The open item is for documentation completeness, not functional risk.

### 4.3 Pin configuration

TX and RX pins: alternate-function push-pull, no pull-up/down.
RTS pin (used as DE): alternate-function push-pull, configured automatically by `CR3.DEM = 1`.

Exact pin assignments: confirmed at implementation once MBUART-O1 is resolved.

### 4.4 IDLE clear sequence — F469 caution

On the STM32F469 (F4 USART), clearing the IDLE flag requires reading SR then reading DR. If a new byte arrives between the SR read and the DR read, that byte is silently discarded. At 9600 baud this is extremely unlikely (the ISR must execute within 1.04 ms to be at risk), but the implementation must follow the exact sequence in RM0386 §30.8 and consider disabling RXNE interrupt briefly during the clear sequence. Verify at implementation. Tracked as **MBUART-O3** (§8).

---

## 5. Sequence integration

ModbusUartDriver appears as explicit lifelines in **SD-02** (Modbus polling cycle, `sequence-diagrams.md` §7):

| SD-02 step | Action | Driver call |
|---|---|---|
| 3 | ModbusMaster → ModbusUartDriver (GW): sync, write frame | `modbus_uart_transmit(frame, len)` |
| 4 | ModbusUartDriver (GW) → ModbusUartDriver (FD): async (bus) | bus propagation — not a driver call |
| 5 | ModbusUartDriver (FD) → ModbusSlave: async, frame received | ISR callback → `xTaskNotifyFromISR` → ModbusSlaveTask calls `modbus_uart_get_rx_frame()` |
| 8 | ModbusSlave → ModbusUartDriver (FD): sync, write response | `modbus_uart_transmit(frame, len)` |
| 10 | ModbusUartDriver (GW) → ModbusMaster: async, frame received | ISR callback → `xTaskNotifyFromISR` → ModbusPollerTask calls `modbus_uart_get_rx_frame()` |

No SD changes required. The existing SD-02 messages accurately describe the driver's behaviour.

---

## 6. Error and fault behaviour

### 6.1 TX errors

`modbus_uart_transmit` returns `MODBUS_UART_ERR_TIMEOUT` if TXE or TC does not assert. The consumer (ModbusMaster / ModbusSlave) logs the event via `ILogger` and reports via `IHealthReport`. For ModbusMaster, a TX timeout starts the retry countdown (REQ-MB-060). For ModbusSlave, a TX timeout is reported but no retry is initiated (the master will time out and retry).

### 6.2 RX errors

On `MODBUS_UART_EVENT_RX_ERROR`, the consumer discards the partial frame and waits for the next RX_DONE event. Error counters are maintained by the middleware layer (ModbusMaster / ModbusSlave stats interfaces), not by the driver. The driver's only obligation is to clear the error flags, reset `s_rx_len`, and invoke the callback.

### 6.3 Buffer overrun

If `s_rx_len` reaches 256 (maximum ADU size) before IDLE fires, the driver switches to discard mode (receives but does not store further bytes) and invokes the callback with `MODBUS_UART_EVENT_RX_ERROR` when IDLE is eventually detected. This prevents buffer corruption.

---

## 7. Unit-test plan

Host-platform tests (Unity framework). The USART peripheral is mocked via `#define USARTx (&mock_usart)` per target. The ISR is tested by calling `USART_modbus_IRQHandler()` directly with the mock register bank in predetermined states.

| ID | Test case | Expected result |
|---|---|---|
| T-MBUART-01 | `modbus_uart_init`: verify 9600 8N1, DE mode enabled (DEM=1), TX enabled | CR1: UE=1, TE=1, 8-bit; CR3: DEM=1; BRR = expected value (MBUART-O2 TBD) |
| T-MBUART-02 | `modbus_uart_attach_rx`: verify RXNEIE and IDLEIE enabled in CR1 | CR1.RXNEIE=1, CR1.IDLEIE=1; callback pointer stored |
| T-MBUART-03 | `modbus_uart_transmit` happy path: 8-byte frame | Each byte written to DR/TDR after TXE set; TC wait issued; function returns OK |
| T-MBUART-04 | `modbus_uart_transmit` TXE timeout: TXE never asserts | Returns MODBUS_UART_ERR_TIMEOUT |
| T-MBUART-05 | `modbus_uart_transmit` TC timeout: all bytes sent, TC never asserts | Returns MODBUS_UART_ERR_TIMEOUT |
| T-MBUART-06 | ISR — RXNE only: receive 4 bytes | s_rx_buf populated, s_rx_len = 4, callback not yet called |
| T-MBUART-07 | ISR — IDLE after 4 bytes received | Callback called with MODBUS_UART_EVENT_RX_DONE; `modbus_uart_get_rx_frame` returns 4 bytes |
| T-MBUART-08 | ISR — ORE (overrun) flag set | Callback called with MODBUS_UART_EVENT_RX_ERROR; s_rx_len reset to 0 |
| T-MBUART-09 | ISR — buffer overrun (257 bytes before IDLE) | 256 bytes stored; 257th discarded; IDLE triggers RX_ERROR callback |
| T-MBUART-10 | `modbus_uart_get_rx_frame` after RX_DONE | Correct bytes returned; length matches |
| T-MBUART-11 | `modbus_uart_transmit` BUSY guard | Second call while s_tx_busy=true returns MODBUS_UART_ERR_BUSY |

Test files: `tests/drivers/test_modbus_uart_driver_fd.c` and `tests/drivers/test_modbus_uart_driver_gw.c`.

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| MBUART-O1 | Identify the specific UART peripheral used for Modbus RS-485 on each board (must not conflict with debug UART on UART1). Verify against board schematics and RS-485 transceiver connection. | Luca | Check schematics at implementation |
| MBUART-O2 | BRR register value for 9600 bps. Depends on PCLK feeding the selected UART. Resolve when `clock-config.md` lands. | Luca | Resolve with DUART-O2, I2CD-O1/O2, SPID-O1 |
| MBUART-O3 | F469 IDLE flag clear sequence (SR read → DR read). Verify against RM0386 §30.8 that no byte is lost in the two-step clear. Consider disabling RXNEIE briefly if risk is real. | Luca | Verify at implementation; document in code comment |

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| MBUART-D1 | IDLE Line interrupt for frame-end detection | Built into both UART peripherals; no separate timer needed; reliable at 9600 baud where real turnaround times are far larger than 3.5 character times |
| MBUART-D2 | IDLE interrupt fires at ~1.04 ms, not the strict 3.5-character-time (3.65 ms) Modbus RTU minimum | Acceptable deviation: the single-master bus has no scenario where two frames arrive within 3.5 character times at 9600 baud. Documented here and in code for traceability |
| MBUART-D3 | Hardware RS-485 DE mode (CR3.DEM, RTS pin as DE) | Eliminates GpioDriver dependency; DE timing is hardware-guaranteed; no risk of premature bus release |
| MBUART-D4 | TC wait in `modbus_uart_transmit` before returning | Mandatory for RS-485 half-duplex: TXE signals the shift register has accepted a byte, not that it has been transmitted. Only TC guarantees the last bit has left the wire |
| MBUART-D5 | Single callback with event enum, not two separate callbacks | One attach function, one registration point; the event enum carries sufficient information for the consumer to dispatch |
| MBUART-D6 | Static 256-byte RX buffer in module state | REQ-NF-408 prohibits dynamic memory after init; 256 bytes is the maximum Modbus RTU ADU size; the buffer is always present, no allocation needed |
| MBUART-D7 | `modbus_uart_get_rx_frame` copies to caller buffer | Decouples the internal buffer from the consumer; the consumer can process the frame at its own pace; the buffer is ready for the next frame immediately after copy |
| MBUART-D8 | Singleton module (no handle) | One Modbus UART per board; consistent with all prior driver companions |
