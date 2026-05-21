# DebugUartDriver — LLD Companion

**Version:** 1.0
**Date:** May 2026
**Status:** Draft

**HLD anchor:** `DebugUartDriver` in `components.md` (Field Device §4 driver layer, line 168; Gateway §4 driver layer, line 426). Layer: Driver. Targets: STM32F469 Discovery (Field Device, USART3) and STM32L475 IoT Discovery (Gateway, USART1).

---

## 1. Sources

### 1.1 Component specification

The driver appears identically in `components.md` for both boards:

> **NAME:** DebugUartDriver
> **LAYER:** Driver
> **RESPONSIBILITY:** Sends and receives byte streams over the UART peripheral connected to the debug console (REQ-LI-010).
> **PROVIDES (upward):** `IDebugUart`
> **USES (downward):** CMSIS

Note: the responsibility says "byte streams", but the runtime contract in `task-breakdown.md` requires line-buffered RX (see §1.5). This companion implements line buffering inside the driver — the byte-stream characterisation in `components.md` refers to the TX surface and to the underlying wire transport, not to the RX API exposed to ConsoleTask.

### 1.2 Consumers

| Board | Consumer | Layer | Use | Reference |
|---|---|---|---|---|
| Field Device | Logger | Middleware | TX (log output) | `components.md` line 246 |
| Field Device | ConsoleService | Application | TX + RX | `components.md` line 308 |
| Gateway | Logger | Middleware | TX | `components.md` line 516 |
| Gateway | ConsoleService | Application | TX + RX | `components.md` line 610 |

Logger is documented as a "bootstrap exception" (`components.md` §1 preamble, line 58): it depends directly on `DebugUartDriver` and `RtcDriver` rather than going through `TimeProvider`, because `TimeProvider` itself logs through Logger. The implication for this companion is that the TX path must be operational before the FreeRTOS scheduler starts.

### 1.3 Traceability

`components.md` cites REQ-LI-010 in the responsibility line. The driver also implements REQ-LI-000 implicitly — REQ-LI-000 specifies the line parameters that the driver realises in hardware.

| ID | Text | SRS source | Use case |
|---|---|---|---|
| REQ-LI-010 | "The system shall receive a diagnostic command [command list TBD] and execute it." | SRS line 163 | UC-04 (SRS matrix line 462) |
| REQ-LI-000 | "The system shall provide a serial console interface accessible to the Field Technician. The connection parameters are: Baud Rate 115200, Data bits 8, Parity bit No Parity, Stop bits 1." | SRS line 158 | UC-04 |

REQ-LI-000 governs the wire-level configuration and is therefore the binding requirement for §4 (hardware contract).

### 1.4 HLD context

`hld.md` §9 places DebugUartDriver in the CMSIS register-level category. Both boards' user manuals confirm the routing of the chosen USART peripheral to the ST-LINK V2-1 virtual COM port:

- **F469 Discovery (Field Device).** UM1932 §4.11 (page 16): *"The serial interface USART3 is directly available as a virtual COM port of the PC connected to the ST-LINK/V2-1 USB connector CN1. The virtual COM port settings are configured as: 115200 b/s, 8 bits data, no parity, 1 stop bit, no flow control."*
- **L475 IoT Discovery (Gateway).** UM2153 Figure 22 (page 42) shows signals `ST-LINK-UART1_RX` and `ST-LINK-UART1_TX` routing from the STM32L475 through the ST-LINK MCU (STM32F103CBT6). Confirmed by §7.3 (page 13).

Both boards therefore yield a USB virtual COM port on the host PC when connected via the ST-LINK USB cable — no external USB-to-UART adapter is required.

### 1.5 Runtime context

**Sequence diagrams (`sequence-diagrams.md`).** `DebugUartDriver` does not appear directly as a lifeline. ConsoleService appears in SD-12 (Field-technician provisioning, line 1482 onward); the wire-level transport beneath ConsoleService is implicit and abstracted. This is the same pattern as GPIO: bus drivers are correctly hidden beneath their consumers in SDs.

**State machines (`state-machines.md`).** No occurrences. The driver has no behavioural state of its own beyond a simple initialised / RX-attached flag.

**Task breakdown (`task-breakdown.md` lines 213 and 305).** Identical on both boards:

> `DebugUartDriver` ISR → `ConsoleTask` | `«notify»` | Single-event "line received"

This is the binding statement that determines the RX API: the driver accumulates incoming bytes into a line buffer and signals the consumer once a complete line is ready. ConsoleTask does not see individual bytes. The signalling mechanism is a function-pointer callback (see §2) — the consumer wires the callback to its own threading primitive (typically `xTaskNotifyFromISR()` in the FreeRTOS case).

The ISR-to-task contract is documented further in `task-breakdown.md` §6 (lines 312–321): acknowledge, capture, notify, return.

### 1.6 Hardware references

- **STM32F469 (Field Device, USART3).** RM0386 USART chapter for register definitions. UM1932 §4.11 (virtual COM routing). MB1189 board schematic for the specific USART3 MCU pin assignment — not in project knowledge; PB10 (TX) / PB11 (RX) is the default F4 USART3 mapping (AF7) and is the assignment used by the F469 Discovery.
- **STM32L475 (Gateway, USART1).** RM0351 USART chapter for register definitions. UM2153 §7.3 (ST-LINK V2-1) and Figure 22 (page 42) for routing. Standard B-L475E-IOT01A USART1 mapping: PB6 (TX, AF7) and PB7 (RX, AF7).
- **CMSIS device headers** (in project knowledge). `stm32f469xx.h` line 958: `USART_TypeDef` layout with `SR`, `DR`, `BRR`, `CR1`–`CR3`, `GTPR`. `stm32l475xx.h` line 935: `USART_TypeDef` layout with `CR1`–`CR3`, `BRR`, `GTPR`, `RTOR`, `RQR`, `ISR`, `ICR`, `RDR`, `TDR`. **The two register sets are not source-compatible** — the F4 and L4 USART peripherals are different generations.

