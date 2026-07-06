/**
 * @file exti_driver.c
 * @brief ExtiDriver implementation — shared between both boards.
 *
 * The only platform-conditional code is the register-name alias block in
 * §3.4 of the companion: STM32L475 exposes IMR1/RTSR1/FTSR1/PR1 (multi-bank
 * EXTI), STM32F469 exposes the bare IMR/RTSR/FTSR/PR. Everything else is
 * identical across both boards.
 *
 * @note See docs/lld/drivers/exti-driver.md for the full design specification.
 */

#include "exti_driver.h"

#include <stddef.h>
#include <string.h>

#if defined(STM32L475xx)
#include "stm32l475xx.h"
#elif defined(STM32F469xx)
#include "stm32f469xx.h"
#else
#error "ExtiDriver: unsupported target. Define STM32L475xx or STM32F469xx."
#endif

#if defined(STM32L475xx)
#define EXTI_IMR EXTI->IMR1
#define EXTI_PR EXTI->PR1
#define EXTI_RTSR EXTI->RTSR1
#define EXTI_FTSR EXTI->FTSR1
#elif defined(STM32F469xx)
#define EXTI_IMR EXTI->IMR
#define EXTI_PR EXTI->PR
#define EXTI_RTSR EXTI->RTSR
#define EXTI_FTSR EXTI->FTSR
#endif

#define EXTI_LINE_COUNT (16u)
#define EXTI_PORT_MAX (EXTI_PORT_H)
#define EXTI_EXTICR_LINES_PER_REG (4u)
#define EXTI_EXTICR_FIELD_WIDTH (4u)
#define EXTI_EXTICR_FIELD_MASK (0xFUL)

/** Bit N set = EXTI line N has been configured via exti_configure(). */
static uint16_t s_configured = 0U;

static IRQn_Type prv_line_to_irqn(uint8_t line)
{
    IRQn_Type irqn;

    switch (line)
    {
    case 0U:
        irqn = EXTI0_IRQn;
        break;
    case 1U:
        irqn = EXTI1_IRQn;
        break;
    case 2U:
        irqn = EXTI2_IRQn;
        break;
    case 3U:
        irqn = EXTI3_IRQn;
        break;
    case 4U:
        irqn = EXTI4_IRQn;
        break;
    default:
        if (line <= 9U)
        {
            irqn = EXTI9_5_IRQn;
        }
        else
        {
            irqn = EXTI15_10_IRQn;
        }
        break;
    }

    return irqn;
}

exti_err_t exti_configure(uint8_t line, exti_port_t port, exti_edge_t edge)
{
    if ((line >= EXTI_LINE_COUNT) || (port > EXTI_PORT_MAX) || (edge > EXTI_EDGE_BOTH))
    {
        return EXTI_ERR_INVALID_ARG;
    }

    if ((s_configured & (1U << line)) != 0U)
    {
        return EXTI_ERR_CONFLICT;
    }

    if ((RCC->APB2ENR & RCC_APB2ENR_SYSCFGEN) == 0U)
    {
        RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
        (void) RCC->APB2ENR; /* dummy read: let the clock stabilise */
    }

    uint8_t reg_idx = line / EXTI_EXTICR_LINES_PER_REG;
    uint8_t bit_pos = (line % EXTI_EXTICR_LINES_PER_REG) * EXTI_EXTICR_FIELD_WIDTH;

    SYSCFG->EXTICR[reg_idx] &= ~(EXTI_EXTICR_FIELD_MASK << bit_pos);
    SYSCFG->EXTICR[reg_idx] |= ((uint32_t) port << bit_pos);

    if ((edge == EXTI_EDGE_RISING) || (edge == EXTI_EDGE_BOTH))
    {
        EXTI_RTSR |= (1UL << line);
    }
    else
    {
        EXTI_RTSR &= ~(1UL << line);
    }

    if ((edge == EXTI_EDGE_FALLING) || (edge == EXTI_EDGE_BOTH))
    {
        EXTI_FTSR |= (1UL << line);
    }
    else
    {
        EXTI_FTSR &= ~(1UL << line);
    }

    s_configured |= (uint16_t) (1U << line);

    return EXTI_ERR_OK;
}

exti_err_t exti_enable(uint8_t line, uint32_t nvic_priority)
{
    if (line >= EXTI_LINE_COUNT)
    {
        return EXTI_ERR_INVALID_ARG;
    }

    if ((s_configured & (1U << line)) == 0U)
    {
        return EXTI_ERR_NOT_CONFIGURED;
    }

    IRQn_Type irqn = prv_line_to_irqn(line);

    NVIC_SetPriority(irqn, nvic_priority);
    NVIC_EnableIRQ(irqn);
    EXTI_IMR |= (1UL << line);

    return EXTI_ERR_OK;
}

exti_err_t exti_disable(uint8_t line)
{
    if (line >= EXTI_LINE_COUNT)
    {
        return EXTI_ERR_INVALID_ARG;
    }

    if ((s_configured & (1U << line)) == 0U)
    {
        return EXTI_ERR_NOT_CONFIGURED;
    }

    EXTI_IMR &= ~(1UL << line);
    NVIC_DisableIRQ(prv_line_to_irqn(line));

    return EXTI_ERR_OK;
}

void exti_clear_pending(uint8_t line)
{
    EXTI_PR = (1UL << line);
}

#ifdef TEST
void exti_reset_for_test(void)
{
    s_configured = 0U;
}
#endif
