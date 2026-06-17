/**
 * @file touchscreen_driver.h
 * @brief TouchscreenDriver public API -- FT6x06 capacitive touch controller.
 *
 * Two-phase init:
 *   touchscreen_init()       [pre-scheduler; must follow lcd_init()]
 *   touchscreen_attach_irq() [from LcdUiTask startup prologue]
 *
 * @see docs/lld/drivers/touchscreen-driver.md
 */

#ifndef TOUCHSCREEN_DRIVER_H
#define TOUCHSCREEN_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/* ===================================================================== */
/* Error codes                                                           */
/* ===================================================================== */

typedef enum
{
    TS_ERR_OK = 0,      /**< Operation succeeded. */
    TS_ERR_NO_DATA = 1, /**< No active touch point. */
    TS_ERR_I2C = 2,     /**< Underlying I2C transaction failed. */
    TS_ERR_NULL = 3,    /**< Null pointer argument. */
    TS_ERR_GPIO = 4,    /**< GPIO initialisation error */
} ts_err_t;

/* ===================================================================== */
/* Touch event                                                           */
/* ===================================================================== */

typedef enum
{
    TS_EVENT_PRESS = 0,   /**< New touch point detected. */
    TS_EVENT_RELEASE = 1, /**< Touch point released. */
    TS_EVENT_CONTACT = 2, /**< Ongoing contact (no change from prior read). */
} ts_event_t;

/* ===================================================================== */
/* Touch point                                                           */
/* ===================================================================== */

/**
 * @brief A single touch point reading.
 *
 * Coordinates are in display pixels (0..799 for x, 0..479 for y).
 */
typedef struct
{
    uint16_t x;
    uint16_t y;
    ts_event_t event;
} ts_touch_t;

/* ===================================================================== */
/* Callback type                                                         */
/* ===================================================================== */

/** @brief IRQ callback. Called from EXTI9_5 ISR context. */
typedef void (*ts_irq_cb_t)(void *context);

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

/**
 * @brief Initialise the FT6x06 touchscreen controller.
 *
 * Configures PJ5/EXTI5, writes INT_MODE and TH_GROUP, probes DEV_MODE.
 * EXTI interrupt remains masked until touchscreen_attach_irq().
 * Pre-condition: lcd_init() completed (PH7 released, FT6x06 out of reset).
 *
 * @return TS_ERR_OK or TS_ERR_I2C.
 */
ts_err_t touchscreen_init(void);

/**
 * @brief Register the EXTI5 IRQ callback and enable the interrupt.
 *
 * @param callback  Function called from EXTI9_5 ISR. Must not be NULL.
 * @param context   Opaque pointer passed unchanged to the callback.
 * @return TS_ERR_OK or TS_ERR_NULL.
 */
ts_err_t touchscreen_attach_irq(ts_irq_cb_t callback, void *context);

/**
 * @brief Read the current touch point from the FT6x06.
 *
 * @param[out] touch  Populated on TS_ERR_OK. Must not be NULL.
 * @return TS_ERR_OK / TS_ERR_NO_DATA / TS_ERR_I2C / TS_ERR_NULL.
 */
ts_err_t touchscreen_read(ts_touch_t *touch);

#ifdef TEST
/** @brief Reset driver state for unit tests. */
void touchscreen_reset_for_test(void);
/** @brief EXTI9_5 ISR -- call directly from unit tests to simulate interrupt. */
void EXTI9_5_IRQHandler(void);
#endif /* TEST */

#endif /* TOUCHSCREEN_DRIVER_H */
