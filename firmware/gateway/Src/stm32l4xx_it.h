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

/** @brief EXTI line 1 — ISM43362 DATARDY (PE1) -> WifiDriver. */
void EXTI1_IRQHandler(void);

/** @brief EXTI lines 5..9 — line 8: LIS3MDL DRDY (PC8) -> MagnetometerDriver. */
void EXTI9_5_IRQHandler(void);

/** @brief EXTI lines 10..15 — line 11: LSM6DSL INT1 (PD11) -> ImuDriver. */
void EXTI15_10_IRQHandler(void);

#endif /* STM32L4XX_IT_H */
