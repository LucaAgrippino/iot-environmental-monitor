# Bug Log — ModbusUartDriver

## TC timeout uses wrong constant (MBUART-BUG-01)

**File:** `firmware/field-device/drivers/modbus_uart_driver/modbus_uart_driver.c`
**Line:** 306
**Category:** Wrong constant

**What the code does:**
After writing the last byte, the driver polls the TC (Transmission Complete) flag
and times out after `MODBUS_UART_TXE_TIMEOUT_MS` (5 ms).

**What it should do:**
The TC wait should time out after `MODBUS_UART_TC_TIMEOUT_MS` (10 ms), which is the
constant defined specifically for the post-last-byte TC wait (double the per-byte
TXE timeout to give the shift register time to finish clocking out the stop bit).

**Correct fix:**

```c
/* before (line 306) */
if ((s_modbus_uart.get_ms() - t0) >= MODBUS_UART_TXE_TIMEOUT_MS)

/* after */
if ((s_modbus_uart.get_ms() - t0) >= MODBUS_UART_TC_TIMEOUT_MS)
```

**How to find it with a debugger:**

1. Set `MODBUS_UART_TC_TIMEOUT_MS` to a value significantly larger than
   `MODBUS_UART_TXE_TIMEOUT_MS` (e.g., TC = 50 ms, TXE = 5 ms).
2. Place a breakpoint at the TC polling loop entry (after the per-byte loop).
3. Arrange for TC to be asserted exactly once, 8 ms after the last byte is written
   (within TC_TIMEOUT but past TXE_TIMEOUT).
4. `modbus_uart_transmit()` will return `MODBUS_UART_ERR_TIMEOUT` instead of
   `MODBUS_UART_ERR_OK` — the TC wait expired after 5 ms, 3 ms before TC asserted.
5. Alternatively: instrument `get_ms` to advance at a known rate and assert that
   `transmit()` returns OK only when TC fires within 5 ms.

**Why it passes CI:**

Unit tests pre-assert both `TXE` and `TC` in the mock status register before calling
`modbus_uart_transmit()`. The TC polling loop exits immediately on the first
iteration (TC is already set), so the timeout comparison is never reached. The bug
is only visible when TC is delayed relative to TXE — a realistic hardware scenario
at high baud rates or with a slow shift register, but impossible to observe in the
mock-based unit tests.
