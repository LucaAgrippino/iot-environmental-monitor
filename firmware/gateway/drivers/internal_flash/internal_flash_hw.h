/**
 * @file internal_flash_hw.h
 * @brief Register and bus access of the InternalFlashDriver, behind one seam.
 *
 * On target these are the CMSIS accesses to the FLASH controller. Under TEST they call
 * fake_flash_ctrl, a model of the controller (unlock sequence, PG/PER/START, BSY, EOP, error
 * flags) that records every register write. The IFLASH_* masks are the driver's own; on
 * target each is _Static_assert-ed against its CMSIS name.
 *
 * @see docs/lld/drivers/internal-flash-driver.md §3.1, §4
 */

#ifndef INTERNAL_FLASH_HW_H
#define INTERNAL_FLASH_HW_H

#include <stdint.h>

/* ---------------------------------------------------------------------- */
/* Register bits (RM0351 §3.7)                                             */
/* ---------------------------------------------------------------------- */

#define IFLASH_KEY1 (0x45670123UL)
#define IFLASH_KEY2 (0xCDEF89ABUL)

#define IFLASH_SR_EOP (1UL << 0)
#define IFLASH_SR_OPERR (1UL << 1)
#define IFLASH_SR_PROGERR (1UL << 3)
#define IFLASH_SR_WRPERR (1UL << 4)
#define IFLASH_SR_PGAERR (1UL << 5)
#define IFLASH_SR_SIZERR (1UL << 6)
#define IFLASH_SR_PGSERR (1UL << 7)
#define IFLASH_SR_MISERR (1UL << 8)
#define IFLASH_SR_FASTERR (1UL << 9)
#define IFLASH_SR_RDERR (1UL << 14)
#define IFLASH_SR_BSY (1UL << 16)

/** Every error flag; all are cleared by writing 1. */
#define IFLASH_SR_ERRORS                                                                           \
    (IFLASH_SR_OPERR | IFLASH_SR_PROGERR | IFLASH_SR_WRPERR | IFLASH_SR_PGAERR |                   \
     IFLASH_SR_SIZERR | IFLASH_SR_PGSERR | IFLASH_SR_MISERR | IFLASH_SR_FASTERR | IFLASH_SR_RDERR)

#define IFLASH_CR_PG (1UL << 0)
#define IFLASH_CR_PER (1UL << 1)
#define IFLASH_CR_PNB_POS (3U)
#define IFLASH_CR_PNB_MASK (0xFFUL << IFLASH_CR_PNB_POS)
#define IFLASH_CR_BKER (1UL << 11)
#define IFLASH_CR_START (1UL << 16)
#define IFLASH_CR_LOCK (1UL << 31)

#define IFLASH_OPTR_DUALBANK (1UL << 21)

#define IFLASH_ACR_ICEN (1UL << 9)
#define IFLASH_ACR_DCEN (1UL << 10)
#define IFLASH_ACR_ICRST (1UL << 11)
#define IFLASH_ACR_DCRST (1UL << 12)

#ifdef TEST

/* ---------------------------------------------------------------------- */
/* Test build: the controller model (fakes/fake_flash_ctrl.c)              */
/* ---------------------------------------------------------------------- */

void fake_flash_keyr_write(uint32_t value);
uint32_t fake_flash_sr_read(void);
void fake_flash_sr_write(uint32_t value);
uint32_t fake_flash_cr_read(void);
void fake_flash_cr_write(uint32_t value);
uint32_t fake_flash_optr_read(void);
uint32_t fake_flash_acr_read(void);
void fake_flash_acr_write(uint32_t value);
void fake_flash_store32(uint32_t addr, uint32_t value);
void fake_flash_load(uint32_t addr, uint8_t *buf, uint32_t len);
uint32_t fake_flash_irq_save(void);
void fake_flash_irq_restore(uint32_t state);

#define IFLASH_HW_KEYR_WRITE(v) fake_flash_keyr_write(v)
#define IFLASH_HW_SR_READ() fake_flash_sr_read()
#define IFLASH_HW_SR_WRITE(v) fake_flash_sr_write(v)
#define IFLASH_HW_CR_READ() fake_flash_cr_read()
#define IFLASH_HW_CR_WRITE(v) fake_flash_cr_write(v)
#define IFLASH_HW_OPTR_READ() fake_flash_optr_read()
#define IFLASH_HW_ACR_READ() fake_flash_acr_read()
#define IFLASH_HW_ACR_WRITE(v) fake_flash_acr_write(v)
#define IFLASH_HW_STORE32(addr, v) fake_flash_store32((addr), (v))
#define IFLASH_HW_LOAD(addr, buf, len) fake_flash_load((addr), (buf), (len))
#define IFLASH_HW_IRQ_SAVE() fake_flash_irq_save()
#define IFLASH_HW_IRQ_RESTORE(s) fake_flash_irq_restore(s)
#define IFLASH_HW_BARRIER() ((void) 0)

