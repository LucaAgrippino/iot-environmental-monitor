# Interview Test — I2cDriver (25 minutes)

## Brief (read this first — 3 minutes)

The I2cDriver serialises I2C bus transactions for a fleet of sensor and
peripheral drivers on an STM32 microcontroller. It exposes three operations:
write-only, read-only, and a combined write-then-read. The combined operation
is the workhorse: it sends a register address byte in the write phase, then
receives the register contents in the read phase with a repeated START (no STOP
in between). The peripheral is polled; there are no interrupts or DMA.

Your task is to implement `i2c_write_read_single` — a simplified version of the
combined write-then-read that always reads exactly one byte. Use the STM32 I2C
v1 (F4 series) register model. The key constraint is a well-known hardware
errata: when receiving only one byte, you must disable the ACK bit **before**
clearing ADDR (by reading SR1 then SR2), not after. Getting this order wrong
causes the peripheral to NACK the byte it is about to send you.

---

## Files given to the candidate

### `i2c_exercise.h`

```c
#ifndef I2C_EXERCISE_H
#define I2C_EXERCISE_H

#include <stdint.h>

/**
 * @brief I2C v1 register map (STM32F4). Only the fields used here.
 * In production code this comes from stm32f469xx.h; here it is inlined
 * for the exercise.
 */
typedef struct {
    volatile uint32_t CR1;   /* Control register 1 */
    volatile uint32_t CR2;
    volatile uint32_t OAR1;
    volatile uint32_t OAR2;
    volatile uint32_t DR;    /* Data register (TX write, RX read) */
    volatile uint32_t SR1;   /* Status register 1 */
    volatile uint32_t SR2;   /* Status register 2 */
    volatile uint32_t CCR;
    volatile uint32_t TRISE;
} I2C_TypeDef;

/* CR1 bits */
#define I2C_CR1_PE    (1UL << 0U)   /* Peripheral enable  */
#define I2C_CR1_START (1UL << 8U)   /* Start generation   */
#define I2C_CR1_STOP  (1UL << 9U)   /* Stop generation    */
#define I2C_CR1_ACK   (1UL << 10U)  /* Acknowledge enable */

/* SR1 bits */
#define I2C_SR1_SB    (1UL << 0U)   /* Start bit generated */
#define I2C_SR1_ADDR  (1UL << 1U)   /* Address sent        */
#define I2C_SR1_BTF   (1UL << 2U)   /* Byte transfer finished */
#define I2C_SR1_RXNE  (1UL << 6U)   /* Receive buffer not empty */
#define I2C_SR1_TXE   (1UL << 7U)   /* Transmit buffer empty */
#define I2C_SR1_AF    (1UL << 10U)  /* Acknowledge failure */

/* SR2 bits */
#define I2C_SR2_BUSY  (1UL << 1U)   /* Bus busy */

typedef enum {
    I2C_OK       = 0,
    I2C_ERR_NACK = 1,
    I2C_ERR_TIMEOUT = 2,
    I2C_ERR_BUS_BUSY = 3,
} i2c_err_t;

#define I2C_TIMEOUT_LOOPS (50000U)

/* I2C1 peripheral pointer — in production this is a CMSIS macro. */
extern I2C_TypeDef *const I2C1;

/**
 * @brief Combined write-then-read I2C transaction, single receive byte.
 *
 * Generates: START -> address (write) -> reg_addr -> repeated START ->
 * address (read) -> 1 byte -> STOP.
 *
 * The repeated START is issued internally; the caller does not manage it.
 *
 * @param dev_addr   7-bit device address (not shifted).
 * @param reg_addr   Register address byte to send in the write phase.
 * @param rx_byte    Pointer to receive the single result byte. Must not be NULL.
 * @return I2C_OK, I2C_ERR_NACK, I2C_ERR_TIMEOUT, or I2C_ERR_BUS_BUSY.
 */
i2c_err_t i2c_write_read_single(uint8_t dev_addr, uint8_t reg_addr,
                                 uint8_t *rx_byte);

#endif /* I2C_EXERCISE_H */
```

### `i2c_exercise.c` (partial)

