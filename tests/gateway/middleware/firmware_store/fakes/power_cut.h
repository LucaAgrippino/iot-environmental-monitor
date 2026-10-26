/**
 * @file power_cut.h
 * @brief Power-loss injector shared by the flash fakes (FirmwareStore tests).
 *
 * Every mutating flash operation (erase or program, on either device) calls
 * power_cut_tick(). Armed with N, the operation numbered N (counting from zero)
 * is cut: it leaves a torn effect behind and fails. Every later operation also
 * fails and has no effect, as if the supply were gone, until power_cut_restore().
 *
 * Including this header makes Ceedling auto-link power_cut.c.
 */

#ifndef POWER_CUT_H
#define POWER_CUT_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Verdict for one mutating operation. */
typedef enum
{
    POWER_CUT_PASS = 0, /**< Power is on: perform the operation normally.            */
    POWER_CUT_TORN,     /**< This operation is cut: leave a torn effect and fail.     */
    POWER_CUT_DEAD      /**< Power already lost: do nothing and fail.                 */
} power_cut_verdict_t;

/** @brief Disarm, power on, zero the operation counter. */
void power_cut_reset(void);

/** @brief Cut power during the mutating operation with this index (0 = the first). */
void power_cut_arm(uint32_t operation_index);

/** @brief Power is back: later operations pass. The counter keeps running; the cut stays spent. */
void power_cut_restore(void);

/** @brief True once the cut has fired and power has not been restored. */
bool power_cut_is_dead(void);

/** @brief Mutating operations seen since power_cut_reset(). */
uint32_t power_cut_operations(void);

/** @brief Called by the flash fakes before they mutate anything. */
power_cut_verdict_t power_cut_tick(void);

#endif /* POWER_CUT_H */
