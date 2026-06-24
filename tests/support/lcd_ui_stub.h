/**
 * @file lcd_ui_stub.h
 * @brief Narrow stub for ILcdUi in LifecycleController unit tests.
 *
 * Provides ilcd_ui_t vtable with the methods LifecycleController calls:
 * init(), show_splash(), dismiss_splash(). Prevents Ceedling from
 * auto-linking lcd_ui.c (which cascades to LVGL and BSP code).
 */

#ifndef LCD_UI_STUB_H
#define LCD_UI_STUB_H

#include <stdint.h>

typedef enum
{
    LCD_UI_ERR_OK       = 0,
    LCD_UI_ERR_NOT_INIT = 1,
    LCD_UI_ERR_NULL_ARG = 2,
    LCD_UI_ERR_DRIVER   = 3,
} lcd_ui_err_t;

typedef struct
{
    lcd_ui_err_t (*init)(void);
    lcd_ui_err_t (*show_splash)(void);
    lcd_ui_err_t (*dismiss_splash)(void);
} ilcd_ui_t;

#endif /* LCD_UI_STUB_H */
