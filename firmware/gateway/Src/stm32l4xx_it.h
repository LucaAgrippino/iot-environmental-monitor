/**
 * @file stm32l4xx_it.h
 * @brief Gateway interrupt service routines owned by the system layer.
 *
 * Declares the EXTI vectors that route a pending line to the owning driver's
 * ISR entry point. The definitions are weak (see stm32l4xx_it.c) so a bring-up
 * harness can still supply its own.
 */

#ifndef STM32L4XX_IT_H
#define STM32L4XX_IT_H

#include <stdint.h>

/** Uncorrectable flash ECC errors absorbed by NMI_Handler since reset. */
extern volatile uint32_t g_flash_ecc_nmi_count;

/**
 * @brief NMI: an uncorrectable flash ECC error is cleared and execution resumes.
 *
 * A boot record torn by a power cut leaves a double word whose ECC no longer matches, and the
 * first read of it raises this NMI (RM0351 3.7). Without a handler the startup default is an
 * infinite loop, so FirmwareStore's own read of that record would hang the Gateway. The read
 * itself completes with unreliable data, which the record's CRC then rejects (bootloader.md
 * BL-D8, BL-O8). Any other NMI source halts, as the default did.
 */
void NMI_Handler(void);

/** @brief EXTI line 1 — ISM43362 DATARDY (PE1) -> WifiDriver. */
void EXTI1_IRQHandler(void);

/** @brief EXTI lines 5..9 — line 8: LIS3MDL DRDY (PC8) -> MagnetometerDriver. */
void EXTI9_5_IRQHandler(void);

/** @brief EXTI lines 10..15 — line 11: LSM6DSL INT1 (PD11) -> ImuDriver. */
void EXTI15_10_IRQHandler(void);

#endif /* STM32L4XX_IT_H */
