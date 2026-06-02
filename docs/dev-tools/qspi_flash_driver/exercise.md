# Interview Test — QspiFlashDriver (20–25 minutes)

## Brief (3 minutes)

This embedded system stores sensor configuration and firmware update images on an external
NOR flash device (Macronix MX25R6435F, 8 MB) connected via the STM32 QUADSPI peripheral.
The driver must provide three primitive operations — read, page-program, and sector-erase —
using the QUADSPI peripheral in indirect mode. All commands use standard SPI (1-1-1 mode).

You are asked to implement `qspi_write_page`: a function that programs up to 256 bytes into
a single flash page. The function must issue a Write Enable command (0x06), then a Page Program
command (0x02), wait for the device to finish via WIP polling (Read Status Register, 0x05),
and enforce the hardware constraint that all bytes in a single page program must lie within
the same 256-byte page boundary. Peripheral register access is via CMSIS-style macros. No
dynamic memory allocation. No RTOS primitives.

---

## Files given to the candidate

### `qspi_exercise.h`

```c
#ifndef QSPI_EXERCISE_H
#define QSPI_EXERCISE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Error codes for QspiFlashDriver operations.
 */
typedef enum {
    QSPI_OK      = 0, /**< Success. */
    QSPI_BUSY    = 1, /**< Peripheral busy at entry. */
    QSPI_TIMEOUT = 2, /**< WIP poll timed out. */
    QSPI_ADDR    = 3, /**< Address out of range. */
    QSPI_LEN     = 4, /**< len == 0, or write crosses 256-byte page boundary. */
} qspi_err_t;

/** Flash device size in bytes (8 MB). */
#define FLASH_DEVICE_SIZE   (8UL * 1024UL * 1024UL)

/** Flash page size in bytes. */
#define FLASH_PAGE_SIZE     (256U)

/** Flash command codes. */
#define QSPI_CMD_WREN  (0x06U)  /**< Write Enable. */
#define QSPI_CMD_PP    (0x02U)  /**< Page Program. */
#define QSPI_CMD_RDSR  (0x05U)  /**< Read Status Register. */

/** WIP bit in flash Status Register 1. */
#define FLASH_SR1_WIP   (0x01U)

/** Maximum WIP poll iterations before timeout. */
#define WIP_TIMEOUT_COUNT  (300000U)

/**
 * @brief QUADSPI CCR field positions (subset).
 *
 * INSTRUCTION [7:0], IMODE [9:8], ADMODE [11:10], ADSIZE [13:12],
 * DMODE [25:24], FMODE [27:26].
 * IMODE/ADMODE/DMODE = 01 means single-wire. ADSIZE = 10 means 24-bit address.
 * FMODE = 00 = indirect write; FMODE = 01 = indirect read.
 */
#define CCR_INSTRUCTION_POS  (0U)
#define CCR_IMODE_SINGLE     (0x1UL << 8U)
#define CCR_ADMODE_SINGLE    (0x1UL << 10U)
#define CCR_ADSIZE_24BIT     (0x2UL << 12U)
#define CCR_DMODE_SINGLE     (0x1UL << 24U)
#define CCR_FMODE_IND_WRITE  (0x0UL << 26U)
#define CCR_FMODE_IND_READ   (0x1UL << 26U)

/** QUADSPI SR bits. */
#define QSPI_SR_TCF    (1UL << 1U)  /**< Transfer Complete Flag. */
#define QSPI_SR_BUSY   (1UL << 5U)  /**< Peripheral busy. */

/** QUADSPI FCR — write 1 to clear TCF. */
#define QSPI_FCR_CTCF  (1UL << 1U)

/* Assume QUADSPI peripheral is accessible via the QUADSPI macro, which
 * expands to a pointer to a QUADSPI_TypeDef struct with fields:
 *   CR, DCR, SR, FCR, DLR, CCR, AR, ABR, DR  (all volatile uint32_t).
 * Assume a helper poll_tcf() is already implemented — it polls QUADSPI_SR.TCF
 * until set (or returns QSPI_TIMEOUT), then clears it. Its signature is:
 *   qspi_err_t poll_tcf(void);
 */

/**
 * @brief Program up to 256 bytes into a single flash page.
 *
 * Issues Write Enable (0x06) then Page Program (0x02) with addr and len
 * bytes from data. Polls WIP until the device completes the write.
 *
 * Constraints:
 *   - len must be >= 1 and <= FLASH_PAGE_SIZE.
 *   - addr and addr + len - 1 must lie in the same 256-byte page.
 *   - addr must be < FLASH_DEVICE_SIZE.
 *
 * @param addr  Byte address of the first byte to program.
 * @param data  Source buffer (must not be NULL).
 * @param len   Number of bytes (1 .. 256).
 * @return QSPI_OK on success; QSPI_LEN, QSPI_ADDR, QSPI_TIMEOUT, or
 *         QSPI_BUSY on error.
 */
qspi_err_t qspi_write_page(uint32_t addr, const uint8_t *data, uint16_t len);

#endif /* QSPI_EXERCISE_H */
```