```c
#include "i2c_exercise.h"

/* Helper: poll a status flag up to TIMEOUT_LOOPS times.
 * Returns 1 if the flag set within the loop, 0 on timeout. */
static int poll_flag(volatile uint32_t *reg, uint32_t mask)
{
    uint32_t n = I2C_TIMEOUT_LOOPS;
    while (!(*reg & mask)) {
        if (--n == 0) return 0;
    }
    return 1;
}

i2c_err_t i2c_write_read_single(uint8_t dev_addr, uint8_t reg_addr,
                                 uint8_t *rx_byte)
{
    /* 1. Check BUSY */
    if (I2C1->SR2 & I2C_SR2_BUSY) return I2C_ERR_BUS_BUSY;

    /* 2. Generate START */
    I2C1->CR1 |= I2C_CR1_START;
    if (!poll_flag(&I2C1->SR1, I2C_SR1_SB)) return I2C_ERR_TIMEOUT;

    /* 3. Send address + W */
    I2C1->DR = (uint8_t)((dev_addr << 1U) & 0xFEU);
    if (!poll_flag(&I2C1->SR1, I2C_SR1_ADDR)) {
        if (I2C1->SR1 & I2C_SR1_AF) {
            I2C1->SR1 &= ~I2C_SR1_AF;
            I2C1->CR1 |= I2C_CR1_STOP;
            return I2C_ERR_NACK;
        }
        return I2C_ERR_TIMEOUT;
    }
    (void)I2C1->SR1; (void)I2C1->SR2; /* clear ADDR */

    /* 4. Send register address byte */
    I2C1->DR = reg_addr;
    if (!poll_flag(&I2C1->SR1, I2C_SR1_TXE)) return I2C_ERR_TIMEOUT;
    if (!poll_flag(&I2C1->SR1, I2C_SR1_BTF)) return I2C_ERR_TIMEOUT;

    /* 5. Repeated START */
    I2C1->CR1 |= I2C_CR1_START;
    if (!poll_flag(&I2C1->SR1, I2C_SR1_SB)) return I2C_ERR_TIMEOUT;

    /* 6. Send address + R */
    I2C1->DR = (uint8_t)(((dev_addr << 1U) & 0xFEU) | 0x01U);

    /* TODO: implement the single-byte receive phase.
     *
     * Constraints:
     *   - rx_len == 1 requires a specific register sequence to work correctly
     *     on I2C v1 (RM0386 §27.3.3). Getting the order wrong causes a NACK.
     *   - Use poll_flag() for all waits; return I2C_ERR_TIMEOUT if any times out.
     *   - Write the received byte to *rx_byte.
     *   - Restore ACK for future transactions before returning.
     */

    return I2C_OK;
}
```

---

## Follow-up questions

**Q1:** Why must ACK be cleared **before** reading SR1 then SR2 (the ADDR clear
sequence), not after, when rx_len is 1?

*Model answer:* Reading SR2 clears the ADDR flag, which releases the clock
stretch and allows the peripheral to start shifting in the byte. At that
instant, the hardware needs to know whether to send ACK or NACK at the end of
the byte. If ACK is still set when ADDR clears, the peripheral sends an ACK
after the byte and expects another byte — but there is none, causing the bus to
stall or corrupt the next transaction. Clearing ACK first means the peripheral
sends NACK at the end of the single byte, signalling the remote device to stop.
This is documented in RM0386 §27.3.3 as a mandatory errata workaround.

**Q2:** After the single-byte read, the implementation re-enables ACK. Why is
this necessary, and what symptom would appear if it were omitted?

*Model answer:* The I2C peripheral's CR1.ACK bit is a persistent configuration
register — it stays set to whatever value you last wrote, across calls. If ACK
is left disabled after a single-byte read, the next multi-byte read (or the
next `i2c_write_read_single`) will fail: the peripheral will NACK after the
first byte of the read phase, truncating the response. The symptom on hardware
is an `I2C_ERR_TIMEOUT` (RXNE never asserts for bytes 2+) or a corrupted read
after the first byte. It passes CI because unit tests zero-initialise the mock
registers in setUp() and re-run i2c_init() before each test, which restores
CR1.ACK as part of peripheral reset.

**Q3:** Why is `i2c_write_read` implemented as a single driver call rather than
letting the caller do a separate write followed by a read?

*Model answer:* The repeated START between the write and read phases must happen
atomically — no STOP can appear between them. A STOP would release the bus, and
another master could seize it before the read phase begins. More fundamentally,
the hardware peripheral's repeated START is generated by setting START while the
bus is still busy after the write. If the write and read are separate calls,
the driver would have to leave the bus in an intermediate state (no STOP
generated), which is invisible to and incoherent for any caller. Implementing
it as one atomic call keeps the hardware detail inside the driver (Principle 1:
strict directional layering) and gives callers a simple, race-free API.