This last point is the determining factor for §3.1: the driver has one header and **two implementation files**, one per board, selected by build target.

---

## 2. Public API

### 2.1 API style

Following the same rationale as the GPIO companion: there is exactly one realisation of `IDebugUart` per board, with no run-time polymorphism. A direct C API on a flat module is sufficient; no opaque handle, no vtable, no instance management. The header `debug_uart_driver.h` is shared between both boards; the implementation is selected at build time.

**The driver does not depend on FreeRTOS.** `components.md` lists `USES (downward): CMSIS` — only CMSIS. This is honoured strictly: the header includes no FreeRTOS files; the implementation uses no FreeRTOS primitives. Two consequences:

- **RX notification is a function-pointer callback invoked from ISR context.** The consumer's callback knows about FreeRTOS and may call `xTaskNotifyFromISR()` from inside it; the driver does not.
- **TX is not internally serialised.** Concurrent calls from multiple contexts produce interleaved output. The caller (Logger, in the multi-producer case) holds its own mutex.

Initialisation is split in two phases (see §2.3): TX-ready first, RX-attached later. This accommodates Logger's bootstrap exception — Logger may need to log before the consumer of RX has been wired.

### 2.2 Header content

```c
/**
 * @file debug_uart_driver.h
 * @brief CMSIS-level debug-UART driver — line-buffered RX, blocking TX.
 *
 * Provides IDebugUart (per components.md): TX of arbitrary byte streams
 * and ISR-driven line-ready notification for RX. Used by Logger (TX)
 * and ConsoleService (TX + RX) on both boards.
 *
 * The driver depends only on CMSIS. It does NOT depend on FreeRTOS or
 * any other RTOS. The consumer wires the RX line-ready callback to its
 * own threading primitives (e.g., xTaskNotifyFromISR()).
 *
 * Thread safety: the driver is NOT internally serialised. Concurrent
 * calls to debug_uart_send() from multiple contexts will interleave
 * bytes on the wire. The caller (typically Logger) must serialise
 * itself if multiple producers exist.
 *
 * @note Realised on USART3 (Field Device) or USART1 (Gateway), each routed
 *       to the board's ST-LINK V2-1 virtual COM port.
 * @note See docs/lld/drivers/debug-uart-driver.md for the full design.
 */

#ifndef DEBUG_UART_DRIVER_H
#define DEBUG_UART_DRIVER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Configuration constants                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Maximum length of a single received line, in bytes.
 *
 * Lines longer than this are truncated; the truncation is reported via
 * the line-flag output of debug_uart_read_line().
 */
#define DEBUG_UART_LINE_MAX_LEN  (128U)

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Debug-UART driver result codes.
 */
typedef enum
{
    DEBUG_UART_OK                      =  0, /**< Success. */
    DEBUG_UART_ERR_NOT_INITIALISED     =  1, /**< debug_uart_init() not yet called. */
    DEBUG_UART_ERR_NULL_POINTER        =  2, /**< Required pointer is NULL. */
    DEBUG_UART_ERR_INVALID_PARAM       =  3, /**< Out-of-range parameter. */
    DEBUG_UART_ERR_TX_TIMEOUT          =  4, /**< Peripheral TXE flag did not assert within timeout. */
    DEBUG_UART_ERR_NO_LINE_AVAILABLE   =  5, /**< debug_uart_read_line() called with nothing pending. */
    DEBUG_UART_ERR_RX_ALREADY_ATTACHED =  6  /**< debug_uart_attach_rx() called twice. */
} debug_uart_err_t;

/* ------------------------------------------------------------------ */
/* Line completion flag                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Reported alongside each line read.
 */
typedef enum
{
    DEBUG_UART_LINE_OK        = 0, /**< Line fitted within DEBUG_UART_LINE_MAX_LEN. */
    DEBUG_UART_LINE_TRUNCATED = 1  /**< Line exceeded DEBUG_UART_LINE_MAX_LEN; tail was dropped. */
} debug_uart_line_flag_t;

/* ------------------------------------------------------------------ */
/* RX callback                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Signature of the RX line-ready callback.
 *
 * Invoked from the USART RX ISR each time a complete line has been
 * accumulated. The callback runs in interrupt context with the USART
 * vector active; it must follow the ISR contract from
 * task-breakdown.md §6 (acknowledge, capture, notify, return).
 *
 * Typical FreeRTOS wiring:
 * @code
 *   static TaskHandle_t s_console_task;
 *   static void on_line_ready(void *ctx)
 *   {
 *       BaseType_t woken = pdFALSE;
 *       xTaskNotifyFromISR(s_console_task, (1U << CONSOLE_LINE_BIT),
 *                          eSetBits, &woken);
 *       portYIELD_FROM_ISR(woken);
 *   }
 * @endcode
 *
 * The driver does not call portYIELD_FROM_ISR() itself — that is a
 * FreeRTOS concern and belongs in the callback.
 *
 * @param[in] context Opaque pointer registered with debug_uart_attach_rx().
 */
typedef void (*debug_uart_line_callback_t)(void *context);

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the driver (phase 1 — TX-ready).
 *
 * Enables peripheral clocks (USART and the GPIO port hosting the TX/RX
 * pins), configures the TX/RX pins for alternate-function mode at the
 * board-specific alternate-function number, sets the USART for 115200,
 * 8N1, no flow control (REQ-LI-000), and arms the peripheral for TX.
 * RX interrupts are not enabled yet.
 *
 * Must be called once before any other function. After this call returns
 * OK, debug_uart_send() is callable; debug_uart_read_line() is not.
 *
 * @return DEBUG_UART_OK on success.
 *
 * @note Threading: not internally serialised. Caller must ensure this
 *       function is called exactly once and not concurrently with any
 *       other API entry point.
 */
debug_uart_err_t debug_uart_init(void);

/**
 * @brief Attach an RX line-ready callback (phase 2).
 *
 * Stores the callback and context, enables the RX-not-empty interrupt,
 * and unmasks the USART NVIC vector. From this point on, each complete
 * line received causes @c callback to be invoked from ISR context, with
 * @c context as its argument.
 *
 * Calling twice returns DEBUG_UART_ERR_RX_ALREADY_ATTACHED.
 *
 * @param[in] callback Function invoked on each complete line. Must not be NULL.
 *                     Runs in ISR context — see debug_uart_line_callback_t.
 * @param[in] context  Opaque pointer passed back to @c callback. May be NULL.
 *
 * @return DEBUG_UART_OK on success; DEBUG_UART_ERR_NOT_INITIALISED,
 *         DEBUG_UART_ERR_NULL_POINTER, or DEBUG_UART_ERR_RX_ALREADY_ATTACHED.
 *
 * @note Threading: not internally serialised. Caller calls once.
 */
debug_uart_err_t debug_uart_attach_rx(debug_uart_line_callback_t callback,
                                      void *context);

/**
 * @brief Send a buffer of bytes synchronously.
 *
 * Writes each byte to the USART data register, polling the TX-empty flag
 * between bytes. Returns after the last byte has been pushed to the data
 * register (not after it has fully left the wire).
 *
 * NOT thread-safe. If multiple producers exist, the caller must serialise.
 * The typical case: Logger holds its own mutex and is the only caller in
 * the multi-task path; ConsoleService is single-task.
 *
 * @param[in] data       Buffer of bytes to send. Must be non-NULL when length > 0.
 * @param[in] length     Number of bytes; 0 is permitted and returns OK immediately.
 * @param[in] timeout_ms Maximum time to wait for the TXE flag, per byte,
 *                       in milliseconds. Used to detect a wedged peripheral.
 *
 * @return DEBUG_UART_OK on success; DEBUG_UART_ERR_NOT_INITIALISED,
 *         DEBUG_UART_ERR_NULL_POINTER, or DEBUG_UART_ERR_TX_TIMEOUT.
 *
 * @note Threading: task-context only. Blocks during the byte loop for the
 *       duration of the transmission (~87 µs per byte at 115200 bps).
 *       NOT internally serialised. NOT ISR-safe.
 */
debug_uart_err_t debug_uart_send(const uint8_t *data,
                                 size_t length,
                                 uint32_t timeout_ms);

/**
 * @brief Read the most recent complete line.
 *
 * Copies the latest accumulated line into the caller's buffer and
 * null-terminates it. The line terminator characters (\r and \n) are
 * stripped before copying. Resets the internal line-ready flag so that
 * the next callback invocation corresponds to a new line.
 *
 * @param[out] out_buf    Caller-provided buffer; receives the line.
 * @param[in]  buf_size   Size of @c out_buf in bytes. Must be at least
 *                        DEBUG_UART_LINE_MAX_LEN + 1 (for the null
 *                        terminator).
 * @param[out] out_length Number of bytes written, excluding the null.
 *                        May be 0 (an empty line).
 * @param[out] out_flag   DEBUG_UART_LINE_OK or DEBUG_UART_LINE_TRUNCATED.
 *
 * @return DEBUG_UART_OK on success; DEBUG_UART_ERR_NOT_INITIALISED,
 *         DEBUG_UART_ERR_NULL_POINTER, DEBUG_UART_ERR_INVALID_PARAM
 *         (buf_size too small), or DEBUG_UART_ERR_NO_LINE_AVAILABLE.
 *
 * @note Threading: task-context only. Typically called by the consumer
 *       task after its callback has notified it. Briefly disables the
 *       USART NVIC vector while copying.
 */
debug_uart_err_t debug_uart_read_line(uint8_t *out_buf,
                                      size_t buf_size,
                                      size_t *out_length,
                                      debug_uart_line_flag_t *out_flag);

#endif /* DEBUG_UART_DRIVER_H */
```