### `qspi_exercise.c` (partial)

```c
#include "qspi_exercise.h"
#include <stddef.h>

/* Page boundary mask: strips the lower 8 bits. */
#define PAGE_MASK  (~0xFFUL)

/* poll_tcf() is provided — polls QUADSPI->SR.TCF and clears it.
 * Returns QSPI_OK or QSPI_TIMEOUT. */
extern qspi_err_t poll_tcf(void);

qspi_err_t qspi_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    /* TODO: implement this function.
     *
     * Sequence:
     *   1. Check QUADSPI peripheral not BUSY; check constraints (NULL, len, page boundary, addr).
     *   2. Issue Write Enable (0x06): set CCR with instruction + IMODE, FMODE=indirect write,
     *      no address, no data. Wait for TCF.
     *   3. Set DLR = len - 1. Set CCR for Page Program (0x02): IMODE, ADMODE (24-bit), DMODE,
     *      FMODE=indirect write. Set AR = addr.
     *   4. Write len bytes to QUADSPI->DR one at a time.
     *   5. Wait for TCF.
     *   6. Poll WIP: issue RDSR (0x05, no address, FMODE=indirect read, DLR=0),
     *      wait for TCF, read 1 byte from DR. Repeat until WIP=0 or timeout.
     *   7. Return QSPI_OK on success.
     */

    return QSPI_OK; /* replace this */
}
```

---

## Follow-up questions

**Q1:** Why must Write Enable be re-issued before every page program and sector
erase, even for consecutive writes to the same sector?

*Model answer:* NOR flash hardware automatically clears the Write Enable Latch
(WEL) after every successfully completed write or erase operation. This is a
safety mechanism to prevent accidental writes if the SPI bus glitches. The driver
must re-issue WREN (0x06) before each operation, not once at startup.

**Q2:** The companion document says WIP polling busy-waits for up to 500 ms
during a sector erase. What is the impact on system responsiveness, and what
trade-off would you consider to improve it?

*Model answer:* The calling task is blocked for up to 500 ms, which delays any
tasks waiting to run at the same or lower priority. An improvement is to replace
the tight loop with `vTaskDelay(1)` between each RDSR read, which yields the
CPU to other tasks during the wait. The trade-off is that this imports FreeRTOS
into the driver layer, violating the P1 layering principle (drivers should depend
only on CMSIS). The project defers this change until integration profiling
confirms it is needed.

**Q3:** Why does the driver enforce the 256-byte page boundary at the driver
layer rather than relying on callers to align writes?

*Model answer:* NOR flash hardware wraps a Page Program command at the page
boundary — if the write crosses a 256-byte boundary, the device silently programs
the overflow bytes into the start of the same page, corrupting previously
programmed data. This is a hardware behaviour the caller cannot easily observe.
Enforcing it in the driver converts silent data corruption into an explicit error
return (QSPI_FLASH_ERR_LEN), consistent with the P8 principle of no silent
failures. Callers (middleware) must split multi-page writes into aligned calls.