#else /* TARGET BUILD */

/* ---------------------------------------------------------------------- */
/* Target build: CMSIS                                                     */
/* ---------------------------------------------------------------------- */

#include <string.h>

#include "stm32l475xx.h"

_Static_assert(IFLASH_SR_EOP == FLASH_SR_EOP, "SR.EOP");
_Static_assert(IFLASH_SR_OPERR == FLASH_SR_OPERR, "SR.OPERR");
_Static_assert(IFLASH_SR_PROGERR == FLASH_SR_PROGERR, "SR.PROGERR");
_Static_assert(IFLASH_SR_WRPERR == FLASH_SR_WRPERR, "SR.WRPERR");
_Static_assert(IFLASH_SR_PGAERR == FLASH_SR_PGAERR, "SR.PGAERR");
_Static_assert(IFLASH_SR_SIZERR == FLASH_SR_SIZERR, "SR.SIZERR");
_Static_assert(IFLASH_SR_PGSERR == FLASH_SR_PGSERR, "SR.PGSERR");
_Static_assert(IFLASH_SR_MISERR == FLASH_SR_MISERR, "SR.MISERR");
_Static_assert(IFLASH_SR_FASTERR == FLASH_SR_FASTERR, "SR.FASTERR");
_Static_assert(IFLASH_SR_RDERR == FLASH_SR_RDERR, "SR.RDERR");
_Static_assert(IFLASH_SR_BSY == FLASH_SR_BSY, "SR.BSY");
_Static_assert(IFLASH_CR_PG == FLASH_CR_PG, "CR.PG");
_Static_assert(IFLASH_CR_PER == FLASH_CR_PER, "CR.PER");
_Static_assert(IFLASH_CR_PNB_MASK == FLASH_CR_PNB, "CR.PNB");
_Static_assert(IFLASH_CR_BKER == FLASH_CR_BKER, "CR.BKER");
_Static_assert(IFLASH_CR_START == FLASH_CR_STRT, "CR.START");
_Static_assert(IFLASH_CR_LOCK == FLASH_CR_LOCK, "CR.LOCK");
_Static_assert(IFLASH_OPTR_DUALBANK == FLASH_OPTR_DUALBANK, "OPTR.DUALBANK");
_Static_assert(IFLASH_ACR_ICEN == FLASH_ACR_ICEN, "ACR.ICEN");
_Static_assert(IFLASH_ACR_DCEN == FLASH_ACR_DCEN, "ACR.DCEN");
_Static_assert(IFLASH_ACR_ICRST == FLASH_ACR_ICRST, "ACR.ICRST");
_Static_assert(IFLASH_ACR_DCRST == FLASH_ACR_DCRST, "ACR.DCRST");

/** @brief Mask interrupts and return the previous PRIMASK. */
static inline uint32_t iflash_hw_irq_save(void)
{
    const uint32_t primask = __get_PRIMASK();

    __disable_irq();

    return primask;
}

/** @brief Re-enable interrupts unless they were already masked. */
static inline void iflash_hw_irq_restore(uint32_t primask)
{
    if (primask == 0U)
    {
        __enable_irq();
    }
}

#define IFLASH_HW_KEYR_WRITE(v) (FLASH->KEYR = (v))
#define IFLASH_HW_SR_READ() (FLASH->SR)
#define IFLASH_HW_SR_WRITE(v) (FLASH->SR = (v))
#define IFLASH_HW_CR_READ() (FLASH->CR)
#define IFLASH_HW_CR_WRITE(v) (FLASH->CR = (v))
#define IFLASH_HW_OPTR_READ() (FLASH->OPTR)
#define IFLASH_HW_ACR_READ() (FLASH->ACR)
#define IFLASH_HW_ACR_WRITE(v) (FLASH->ACR = (v))
#define IFLASH_HW_STORE32(addr, v) (*(volatile uint32_t *) (addr) = (v))
#define IFLASH_HW_LOAD(addr, buf, len) ((void) memcpy((buf), (const void *) (addr), (len)))
#define IFLASH_HW_IRQ_SAVE() iflash_hw_irq_save()
#define IFLASH_HW_IRQ_RESTORE(s) iflash_hw_irq_restore(s)
#define IFLASH_HW_BARRIER() __DSB()

#endif /* TEST */

#endif /* INTERNAL_FLASH_HW_H */