### 2.3 API design rationale

The unusual choices deserve note:

- **No FreeRTOS dependency.** Strictly honours `components.md` `USES (downward): CMSIS` and P1. The driver is portable to any threading model — bare-metal, ThreadX, Zephyr — without source changes. The RX callback is the seam: consumers wire it to whatever notification primitive their RTOS provides.

- **Two-phase init.** `debug_uart_init()` brings the TX path up first — Logger needs it before the FreeRTOS scheduler starts. `debug_uart_attach_rx()` activates RX once the consumer's callback is wired (after its task has been created). A single combined init would either force the consumer task to exist very early (breaking natural creation order) or delay Logger's first usable output.

- **Caller serialises TX.** The driver writes bytes to a register; concurrent writes interleave. Logger holds its own mutex (Logger is middleware, free to use FreeRTOS). ConsoleService is single-task. Each caller serialises itself by construction. Internal mutex would (a) require FreeRTOS, (b) duplicate Logger's mutex, (c) couple this driver to one specific RTOS.

- **Blocking TX, no mutex timeout.** Sending 80 chars at 115200 bps takes ~7 ms — acceptable for a debug interface. The `timeout_ms` parameter bounds the wait for each byte's TXE flag (peripheral-wedge detection), not contention.

- **Single line buffer, single-line read.** Lines are accumulated into one buffer; on EOL the line is frozen and the callback fires. If a second line arrives before the consumer reads the first, the second overwrites — favoured simplicity over historical preservation. A ring of completed lines is a possible v2 refinement if multi-line buffering becomes necessary (it is not, for a single human typing).

- **Line terminator stripping.** `\r`, `\n`, and `\r\n` are all accepted; terminators stripped before exposing to the consumer.

