/**
 * @file imu_driver_internal.h
 * @brief Internal header — ISR entry point of ImuDriver (GPB-D7).
 *
 * Not part of the public API. Included by the EXTI line 11 handler (the system
 * layer, or a bring-up harness) and by the unit tests.
 */

#ifndef IMU_DRIVER_INTERNAL_H
#define IMU_DRIVER_INTERNAL_H

/**
 * @brief INT1 ISR handler — call from the EXTI15_10 handler after exti_clear_pending(11).
 *
 * Invokes the registered INT1 callback if there is one. ISR context only.
 */
void imu_int1_irq_handler(void);

#endif /* IMU_DRIVER_INTERNAL_H */
