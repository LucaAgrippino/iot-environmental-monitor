/**
 * @file gpio_driver.c
 * @brief GpioDriver implementation for STM32L475 (B-L475E-IOT01A, Gateway).
 *
 * Implements the IGpio interface declared in gpio_driver.h. Same filename
 * as the Field Device's implementation (firmware/field-device/drivers/gpio/) —
 * that is fine for the real per-board embedded builds, which never compile
 * both trees together. Ceedling's host test project does, though, so this
 * module's tests run under the dedicated tests/project_gateway.yml rather
 * than the shared tests/project.yml (see GPIO-O4).
 *
 * The L475 exposes eight GPIO ports (GPIOA..GPIOH) gated by RCC->AHB2ENR,
 * versus the F469's eleven ports on AHB1ENR. GPIO_PORT_I/J/K exist in the
 * shared enum for the F469 build only and are rejected here as
 * GPIO_ERR_INVALID_PORT.
 *
 * @note See docs/lld/drivers/gpio-driver.md for the full design specification.
 */

#include "gpio_driver.h"
#include "stm32l475xx.h"

#include <stdbool.h>
#include <stddef.h>

/** Number of GPIO ports present on the STM32L475 (GPIOA..GPIOH). */
#define GPIO_L4_PORT_COUNT (8u)

typedef struct
{
    bool initialised;                           /**< Set by gpio_init(); guards all entry points. */
    GPIO_TypeDef *port_map[GPIO_L4_PORT_COUNT]; /**< CMSIS peripheral pointer per gpio_port_t. */
    uint32_t clock_bits[GPIO_L4_PORT_COUNT];    /**< RCC AHB2ENR enable bit per gpio_port_t. */
} gpio_driver_t;

static gpio_driver_t s_gpio = {.port_map = {GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH},
                               .clock_bits = {RCC_AHB2ENR_GPIOAEN, RCC_AHB2ENR_GPIOBEN,
                                              RCC_AHB2ENR_GPIOCEN, RCC_AHB2ENR_GPIODEN,
                                              RCC_AHB2ENR_GPIOEEN, RCC_AHB2ENR_GPIOFEN,
                                              RCC_AHB2ENR_GPIOGEN, RCC_AHB2ENR_GPIOHEN},
                               .initialised = false};

gpio_err_t gpio_init(void)
{
    if (true == s_gpio.initialised)
    {
        return GPIO_OK;
    }

    for (uint8_t i = 0; i < GPIO_L4_PORT_COUNT; i++)
    {
        RCC->AHB2ENR |= s_gpio.clock_bits[i];
    }
    /* Dummy read, per RM0351, to let the clock stabilise before any
     * GPIO register access. */
    (void) RCC->AHB2ENR;

    s_gpio.initialised = true;

    return GPIO_OK;
}

gpio_err_t gpio_configure_pin(const gpio_pin_config_t *config)
{
    if (NULL == config)
    {
        return GPIO_ERR_NULL_POINTER;
    }

    if (false == s_gpio.initialised)
    {
        return GPIO_ERR_NOT_INITIALISED;
    }

    if (config->port >= GPIO_L4_PORT_COUNT)
    {
        return GPIO_ERR_INVALID_PORT;
    }

    if (config->pin > 15u)
    {
        return GPIO_ERR_INVALID_PIN;
    }

    if (config->mode > GPIO_MODE_ANALOGUE)
    {
        return GPIO_ERR_INVALID_MODE;
    }

    if ((GPIO_MODE_ALTERNATE == config->mode) && (config->alternate > 15u))
    {
        return GPIO_ERR_INVALID_CONFIG;
    }

    GPIO_TypeDef *const gpio_port = s_gpio.port_map[config->port];

    gpio_port->OTYPER &= ~(1u << config->pin);
    gpio_port->OTYPER |= ((uint32_t) config->otype << config->pin);

    gpio_port->OSPEEDR &= ~(0x3u << (2u * config->pin));
    gpio_port->OSPEEDR |= ((uint32_t) config->speed << (2u * config->pin));

    gpio_port->PUPDR &= ~(0x3u << (2u * config->pin));
    gpio_port->PUPDR |= ((uint32_t) config->pull << (2u * config->pin));

    const uint8_t afr_idx = config->pin / 8u;
    const uint8_t afr_shift = 4u * (config->pin % 8u);
    gpio_port->AFR[afr_idx] &= ~(0xFu << afr_shift);
    gpio_port->AFR[afr_idx] |= ((uint32_t) config->alternate << afr_shift);

    /* MODER written last so the pin is never transiently mis-driven while
     * its other attributes are still being applied (companion §3.2 step 8). */
    gpio_port->MODER &= ~(0x3u << (2u * config->pin));
    gpio_port->MODER |= ((uint32_t) config->mode << (2u * config->pin));

    return GPIO_OK;
}

gpio_err_t gpio_read_pin(gpio_port_t port, uint8_t pin, gpio_level_t *out_level)
{
    if (NULL == out_level)
    {
        return GPIO_ERR_NULL_POINTER;
    }

    if (false == s_gpio.initialised)
    {
        return GPIO_ERR_NOT_INITIALISED;
    }

    if (port >= GPIO_L4_PORT_COUNT)
    {
        return GPIO_ERR_INVALID_PORT;
    }

    if (pin > 15u)
    {
        return GPIO_ERR_INVALID_PIN;
    }

    const GPIO_TypeDef *gpio_port = s_gpio.port_map[port];

    if ((gpio_port->IDR & (1u << pin)) != 0u)
    {
        *out_level = GPIO_LEVEL_HIGH;
    }
    else
    {
        *out_level = GPIO_LEVEL_LOW;
    }

    return GPIO_OK;
}

gpio_err_t gpio_write_pin(gpio_port_t port, uint8_t pin, gpio_level_t level)
{
    if (false == s_gpio.initialised)
    {
        return GPIO_ERR_NOT_INITIALISED;
    }

    if (port >= GPIO_L4_PORT_COUNT)
    {
        return GPIO_ERR_INVALID_PORT;
    }

    if (pin > 15u)
    {
        return GPIO_ERR_INVALID_PIN;
    }

    GPIO_TypeDef *const gpio_port = s_gpio.port_map[port];

    const uint8_t shift = (GPIO_LEVEL_HIGH == level) ? pin : (pin + 16u);
    gpio_port->BSRR = (1u << shift);

    return GPIO_OK;
}

gpio_err_t gpio_toggle_pin(gpio_port_t port, uint8_t pin)
{
    if (false == s_gpio.initialised)
    {
        return GPIO_ERR_NOT_INITIALISED;
    }

    if (port >= GPIO_L4_PORT_COUNT)
    {
        return GPIO_ERR_INVALID_PORT;
    }

    if (pin > 15u)
    {
        return GPIO_ERR_INVALID_PIN;
    }

    GPIO_TypeDef *const gpio_port = s_gpio.port_map[port];
    gpio_port->ODR ^= (1u << pin);

    return GPIO_OK;
}

#ifdef TEST
void gpio_driver_reset_for_test(void)
{
    s_gpio.initialised = false;
}
#endif