- **ISR-context callback.** The callback runs with the USART interrupt active and must follow the ISR contract from `task-breakdown.md` §6. The driver's header documents this contract and provides a wiring example in the Doxygen for `debug_uart_line_callback_t`.

- **No `debug_uart_recv_byte()` raw API.** The runtime contract explicitly says "line received". Bypassing the line buffering would invite consumers to break this contract.

---

## 3. Internal design

### 3.0 Private struct

```c
typedef struct {
    bool                        initialised;           /**< Set by debug_uart_init(). */
    bool                        rx_attached;           /**< Set by debug_uart_attach_rx(). */
    debug_uart_line_callback_t  line_callback;         /**< ISR invokes on line-complete. */
    void                       *line_callback_context; /**< Caller context for the callback. */
    uint8_t                     rx_accum_buf[DEBUG_UART_LINE_MAX_LEN]; /**< ISR accumulation buffer. */
    volatile size_t             rx_accum_len;          /**< Bytes currently in accum_buf. */
    volatile bool               rx_overflow;           /**< Set if accum_buf filled before EOL. */
    uint8_t                     rx_ready_buf[DEBUG_UART_LINE_MAX_LEN + 1U]; /**< Frozen line for caller. */
    volatile size_t             rx_ready_len;          /**< Length of frozen line. */
    volatile bool               rx_ready_truncated;    /**< Set if frozen line was truncated. */
    volatile bool               rx_ready_flag;         /**< Set when a frozen line awaits collection. */
} debug_uart_driver_t;

static debug_uart_driver_t s_debug_uart;
```


### 3.1 Module state

Static storage, declared in the `.c` file:

| Symbol | Type | Purpose |
|---|---|---|
| `s_initialised` | `static bool` | Set true by `debug_uart_init()`. |
| `s_rx_attached` | `static bool` | Set true by `debug_uart_attach_rx()`. |
| `s_line_callback` | `static debug_uart_line_callback_t` | Invoked from ISR when a line is ready. |
| `s_line_callback_context` | `static void *` | Opaque context passed to the callback. |
| `s_rx_accum_buf` | `static uint8_t[DEBUG_UART_LINE_MAX_LEN]` | ISR accumulates the in-progress line. |
| `s_rx_accum_len` | `static volatile size_t` | Bytes currently in the accumulating buffer. |
| `s_rx_overflow` | `static volatile bool` | Set if accumulating buffer fills before EOL. |
| `s_rx_ready_buf` | `static uint8_t[DEBUG_UART_LINE_MAX_LEN + 1U]` | Frozen line waiting to be read; null-terminated. |
| `s_rx_ready_len` | `static volatile size_t` | Length of frozen line. |
| `s_rx_ready_truncated` | `static volatile bool` | Set if the frozen line was truncated. |
| `s_rx_ready_flag` | `static volatile bool` | Set when a frozen line is available; cleared after read. |

No FreeRTOS handles, no mutex. Two buffers (accumulating and ready) implement a simple double-buffer. The ISR writes to the accumulating buffer; on EOL it transposes to the ready buffer (or replaces it if a previous unread line is still there).

### 3.2 Per-function internal flow

### debug_uart_init

**`debug_uart_init()`**

1. If `s_initialised` is already true, return `DEBUG_UART_OK` (idempotent).
2. Enable peripheral clocks via RCC: the USART instance and the GPIO port hosting TX/RX (`RCC->AHB1ENR` on F469 for GPIOB, `RCC->APB1ENR` for USART3; `RCC->AHB2ENR` on L475 for GPIOB, `RCC->APB2ENR` for USART1).
3. Configure TX and RX pins:
   - Set `MODER` bits for the pins to "alternate function" (0b10).
   - Set `AFR[1]` bits for the pins to AF7 (USART3 on F469; USART1 on L475 — see §4 for board-specific pin numbers and alt-function bits).
   - Set `OSPEEDR` to high speed; `OTYPER` push-pull (default); `PUPDR` pull-up (idle line is high).
4. Configure the USART peripheral:
   - **F469 path:** Disable USART (`CR1.UE = 0` for clean config). Set baud rate in `BRR` from PCLK1 (assumed 45 MHz post-clock-tree-config); compute `USARTDIV = PCLK1 / (16 * 115200) = 24.4140625`, encode as fractional. Set `CR1`: word length 8 (M=0), parity disabled (PCE=0). Set `CR2`: 1 stop bit (STOP=00). Set `CR3`: no flow control (CTSE=0, RTSE=0). Enable TX (`CR1.TE=1`). Enable USART (`CR1.UE=1`).
   - **L475 path:** Disable USART (`CR1.UE = 0`). Set baud rate in `BRR` from PCLK2 (assumed 80 MHz); compute `USARTDIV = PCLK2 / 115200`, write to `BRR` as integer. Set `CR1`: word length 8 (M0=M1=0), parity disabled. Set `CR2`: 1 stop bit. Set `CR3`: no flow control. Enable TX (`CR1.TE=1`). Enable USART (`CR1.UE=1`).
5. Set `s_initialised = true`.
6. Return `DEBUG_UART_OK`.

No FreeRTOS objects are created here — there are none.

**`debug_uart_attach_rx(callback, context)`**

