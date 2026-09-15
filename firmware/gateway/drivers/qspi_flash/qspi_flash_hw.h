#ifndef QSPI_FLASH_HW_H
#define QSPI_FLASH_HW_H

#include <stdint.h>

/**
 * @brief Mockable indirection layer for the QUADSPI accesses that a plain
 *        register-struct mock cannot observe.
 *
 * Three accesses need more than "last value written" semantics in host
 * tests (companion §7.2):
 *   - CCR writes: the test must see the *sequence* of instructions
 *     (e.g. WREN before Page Program), not just the last one.
 *   - DR byte reads: each read must pop the next byte of a multi-byte
 *     response (RDID = 3 bytes, Read Data = N bytes).
 *   - DR byte writes: each written byte must be captured for comparison.
 *
 * In firmware builds the macros expand to direct register accesses. DR is
 * accessed through a uint8_t pointer so that the bus transaction is a
 * byte-width LDRB/STRB — a 32-bit access would advance the QUADSPI FIFO by
 * four bytes. In host unit-test builds they expand to instrumented stub
 * functions defined in stm32l475_cmsis_mock.c.
 */

#ifdef TEST

/* --------------------------------------------------------------------- */
/* Test-mode prototypes (implementations in stm32l475_cmsis_mock.c)       */
/* --------------------------------------------------------------------- */

void qspi_hw_write_ccr(uint32_t value);
uint8_t qspi_hw_read_dr_byte(void);
void qspi_hw_write_dr_byte(uint8_t value);

#define QSPI_HW_WRITE_CCR(v) qspi_hw_write_ccr(v)
#define QSPI_HW_READ_DR_BYTE() qspi_hw_read_dr_byte()
#define QSPI_HW_WRITE_DR_BYTE(b) qspi_hw_write_dr_byte(b)

#else /* TARGET BUILD */

/* --------------------------------------------------------------------- */
/* Real register accesses — available once stm32l475xx.h is included     */
/* --------------------------------------------------------------------- */

#define QSPI_HW_WRITE_CCR(v) (QUADSPI->CCR = (v))
#define QSPI_HW_READ_DR_BYTE() (*((volatile uint8_t *) &(QUADSPI->DR)))
#define QSPI_HW_WRITE_DR_BYTE(b) (*((volatile uint8_t *) &(QUADSPI->DR)) = (uint8_t) (b))

#endif /* TEST */

#endif /* QSPI_FLASH_HW_H */
