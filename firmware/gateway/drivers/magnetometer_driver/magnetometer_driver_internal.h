/**
 * @file magnetometer_driver_internal.h
 * @brief Internal header — ISR entry point of MagnetometerDriver (GPB-D7).
 *
 * Not part of the public API. Included by the EXTI line 8 handler (the system
 * layer, or a bring-up harness) and by the unit tests.
 */

#ifndef MAGNETOMETER_DRIVER_INTERNAL_H
#define MAGNETOMETER_DRIVER_INTERNAL_H

/**
 * @brief DRDY ISR handler — call from the EXTI9_5 handler after exti_clear_pending(8).
 *
 * Invokes the registered DRDY callback if there is one. ISR context only.
 */
void magnetometer_drdy_irq_handler(void);

#endif /* MAGNETOMETER_DRIVER_INTERNAL_H */