1. Reject if `s_initialised` is false → `DEBUG_UART_ERR_NOT_INITIALISED`.
2. Reject if `callback` is NULL → `DEBUG_UART_ERR_NULL_POINTER`.
3. Reject if `s_rx_attached` is true → `DEBUG_UART_ERR_RX_ALREADY_ATTACHED`.
4. Store `s_line_callback = callback`, `s_line_callback_context = context`.
5. Reset RX state: `s_rx_accum_len = 0`, `s_rx_ready_flag = false`, `s_rx_overflow = false`.
6. Enable RX in the peripheral: set `CR1.RE = 1` and `CR1.RXNEIE = 1`.
7. Set NVIC priority for the USART vector to a value compatible with `configMAX_SYSCALL_INTERRUPT_PRIORITY` (FreeRTOS-safe range — the priority *value* is set via CMSIS; the constraint comes from the consumer's RTOS, not from this driver). Then enable the vector via `NVIC_EnableIRQ()`.
8. Set `s_rx_attached = true`.
9. Return `DEBUG_UART_OK`.

**`debug_uart_send(data, length, timeout_ms)`**

1. Reject if `s_initialised` is false → `DEBUG_UART_ERR_NOT_INITIALISED`.
2. If `length == 0`, return `DEBUG_UART_OK` (no-op).
3. Reject if `data` is NULL → `DEBUG_UART_ERR_NULL_POINTER`.
4. For each byte in `data[0..length-1]`:
   a. Poll the TX-empty flag (`SR.TXE` on F469, `ISR.TXE` on L475) until set, or until `timeout_ms` has elapsed (measured by a free-running timer counter, since the driver cannot call into FreeRTOS tick APIs).
   b. If the timeout elapses, return `DEBUG_UART_ERR_TX_TIMEOUT`. The peripheral is left in whatever state it reached — caller decides whether to reset or continue.
   c. Write the byte to the data register (`DR` on F469, `TDR` on L475).
5. Return `DEBUG_UART_OK`.

Implementation note for the timeout: a portable, FreeRTOS-free approach is to use the Cortex-M SysTick or DWT cycle counter. Both are CMSIS-accessible. The implementation file selects one and converts `timeout_ms` to ticks/cycles at compile time using the known CPU clock.

**`debug_uart_read_line(out_buf, buf_size, out_length, out_flag)`**

1. Reject if `s_initialised` is false, any pointer is NULL, or `buf_size < DEBUG_UART_LINE_MAX_LEN + 1U`.
2. Disable the USART NVIC vector (`NVIC_DisableIRQ`) briefly to prevent the ISR from re-arming a new line while we read.
3. If `s_rx_ready_flag == false`, re-enable the vector and return `DEBUG_UART_ERR_NO_LINE_AVAILABLE`.
4. Copy `s_rx_ready_buf[0..s_rx_ready_len]` into `out_buf`. Write `out_buf[s_rx_ready_len] = '\0'`.
5. Write `*out_length = s_rx_ready_len`.
6. Write `*out_flag = s_rx_ready_truncated ? DEBUG_UART_LINE_TRUNCATED : DEBUG_UART_LINE_OK`.
7. Clear `s_rx_ready_flag = false`, `s_rx_ready_truncated = false`.
8. Re-enable the USART NVIC vector.
9. Return `DEBUG_UART_OK`.

**RX ISR — `USARTx_IRQHandler()`**

1. Read the status register (`SR` on F469 or `ISR` on L475) once into a local variable.
2. If a framing, parity, noise, or overrun error is flagged, clear it (`SR` read followed by `DR` read on F469; `ICR` write on L475), drop the byte, and exit. Increment an error counter for observability.
3. If RXNE is set:
   a. Read the byte from the data register (`DR` on F469, `RDR` on L475).
   b. If the byte is `\r` or `\n`:
      - If `s_rx_accum_len > 0`, freeze the line: copy `s_rx_accum_buf` to `s_rx_ready_buf`, set `s_rx_ready_len = s_rx_accum_len`, set `s_rx_ready_truncated = s_rx_overflow`, set `s_rx_ready_flag = true`, then reset `s_rx_accum_len = 0` and `s_rx_overflow = false`. Invoke `s_line_callback(s_line_callback_context)`.
      - Empty lines (just `\r\n`) are not delivered — they are treated as a continuation of the previous terminator and the callback is not invoked.
   c. Else if `s_rx_accum_len < DEBUG_UART_LINE_MAX_LEN`:
      - Append the byte: `s_rx_accum_buf[s_rx_accum_len++] = byte`.
   d. Else (buffer full):
      - Set `s_rx_overflow = true`. Drop the byte. Continue accumulating until EOL; truncation is reported on the eventual line read.

The driver does not call any FreeRTOS function from the ISR. If the callback wires to `xTaskNotifyFromISR()` and `portYIELD_FROM_ISR()`, that happens inside the callback, in the consumer's code.

### 3.3 Synchronisation

The driver is **not internally serialised**. The thread-safety contract is documented in the header and summarised here:

- **TX path.** `debug_uart_send()` writes bytes to a peripheral register. Concurrent calls from multiple contexts would interleave. The caller is responsible for ensuring single-call-at-a-time. Logger holds its own mutex around its calls; ConsoleService is single-task. If a future consumer breaks this contract, the failure is the consumer's, not the driver's.
- **RX path.** The accumulating buffer and `s_rx_accum_len` are written only from the ISR. The ready buffer is written by the ISR (on freeze) and read by `debug_uart_read_line()` in task context. The race window is the freeze-then-read.
- **Read protection.** `debug_uart_read_line()` disables the USART NVIC vector via `NVIC_DisableIRQ()` around the copy, and re-enables it via `NVIC_EnableIRQ()` afterwards. Both are CMSIS, not FreeRTOS. The disabled window is short (a memcpy of ≤ 128 bytes) and bounded.
- **NVIC vector masking, not global IRQ disable.** Narrower than `__disable_irq()`, which would mask every interrupt in the system. Other ISRs (Modbus, timers, LCD) continue to run during the brief read window.
- **`volatile`** on the RX state ensures the compiler does not assume task-context reads see a stale value after an ISR write.

The driver has no mutex, no semaphore, no queue, no task notification. Every concurrency primitive lives above this layer.

### 3.4 Principles applied

- **P1 (Strict directional layering).** USES CMSIS only, per `components.md`. **No FreeRTOS dependency** — the driver compiles and runs against any threading model (bare-metal, ThreadX, Zephyr, FreeRTOS) without source changes. The seam is the RX callback: consumers wire it to whatever notification primitive their RTOS provides. The driver configures its own pins via direct GPIO register access; it does not call `gpio_configure_pin()`. This follows the bus-driver convention seen across `components.md` (UART, I2C, SPI all USES CMSIS only; consumers like LedDriver and sensor drivers USES GpioDriver because they own pins outside their primary peripheral).
- **P2 (Dependency Inversion).** Logger and ConsoleService both call `IDebugUart` through this header; neither depends on the implementation file. The two implementation files are interchangeable to the consumer. Additionally, the RX callback inverts the dependency for line notification — the driver depends on a function-pointer interface, not on the consumer's task structure.
- **P5 (Bounded resources, no dynamic allocation post-init).** All buffers are static. No FreeRTOS objects (none are required).
- **P6 (Responsibility traces to requirements).** REQ-LI-010 (cited in `components.md`) and REQ-LI-000 (line parameters) both trace through this companion.
- **P10 (Naming conventions).** Module prefix `debug_uart_`; types `_t`-suffixed; the interface name `IDebugUart` in `components.md` maps 1:1 to this header.

**Principles considered:**

- **P3 (Interface Segregation).** Considered splitting `IDebugUart` into `IDebugUartTx` (used by Logger) and `IDebugUartRx` (used by ConsoleService). Rejected: ConsoleService uses both halves, so the split would not segregate two consumer groups — it would split one consumer's surface, which is not the principle's intent. Logger and ConsoleService have different needs (TX-only vs TX+RX), but the cost of two interface types for one provider here outweighs the benefit.
- **P7 (Pull-based access).** Considered for the RX path — the consumer could poll. Rejected: `task-breakdown.md` explicitly specifies notification-driven RX. Polling at console-line rates (human typing) would burn cycles for no gain.

---

## 4. Hardware contract

### 4.1 Peripheral selection

| Aspect | Field Device (F469) | Gateway (L475) |
|---|---|---|
| USART instance | USART3 | USART1 |
| Routing | ST-LINK V2-1 virtual COM (UM1932 §4.11) | ST-LINK V2-1 virtual COM (UM2153 §7.3, Figure 22) |
| Bus | APB1 | APB2 |
| Peripheral clock-enable register | `RCC->APB1ENR` | `RCC->APB2ENR` |
| Peripheral clock-enable bit | `USART3EN` (bit 18) | `USART1EN` (bit 14) |
| Device-header pointer (CMSIS) | `USART3` | `USART1` |
| Reference manual | RM0386, USART chapter | RM0351, USART chapter |
| `USART_TypeDef` layout | `stm32f469xx.h` line 958 | `stm32l475xx.h` line 935 |

### 4.2 Pin assignment

| Aspect | Field Device | Gateway |
|---|---|---|
| TX pin | PB10 *(verify against MB1189 schematic; standard F4 USART3 default)* | PB6 (UM2153 Figure 22) |
| RX pin | PB11 *(verify against MB1189 schematic)* | PB7 (UM2153 Figure 22) |
| GPIO port clock-enable register | `RCC->AHB1ENR` | `RCC->AHB2ENR` |
| GPIO port clock-enable bit | `GPIOBEN` (bit 1) | `GPIOBEN` (bit 1) |
| Alternate function | AF7 (USART3 on STM32F4 family) | AF7 (USART1 on STM32L4 family) |
| `MODER` bits | `MODER10`, `MODER11` → 0b10 (alternate) | `MODER6`, `MODER7` → 0b10 |
| `AFR` register and bits | `AFR[1]` (i.e., AFRH); bits 8..11 for AF10, 12..15 for AF11; each set to 0b0111 | `AFR[0]` (AFRL); bits 24..27 for AF6, 28..31 for AF7; each set to 0b0111 |
| `PUPDR` | Pull-up (0b01) on both pins | Pull-up (0b01) on both pins |
| `OSPEEDR` | High speed (0b10) | High speed (0b10) |
| `OTYPER` | Push-pull (0) | Push-pull (0) |

### 4.3 USART registers used

**F469 (USART3, legacy F4 USART)** — `USART_TypeDef` per `stm32f469xx.h`:

| Register | Use |
|---|---|
| `SR` (Status, offset 0x00) | `TXE`, `RXNE`, `ORE`, `FE`, `PE`, `NF` flags read in TX loop and RX ISR. |
| `DR` (Data, offset 0x04) | Single register for both TX (write) and RX (read). |
| `BRR` (Baud rate, offset 0x08) | Set in init from PCLK1 and 115200 target. |
| `CR1` (Control 1, offset 0x0C) | `UE`, `M`, `PCE`, `TE`, `RE`, `RXNEIE`, `TCIE`. |
| `CR2` (Control 2, offset 0x10) | `STOP` (00 = 1 stop bit). |
| `CR3` (Control 3, offset 0x14) | `RTSE`, `CTSE` cleared. |
| `GTPR`, `LCKR-equivalents` | Unused. |

**L475 (USART1, modern L4 USART)** — `USART_TypeDef` per `stm32l475xx.h`:

| Register | Use |
|---|---|
| `CR1` (offset 0x00) | `UE`, `M0`, `M1`, `PCE`, `TE`, `RE`, `RXNEIE`, `TXEIE`. |
| `CR2` (offset 0x04) | `STOP`. |
| `CR3` (offset 0x08) | `RTSE`, `CTSE` cleared. |
| `BRR` (offset 0x0C) | Set in init from PCLK2 and 115200 target. |
| `ISR` (offset 0x1C) | `TXE`, `RXNE`, `ORE`, `FE`, `PE`, `NE` flags. |
| `ICR` (offset 0x20) | Error-flag clears (`ORECF`, `FECF`, `PECF`, `NECF`). |
| `RDR` (offset 0x24) | Read RX data. |
| `TDR` (offset 0x28) | Write TX data. |
| `GTPR`, `RTOR`, `RQR` | Unused. |

### 4.4 Clock-tree dependencies

The driver assumes the system clock tree has been configured before `debug_uart_init()` is called, with PCLK1 ≈ 45 MHz on F469 and PCLK2 ≈ 80 MHz on L475. The actual PCLK values must be known to compute `BRR`. Clock-tree setup is outside this driver's scope (handled by the system-startup code or a dedicated `clock-driver.md` companion if one is added).

### 4.5 NVIC

| Aspect | Field Device | Gateway |
|---|---|---|
| Vector name | `USART3_IRQn` | `USART1_IRQn` |
| Handler symbol | `USART3_IRQHandler` | `USART1_IRQHandler` |
| Priority assignment | Numerically ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY` (FreeRTOS-safe range). Exact value to be set during integration when full NVIC priority map is established. | Same. |

TX is poll-based and uses no interrupt. Only the RX-not-empty interrupt (`RXNEIE` in `CR1`) is enabled, and only after `debug_uart_attach_rx()` is called.

---

## 5. Sequence integration

`grep -i "debug_uart\|debuguart\|idebuguart" sequence-diagrams.md` returns no matches. As with GPIO, this driver does not appear as a lifeline in any HLD sequence diagram.

ConsoleService appears in SD-12 (Field Technician provisioning, line 1482 onward). The transport beneath ConsoleService — "connect to console", "display error", "display confirmation prompt" — is implicit and abstracted at the ConsoleService level. The mapping is:

| SD-12 message (verbatim) | DebugUartDriver function (implicit) |
|---|---|
| "connect to console" | Consumer registers its callback via `debug_uart_attach_rx()` at ConsoleTask startup. |
| "set parameter (key, value)" — input from Field Technician | Each line: ISR invokes the callback; callback (in consumer code) calls `xTaskNotifyFromISR()`; ConsoleTask wakes and calls `debug_uart_read_line()`. |
| "display error" / "display confirmation prompt" / "display success" / "display 'discarded'" — output to Field Technician | Delivered via `debug_uart_send()`. |

The runtime contract from `task-breakdown.md` (line 213/305) is the binding statement and is honoured by `debug_uart_attach_rx()` + the RX ISR + consumer-side callback + `debug_uart_read_line()`. The wire (FreeRTOS task notify) is the consumer's choice; the driver guarantees only that the callback fires once per complete line.

**Consumer-side wiring (ConsoleService init, for illustration):**

```c
static TaskHandle_t s_console_task_handle;

static void console_on_line_ready(void *ctx)
{
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(s_console_task_handle,
                       (1U << CONSOLE_LINE_READY_BIT),
                       eSetBits,
                       &woken);
    portYIELD_FROM_ISR(woken);
}

void console_service_init(TaskHandle_t console_task)
{
    s_console_task_handle = console_task;
    (void)debug_uart_attach_rx(console_on_line_ready, NULL);
}
```

The FreeRTOS-aware glue lives where FreeRTOS belongs — in middleware/application. The driver remains pure C + CMSIS.

---

## 6. Error and fault behaviour

### 6.1 Error enum

Defined in §2.2. Producing conditions:

| Code | Produced by | Trigger |
|---|---|---|
| `DEBUG_UART_OK` | All | Operation succeeded. |
| `DEBUG_UART_ERR_NOT_INITIALISED` | All except `debug_uart_init` | Called before `debug_uart_init()`. |
| `DEBUG_UART_ERR_NULL_POINTER` | `send`, `read_line`, `attach_rx` | Required pointer is NULL. |
| `DEBUG_UART_ERR_INVALID_PARAM` | `read_line` | `buf_size < DEBUG_UART_LINE_MAX_LEN + 1`. |
| `DEBUG_UART_ERR_TX_TIMEOUT` | `send` | TXE flag did not assert within the supplied timeout (peripheral wedged). |
| `DEBUG_UART_ERR_NO_LINE_AVAILABLE` | `read_line` | Called when `s_rx_ready_flag` is false (no line waiting). |
| `DEBUG_UART_ERR_RX_ALREADY_ATTACHED` | `attach_rx` | Called twice. |

### 6.2 Retry policy

None at this layer. TX timeouts are surfaced to the caller; the caller decides whether to retry. The RX ISR drops bytes received during a hardware error condition (parity, framing, noise, overrun) after clearing the error flag; it does not request a retransmit (the protocol has no such mechanism). An overrun causes loss of one or more bytes from the in-progress line; the resulting line is reported via the `TRUNCATED` flag if length exceeds the buffer, but unrelated overruns that lose mid-line bytes are not detectable in v1.0 — they manifest as a malformed line that the consumer must parse or reject.

### 6.3 Downstream failure

`USES (downward): CMSIS`. Register access cannot fail at runtime. There is no downstream component whose failure the driver must handle.

### 6.4 Observability

No `IDebugUartStats` interface in v1.0. Counters worth adding when `IHealthReport` integration becomes relevant (none of the current consumers requires it):

- `rx_overrun_count` — number of `ORE` events handled.
- `rx_framing_error_count` — number of `FE` events handled.
- `rx_line_truncated_count` — number of frozen lines that were truncated.
- `tx_timeout_count` — number of `DEBUG_UART_ERR_TX_TIMEOUT` returns.

These are stubbed in the implementation but not exposed in the API for v1.0. Tracked as open item DUART-O3.

No log calls are issued from within this driver. Logger depends on this driver; logging from within it would create the same circular dependency that `components.md` §1 preamble avoids between Logger and TimeProvider. The driver is silent on success and on input-validation failures; only the caller may log.

---

## 7. Unit-test plan

### 7.1 Framework and location

- **Framework:** Unity (ThrowTheSwitch.org).
- **Files:**
  - `tests/firmware/field-device/drivers/test_debug_uart_driver.c`
  - `tests/firmware/gateway/drivers/test_debug_uart_driver.c`
- **Build target:** host (PC). Same approach as GPIO testing.

### 7.2 Mock strategy

Because the driver does not depend on FreeRTOS, the test harness needs only the CMSIS mock infrastructure already established for GPIO:

- The shared mock header `tests/mocks/stm32_cmsis_mock.h` (introduced for GPIO) is extended with mock `USART_TypeDef` instances (`USART1`, `USART3`), the relevant `RCC` enable-register bits, and the NVIC enable/disable functions (`NVIC_EnableIRQ`, `NVIC_DisableIRQ` as test-visible counters).
- The mock peripheral pointers point into static volatile storage that the tests inspect and manipulate.
- The driver source compiles unchanged. The board selection (`STM32F469xx` vs `STM32L475xx`) is set via `CFLAGS` to test either implementation.
- The RX line callback is provided by each test as a simple function that records its invocations into a test-visible struct (callback count, last context value). No FreeRTOS, no task notification.

The absence of a FreeRTOS mock is the visible benefit of the FreeRTOS-free design: the test harness for this driver is no more complex than the test harness for GPIO.

### 7.3 Test cases

For each function, at least one happy-path case and one error-path case. The ISR is tested by directly invoking the handler symbol with the mock peripheral pre-populated.

**`debug_uart_init`**

- `test_debug_uart_init_succeeds_first_call`
- `test_debug_uart_init_idempotent_second_call_returns_ok`
- `test_debug_uart_init_enables_usart_and_gpio_clocks`
- `test_debug_uart_init_configures_pin_alternate_function`
- `test_debug_uart_init_programs_baud_rate_for_pclk`

**`debug_uart_attach_rx`**

- `test_debug_uart_attach_rx_happy_path_enables_rxneie`
- `test_debug_uart_attach_rx_stores_callback_and_context`
- `test_debug_uart_attach_rx_rejects_not_initialised`
- `test_debug_uart_attach_rx_rejects_null_callback`
- `test_debug_uart_attach_rx_rejects_second_call`

**`debug_uart_send`**

- `test_debug_uart_send_zero_length_returns_ok_no_writes`
- `test_debug_uart_send_writes_each_byte_to_data_register`
- `test_debug_uart_send_polls_txe_between_bytes`
- `test_debug_uart_send_rejects_null_data_when_length_nonzero`
- `test_debug_uart_send_rejects_not_initialised`
- `test_debug_uart_send_returns_tx_timeout_when_txe_never_asserts`

**`debug_uart_read_line`**

- `test_debug_uart_read_line_returns_no_line_when_flag_clear`
- `test_debug_uart_read_line_copies_line_and_clears_flag`
- `test_debug_uart_read_line_null_terminates_buffer`
- `test_debug_uart_read_line_reports_ok_flag_when_not_truncated`
- `test_debug_uart_read_line_reports_truncated_flag_when_overflow_occurred`
- `test_debug_uart_read_line_rejects_buf_size_too_small`
- `test_debug_uart_read_line_rejects_null_pointers`
- `test_debug_uart_read_line_rejects_not_initialised`

**RX ISR (invoked directly)**

- `test_isr_accumulates_byte_into_buffer`
- `test_isr_appends_until_eol_then_invokes_callback`
- `test_isr_strips_cr_and_lf`
- `test_isr_handles_crlf_as_single_terminator`
- `test_isr_marks_overflow_when_buffer_full_no_eol`
- `test_isr_drops_byte_and_clears_error_on_ore`
- `test_isr_drops_byte_and_clears_error_on_fe`
- `test_isr_ignores_empty_line_terminator_after_full_line`
- `test_isr_invokes_callback_with_registered_context`

### 7.4 Coverage target

- 100% of public API functions exercised.
- 100% of error codes in `debug_uart_err_t` produced by at least one test case.
- ≥ 90% statement coverage in `debug_uart_driver.c`.

### 7.5 Cannot be host-tested

- Actual electrical timing (baud-rate accuracy on the wire, propagation through the ST-LINK MCU).
- Concurrency hazards between the real ISR and a real task — verified only on board.
- Behaviour under sustained 115200 bps RX traffic (the ISR's ability to keep up).

These are integration-phase concerns. Documented as not in scope for host tests.

---

## 8. Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| DUART-O1 | F469 USART3 pin assignment (PB10/PB11 assumed). MB1189 schematic is not in project knowledge; the standard F4 default is well-known but should be visually confirmed during board bring-up. | Confirm during first board test of debug output. If different, update §4.2 and the implementation `.c` file accordingly. | Open |
| DUART-O2 | PCLK values for both boards (PCLK1 = 45 MHz F469, PCLK2 = 80 MHz L475). Final clock tree decisions are made by the system-startup code (not yet specified in an LLD companion). | Resolve when a `clock-config.md` companion lands, or pin the assumed values in the implementation as named constants traceable to that future companion. | Open |
| DUART-O3 | Statistics counters (`rx_overrun_count`, `tx_timeout_count`, etc.) are accumulated internally but not exposed via an `IDebugUartStats` interface. | Add the interface when `IHealthReport` integration becomes relevant (after `HealthMonitor` LLD companion is drafted). | Open |
| DUART-O4 | `debug_uart_drain()` — block until the wire has flushed the last byte (TC flag). Not required by current consumers; useful before deliberate reset. | Add in v2 if a consumer needs it. | Open |

**Inherited TBDs from `lld.md` §5.** None of O1/O2/O3 in `lld.md` §5 is resolved by this companion. They remain open.

---

*This is the DebugUartDriver companion. It follows the eight-section methodology defined in `lld-methodology.md` and inherits the cross-cutting conventions from `lld.md` §3.*