---

## Model solution

```c
i2c_err_t i2c_write_read_single(uint8_t dev_addr, uint8_t reg_addr,
                                 uint8_t *rx_byte)
{
    /* 1. Check BUSY */
    if (I2C1->SR2 & I2C_SR2_BUSY) return I2C_ERR_BUS_BUSY;

    /* 2. Generate START */
    I2C1->CR1 |= I2C_CR1_START;
    if (!poll_flag(&I2C1->SR1, I2C_SR1_SB)) return I2C_ERR_TIMEOUT;

    /* 3. Send address + W */
    I2C1->DR = (uint8_t)((dev_addr << 1U) & 0xFEU);
    if (!poll_flag(&I2C1->SR1, I2C_SR1_ADDR)) {
        if (I2C1->SR1 & I2C_SR1_AF) {
            I2C1->SR1 &= ~I2C_SR1_AF;
            I2C1->CR1 |= I2C_CR1_STOP;
            return I2C_ERR_NACK;
        }
        return I2C_ERR_TIMEOUT;
    }
    (void)I2C1->SR1; (void)I2C1->SR2;

    /* 4. Send register address */
    I2C1->DR = reg_addr;
    if (!poll_flag(&I2C1->SR1, I2C_SR1_TXE)) return I2C_ERR_TIMEOUT;
    if (!poll_flag(&I2C1->SR1, I2C_SR1_BTF)) return I2C_ERR_TIMEOUT;

    /* 5. Repeated START */
    I2C1->CR1 |= I2C_CR1_START;
    if (!poll_flag(&I2C1->SR1, I2C_SR1_SB)) return I2C_ERR_TIMEOUT;

    /* 6. Send address + R */
    I2C1->DR = (uint8_t)(((dev_addr << 1U) & 0xFEU) | 0x01U);

    /* 7. Single-byte rx: disable ACK BEFORE clearing ADDR (v1 errata). */
    I2C1->CR1 &= ~I2C_CR1_ACK;

    /* 8. Poll ADDR, then clear it (SR1 read then SR2 read). */
    if (!poll_flag(&I2C1->SR1, I2C_SR1_ADDR)) {
        if (I2C1->SR1 & I2C_SR1_AF) {
            I2C1->SR1 &= ~I2C_SR1_AF;
            I2C1->CR1 |= I2C_CR1_STOP;
            return I2C_ERR_NACK;
        }
        return I2C_ERR_TIMEOUT;
    }
    (void)I2C1->SR1; (void)I2C1->SR2;  /* clears ADDR — ACK already disabled */

    /* 9. Generate STOP before reading DR (v1 errata for rx_len=1). */
    I2C1->CR1 |= I2C_CR1_STOP;

    /* 10. Wait for data and read it. */
    if (!poll_flag(&I2C1->SR1, I2C_SR1_RXNE)) return I2C_ERR_TIMEOUT;
    *rx_byte = (uint8_t)I2C1->DR;

    /* 11. Re-enable ACK for subsequent transactions. */
    I2C1->CR1 |= I2C_CR1_ACK;

    return I2C_OK;
}
```

---

## Marking guide

**Must have (pass/fail):**
- ACK disabled **before** `(void)SR1; (void)SR2;` (ADDR clear) on the read address phase — not after.
- STOP set **before** reading DR (not after).
- `*rx_byte` written from `I2C1->DR`.
- Returns `I2C_OK` on success.
- Handles NACK on the read address phase (AF flag) — generates STOP and returns `I2C_ERR_NACK`.
- Polls `RXNE` with a timeout; returns `I2C_ERR_TIMEOUT` if it expires.

**Nice to have (differentiates mid from senior):**
- Re-enables ACK after the read (restores state for next call).
- Comments explaining the ACK-before-ADDR sequence with a reference to RM0386 §27.3.3 or equivalent.
- Uses named constants (`I2C_CR1_ACK`, `I2C_SR1_ADDR`) rather than raw bit values.
- Checks NACK (AF flag) during the write-phase ADDR wait as well.

**Red flags (automatic fail or strong negative signal):**
- Clearing ADDR before disabling ACK (the entire point of the exercise).
- Setting STOP after reading DR instead of before — causes a second bus cycle.
- Busy-wait without any timeout guard.
- Hard-coding the 7-bit address shifted left (e.g. `0x70` for `0x38`) — candidate does not understand the 7-bit/8-bit address distinction.
- Dynamic memory allocation or any `malloc`/`free`.
