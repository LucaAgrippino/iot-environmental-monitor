# Bug Log — I2cDriver

## I2cDriver — missing ACK re-enable after single-byte write-read

**File:** `firmware/field-device/drivers/i2c/i2c_driver_f4.c`
**Line:** ~350 (in the `rx_len == 1` branch of `i2c_write_read`)
**Category:** Missing state-clear before returning from an error path (variant:
missing state restoration before returning from a success path)

**What the code does:**
After a successful single-byte `i2c_write_read`, the driver clears `CR1.ACK`
(as required by the v1 errata for single-byte receive) but does not re-enable
it before returning `I2C_ERR_OK`.

**What it should do:**
After reading the single byte and returning, the driver should set
`I2C1->CR1 |= I2C_CR1_ACK` to restore ACK for the next transaction — exactly
as the multi-byte path does at step 19.

**Correct fix:**
```c
/* before (buggy — ACK not re-enabled) */
rx_buf[0] = (uint8_t) I2C1->DR;
/* Intentional bug: ACK is not re-enabled here. ... */

/* after (correct) */
rx_buf[0] = (uint8_t) I2C1->DR;
I2C1->CR1 |= I2C_CR1_ACK;   /* re-enable ACK for next transaction */
```

**How to find it with a debugger:**
1. Flash the board and run the integration test in Phase 3 (`i2c_write_read`
   with `rx_len=1` to read FT6206 chip ID). Note it returns 0x06 correctly.
2. Set a breakpoint at the next `i2c_read` or `i2c_write_read` call after the
   single-byte call.
3. At the breakpoint, open the I2C1 peripheral registers in the debugger
   (STM32CubeIDE live register view). Read `I2C1_CR1`. Observe that bit 10
   (`ACK`) is **clear** — it should be set.
4. Continue execution: the next call will fail. The FT6206 will not receive an
   ACK after sending its first data byte, causing it to abort the transaction.
   The driver returns `I2C_ERR_TIMEOUT` (RXNE never asserts as the device
   aborts mid-transfer) or `I2C_ERR_NACK`.
5. Add a watchpoint on `I2C1->CR1` to observe that the `ACK` bit is cleared
   by the write_read call and never restored.

**Why it passes CI:**
Each unit test calls `i2c_reset_for_test()` in `setUp()`, which clears
`s_i2c.initialised`. The test's `setUp()` also zeroes the mock registers via
`stm32_cmsis_mock_reset()`, resetting `g_mock_i2c1.CR1 = 0`. The subsequent
`i2c_init()` call in `arrange_init_success()` writes `I2C_CR1_ACK | I2C_CR1_PE`
to `CR1`, restoring the ACK bit before the test begins. No test sequence
exercises a single-byte `i2c_write_read` immediately followed by a multi-byte
read within the same test case, so the cross-call ACK state is never observed.