---

## Model solution

```c
qspi_err_t qspi_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    qspi_err_t err;
    uint16_t   i;

    /* Input validation. */
    if (QUADSPI->SR & QSPI_SR_BUSY)
    {
        return QSPI_BUSY;
    }
    if ((len == 0U) || (len > (uint16_t)FLASH_PAGE_SIZE) || (data == NULL))
    {
        return QSPI_LEN;
    }
    if (addr >= FLASH_DEVICE_SIZE)
    {
        return QSPI_ADDR;
    }
    /* Page boundary check: first and last byte must be in the same 256-byte page. */
    if ((addr & PAGE_MASK) != ((addr + (uint32_t)len - 1U) & PAGE_MASK))
    {
        return QSPI_LEN;
    }

    /* Step 2: Write Enable. */
    QUADSPI->CCR = ((uint32_t)QSPI_CMD_WREN << CCR_INSTRUCTION_POS) |
                   CCR_IMODE_SINGLE | CCR_FMODE_IND_WRITE;
    err = poll_tcf();
    if (err != QSPI_OK)
    {
        return err;
    }

    /* Step 3: Page Program — set DLR, CCR, AR. */
    QUADSPI->DLR = (uint32_t)len - 1U;
    QUADSPI->CCR = ((uint32_t)QSPI_CMD_PP << CCR_INSTRUCTION_POS) |
                   CCR_IMODE_SINGLE | CCR_ADMODE_SINGLE | CCR_ADSIZE_24BIT |
                   CCR_DMODE_SINGLE | CCR_FMODE_IND_WRITE;
    QUADSPI->AR = addr;

    /* Step 4: Write data bytes. */
    for (i = 0U; i < len; i++)
    {
        *(volatile uint8_t *)&QUADSPI->DR = data[i];
    }

    /* Step 5: Wait for transfer complete. */
    err = poll_tcf();
    if (err != QSPI_OK)
    {
        return err;
    }

    /* Step 6: Poll WIP. */
    {
        uint32_t count = WIP_TIMEOUT_COUNT;
        while (count > 0U)
        {
            uint8_t status;
            QUADSPI->DLR = 0U;
            QUADSPI->CCR = ((uint32_t)QSPI_CMD_RDSR << CCR_INSTRUCTION_POS) |
                           CCR_IMODE_SINGLE | CCR_DMODE_SINGLE | CCR_FMODE_IND_READ;
            err = poll_tcf();
            if (err != QSPI_OK)
            {
                return QSPI_TIMEOUT;
            }
            status = *(volatile uint8_t *)&QUADSPI->DR;
            if ((status & FLASH_SR1_WIP) == 0U)
            {
                return QSPI_OK;
            }
            count--;
        }
    }
    return QSPI_TIMEOUT;
}
```

---

## Marking guide

**Must have (pass/fail):**
- Write Enable (0x06) issued BEFORE Page Program (0x02), not after.
- DLR = `len - 1` (not `len`).
- Page boundary check uses `addr + len - 1` (not `addr + len`).
- WIP polling issues a new RDSR command per iteration (not re-reading old DR value).
- WIP timeout returns `QSPI_TIMEOUT`, not `QSPI_OK`.

**Nice to have (differentiates mid from senior):**
- All four input-validation checks (BUSY, len, NULL, addr, boundary) with correct error codes.
- `count--` placed AFTER checking WIP (so timeout fires correctly).
- BARR-C style: fixed-width types, named constants, no magic numbers.
- Correctly identifies that WEL auto-clears — explains why WREN must be re-issued per write.

**Red flags (automatic fail or strong negative signal):**
- Page boundary check uses `addr + len` instead of `addr + len - 1` (classic off-by-one).
- WIP polling checks `QUADSPI->DR` without issuing a new RDSR command each iteration.
- Missing WREN before Page Program.
- Returning `QSPI_OK` from the WIP timeout path.
- Using `malloc` or any heap allocation.
