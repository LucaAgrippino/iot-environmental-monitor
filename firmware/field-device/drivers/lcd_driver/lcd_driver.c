/**
 * @file lcd_driver.c
 * @brief LcdDriver — thin wrapper over ST BSP_LCD for STM32F469I-DISCO.
 *
 * HAL_Init() is intentionally not called. We do not need:
 *   - SysTick configuration (owned by FreeRTOS);
 *   - NVIC priority grouping (set by FreeRTOS port to group 4);
 *   - FLASH prefetch/cache enable (handled in system_clock_init_core);
 *   - MspInit hook (empty weak symbol).
 * HAL_Delay and HAL_GetTick are redirected to DWT-based overrides in
 * lcd_hal_tick.c; this satisfies BSP's only runtime dependency on the
 * HAL tick subsystem.
 *
 * HAL_DeInit() is intentionally not called — it resets all peripherals
 * and disables SysTick, taking down the LCD and the FreeRTOS tick.
 */

#include "lcd_driver/lcd_driver.h"
#include "lcd_driver_internal.h"

#include "stm32f469xx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ===================================================================== */
/* Clock-enable constants (RCC register bits)                           */
/* ===================================================================== */

#define RCC_APB2ENR_LTDCEN_Pos (26U)
#define RCC_APB2ENR_LTDCEN (1UL << RCC_APB2ENR_LTDCEN_Pos)

#define RCC_APB2ENR_DSIEN_Pos (27U)
#define RCC_APB2ENR_DSIEN (1UL << RCC_APB2ENR_DSIEN_Pos)

#define RCC_AHB1ENR_DMA2DEN_Pos (23U)
#define RCC_AHB1ENR_DMA2DEN (1UL << RCC_AHB1ENR_DMA2DEN_Pos)

/* ===================================================================== */
/* LTDC interrupt constants                                             */
/* ===================================================================== */

#define LTDC_IER_LIE_Pos (0U)
#define LTDC_IER_LIE (1UL << LTDC_IER_LIE_Pos)

#define LTDC_ISR_LIF_Pos (0U)
#define LTDC_ISR_LIF (1UL << LTDC_ISR_LIF_Pos)

#define LTDC_ICR_CLIF_Pos (0U)
#define LTDC_ICR_CLIF (1UL << LTDC_ICR_CLIF_Pos)

#define LCD_IRQ_PRIORITY (6U)

/* ===================================================================== */
/* Stage markers (private in production; exposed in test via .h)        */
/* ===================================================================== */

#ifndef TEST
typedef enum
{
    LCD_STAGE_RESET = 0,
    LCD_STAGE_PERIPH_CLOCKS = 1,
    LCD_STAGE_BSP_INIT = 2,
    LCD_STAGE_BSP_DONE = 3,
    LCD_STAGE_FB_CAPTURED = 4,
    LCD_STAGE_SUCCESS = 5,
    LCD_STAGE_FAIL_BSP = 0x80,
} lcd_init_stage_t;
#endif

/* ===================================================================== */
/* Private state                                                        */
/* ===================================================================== */

typedef struct
{
    uint16_t *framebuffer;
    lcd_frame_done_cb_t frame_done_cb;
    void *frame_done_ctx;
    bool initialised;
} lcd_driver_t;

LCD_TEST_VISIBLE lcd_driver_t s_lcd;
LCD_TEST_VISIBLE volatile lcd_init_stage_t s_lcd_init_stage;

/* ===================================================================== */
/* Public API                                                           */
/* ===================================================================== */

lcd_err_t lcd_init(void)
{
    s_lcd_init_stage = LCD_STAGE_RESET;

    /* Stage 1: enable LTDC, DSI, DMA2D peripheral clocks. */
    RCC->APB2ENR |= RCC_APB2ENR_LTDCEN | RCC_APB2ENR_DSIEN;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA2DEN;
    s_lcd_init_stage = LCD_STAGE_PERIPH_CLOCKS;

    /* Stage 2: hand off DSI/LTDC/OTM8009A initialisation to the BSP. */
    s_lcd_init_stage = LCD_STAGE_BSP_INIT;
    if (BSP_LCD_Init(LCD_ORIENTATION_LANDSCAPE) != 0U)
    {
        s_lcd_init_stage = LCD_STAGE_FAIL_BSP;
        return LCD_ERR_INIT;
    }
    s_lcd_init_stage = LCD_STAGE_BSP_DONE;

    /* Stage 4: capture framebuffer base from the SDRAM driver. */
    s_lcd.framebuffer = (uint16_t *) (uintptr_t) sdram_get_base_addr();
    s_lcd_init_stage = LCD_STAGE_FB_CAPTURED;

    /* Stage 5: mark driver ready. */
    s_lcd.initialised = true;
    s_lcd_init_stage = LCD_STAGE_SUCCESS;

    return LCD_ERR_OK;
}

lcd_err_t lcd_attach_frame_done(lcd_frame_done_cb_t cb, void *ctx)
{
    if (!s_lcd.initialised)
    {
        return LCD_ERR_STATE;
    }
    if (cb == NULL)
    {
        return LCD_ERR_NULL;
    }

    s_lcd.frame_done_cb = cb;
    s_lcd.frame_done_ctx = ctx;

    /* Enable LTDC line interrupt. */
    LTDC->IER |= LTDC_IER_LIE;

    /* Configure NVIC for LTDC line interrupt and LTDC error interrupt. */
    NVIC_SetPriority(LTDC_IRQn, LCD_IRQ_PRIORITY);
    NVIC_EnableIRQ(LTDC_IRQn);
    NVIC_SetPriority(LTDC_ER_IRQn, LCD_IRQ_PRIORITY + 1U);
    NVIC_EnableIRQ(LTDC_ER_IRQn);

    return LCD_ERR_OK;
}

uint16_t *lcd_get_framebuffer(void)
{
    if (!s_lcd.initialised)
    {
        return NULL;
    }
    return s_lcd.framebuffer;
}

lcd_err_t lcd_flush(void)
{
    if (!s_lcd.initialised)
    {
        return LCD_ERR_STATE;
    }
    (void) BSP_LCD_Reload(LCD_RELOAD_VERTICAL_BLANKING);
    return LCD_ERR_OK;
}

/* ===================================================================== */
/* ISR                                                                  */
/* ===================================================================== */

void LTDC_IRQHandler(void)
{
    if (LTDC->ISR & LTDC_ISR_LIF)
    {
        LTDC->ICR = LTDC_ICR_CLIF;
        if (s_lcd.frame_done_cb != NULL)
        {
            s_lcd.frame_done_cb(s_lcd.frame_done_ctx);
        }
    }
}

/* ===================================================================== */
/* Test-only                                                            */
/* ===================================================================== */

#ifdef TEST
void lcd_driver_reset(void)
{
    s_lcd.framebuffer = NULL;
    s_lcd.frame_done_cb = NULL;
    s_lcd.frame_done_ctx = NULL;
    s_lcd.initialised = false;
    s_lcd_init_stage = LCD_STAGE_RESET;
}
#endif /* TEST */
