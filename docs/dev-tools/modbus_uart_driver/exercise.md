# Technical Exercise — ModbusUartDriver

## Brief (3 minutes)

You are implementing the receive path of a Modbus RTU driver for an STM32
microcontroller. The UART peripheral uses an IDLE Line interrupt to detect
frame boundaries: after the last byte of a frame, the bus is silent for at
least one character time (~1 ms at 9600 baud), causing the IDLE flag to assert.

Your task is to implement the ISR body that accumulates incoming bytes and, on
IDLE detection, notifies the consumer via a registered callback. The ISR must
also handle hardware error flags (overrun, framing error, noise error) and a
software buffer-overflow condition (frame longer than 256 bytes).

---

## Given files

### modbus_uart_exercise.h

```c
#ifndef MODBUS_UART_EXERCISE_H
#define MODBUS_UART_EXERCISE_H

#include <stdint.h>
#include <stdbool.h>

#define MODBUS_BUF_SIZE (256U)

typedef enum
{
    MODBUS_EVENT_RX_DONE  = 0,
    MODBUS_EVENT_RX_ERROR = 1,
} modbus_event_t;

typedef void (*modbus_rx_cb_t)(modbus_event_t event, void *context);

/* Module state — treat as opaque from the ISR's perspective. */
typedef struct
{
    uint8_t            rx_buf[MODBUS_BUF_SIZE];
    volatile uint16_t  rx_len;
    modbus_rx_cb_t     rx_cb;
    void              *rx_ctx;
} modbus_state_t;

extern modbus_state_t g_modbus;

/* UART status register flags (STM32L4 ISR layout). */
#define UART_ISR_RXNE  (1U << 5)
#define UART_ISR_IDLE  (1U << 4)
#define UART_ISR_ORE   (1U << 3)
#define UART_ISR_FE    (1U << 1)
#define UART_ISR_NE    (1U << 2)
#define UART_ERR_MASK  (UART_ISR_ORE | UART_ISR_FE | UART_ISR_NE)

/* Simulated register access (in tests, these are mock variables). */
extern volatile uint32_t UART_ISR;   /* status register (read) */
extern volatile uint32_t UART_ICR;   /* clear register  (write-1-to-clear) */
extern volatile uint32_t UART_RDR;   /* receive data register */

#define UART_ICR_IDLECF  (1U << 4)
#define UART_ICR_ORECF   (1U << 3)
#define UART_ICR_FECF    (1U << 1)
#define UART_ICR_NECF    (1U << 2)

void UART4_IRQHandler(void);

#endif /* MODBUS_UART_EXERCISE_H */
```

### modbus_uart_exercise.c (partial)

```c
#include "modbus_uart_exercise.h"

modbus_state_t g_modbus;

void UART4_IRQHandler(void)
{
    uint32_t status = UART_ISR;

    /* TODO 1: If RXNE is set, read UART_RDR into the receive buffer.
     *         If the buffer is full (rx_len == MODBUS_BUF_SIZE), discard
     *         the byte (do not write past the end of rx_buf). */

    /* TODO 2: If IDLE is set:
     *         - Clear the IDLE flag by writing UART_ICR_IDLECF to UART_ICR.
     *         - If rx_len > MODBUS_BUF_SIZE (overflow condition: the 257th
     *           byte arrived before IDLE), call the callback with RX_ERROR
     *           and reset rx_len to 0.
     *         - Otherwise, call the callback with RX_DONE. */

    /* TODO 3: If any error flag (ORE, FE, NE) is set:
     *         - Clear all three flags via UART_ICR.
     *         - Reset rx_len to 0.
     *         - Call the callback with RX_ERROR. */
}
```

---

## Questions

**Q1:** On the STM32F4 (legacy USART), clearing the IDLE flag requires reading
SR *then* reading DR — a two-step sequence. On the STM32L4 you write `1` to
`ICR.IDLECF`. Why does the F4 two-step carry a data-loss risk that the L4 does
not, and why is the risk accepted at 9600 baud?

**Answer:** On the F4, reading SR primes the clear and reading DR completes it.
If a new byte arrives between the SR read and the DR read, that byte is consumed
by the DR read and silently discarded — the RXNE interrupt for it never fires.
On the L4, `ICR.IDLECF` is a dedicated write-to-clear bit with no coupling to
the data register, so no byte can be lost. At 9600 baud, one character takes
~1.04 ms; the ISR executes the two-step in nanoseconds, giving a < 1 µs window
for byte loss. On a half-duplex RS-485 bus where the IDLE gap between frames is
always many milliseconds (limited by the master's response timeout), a byte
arriving in that nanosecond window is practically impossible.

**Q2:** The callback is called from ISR context. What constraint does this place
on the consumer (ModbusSlave / ModbusMaster task), and what is the idiomatic
FreeRTOS pattern for bridging ISR context to task context?

**Answer:** The consumer must not call any FreeRTOS API that is not ISR-safe from
within the callback. Specifically, it must not call `xQueueSend`, `xSemaphoreGive`,
or `xTaskNotify` — only the `FromISR` variants. The idiomatic pattern is:
`xTaskNotifyFromISR(task_handle, notification_bits, eSetBits, &higher_priority_woken)`
followed by `portYIELD_FROM_ISR(higher_priority_woken)` at the end of the ISR.
The task then calls `modbus_uart_get_rx_frame()` at task level to retrieve the
buffered frame after waking.

**Q3:** Why does the driver reset `rx_len = 0` inside the RX_ERROR path (ORE/FE/NE)
but also keep `rx_len` valid inside the RX_DONE path? What would break if
`rx_len` were NOT reset after an ORE error?

**Answer:** An ORE (overrun) error means the hardware lost bytes — the data in
`rx_buf` is incomplete and cannot represent a valid Modbus frame. Resetting
`rx_len` discards the corrupt partial frame and readies the buffer for the next
complete frame. In the RX_DONE path, `rx_len` holds the valid byte count that
the consumer needs to pass to `modbus_uart_get_rx_frame()`. If `rx_len` were not
reset after ORE, the next RX_DONE IDLE interrupt would report the correct number
of new bytes *plus* the leftover count from the corrupt frame, causing the consumer
to believe it received a longer (and thus also corrupt) frame — leading to failed
CRC checks and unnecessary Modbus retries, or worse, misinterpreted register values.

---

## Model solution

```c
void UART4_IRQHandler(void)
{
    uint32_t status = UART_ISR;

    /* RXNE: store incoming byte. */
    if (status & UART_ISR_RXNE)
    {
        uint8_t byte = (uint8_t) UART_RDR;
        if (g_modbus.rx_len < MODBUS_BUF_SIZE)
        {
            g_modbus.rx_buf[g_modbus.rx_len] = byte;
        }
        g_modbus.rx_len++;   /* increment even past BUF_SIZE to flag overflow */
    }

    /* IDLE: end of frame. */
    if (status & UART_ISR_IDLE)
    {
        UART_ICR = UART_ICR_IDLECF;

        if (g_modbus.rx_len > MODBUS_BUF_SIZE)
        {
            /* Buffer overrun — frame too long. */
            g_modbus.rx_len = 0U;
            if (g_modbus.rx_cb != NULL)
            {
                g_modbus.rx_cb(MODBUS_EVENT_RX_ERROR, g_modbus.rx_ctx);
            }
        }
        else
        {
            /* Complete frame in buffer. */
            if (g_modbus.rx_cb != NULL)
            {
                g_modbus.rx_cb(MODBUS_EVENT_RX_DONE, g_modbus.rx_ctx);
            }
        }
    }

    /* Hardware errors (ORE / FE / NE). */
    if (status & UART_ERR_MASK)
    {
        UART_ICR = (UART_ICR_ORECF | UART_ICR_FECF | UART_ICR_NECF);
        g_modbus.rx_len = 0U;
        if (g_modbus.rx_cb != NULL)
        {
            g_modbus.rx_cb(MODBUS_EVENT_RX_ERROR, g_modbus.rx_ctx);
        }
    }
}
```

---

## Marking guide

**Must have:**
- `rx_len` incremented even when `rx_len >= MODBUS_BUF_SIZE` (overflow sentinel), not just inside the bounds check — otherwise overflow is undetectable on IDLE
- IDLE flag cleared via `UART_ICR = UART_ICR_IDLECF` (write-to-clear, not read-modify-write)
- ORE/FE/NE cleared via `UART_ICR` write (not just reading RDR)
- `rx_len` reset to 0 on both overflow and error paths
- Callback invoked with correct event in all three cases

**Good to have:**
- NULL-guard on `rx_cb` before invoking it
- Separate IDLE and error checks (not combined into `else if`)
- `uint8_t` cast on `UART_RDR` read to avoid sign extension

**Red flags:**
- `rx_len++` inside the bounds check only — means overflow is never reported
- Clearing IDLE by reading RDR (F4 sequence, not L4)
- `memset(rx_buf, 0, ...)` inside the ISR — unnecessary and slow for an ISR
- Calling `xTaskNotify` (non-FromISR variant) from inside the ISR
- Not resetting `rx_len` after an ORE — the corrupt partial frame is silently passed as valid data
