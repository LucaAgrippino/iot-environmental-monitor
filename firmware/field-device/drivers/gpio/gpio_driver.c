
#include "gpio_driver.h"
#include "stm32f469xx.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct
{
    bool initialised;                        /**< Set by gpio_init(); guards all entry points. */
    GPIO_TypeDef *port_map[GPIO_PORT_COUNT]; /**< CMSIS peripheral pointer per gpio_port_t. */
    uint32_t clock_bits[GPIO_PORT_COUNT];    /**< RCC AHB enable bit per gpio_port_t. */
} gpio_driver_t;

static gpio_driver_t s_gpio = {
    .port_map = {GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOI, GPIOJ, GPIOK},
    .clock_bits = {RCC_AHB1ENR_GPIOAEN, RCC_AHB1ENR_GPIOBEN, RCC_AHB1ENR_GPIOCEN,
                   RCC_AHB1ENR_GPIODEN, RCC_AHB1ENR_GPIOEEN, RCC_AHB1ENR_GPIOFEN,
                   RCC_AHB1ENR_GPIOGEN, RCC_AHB1ENR_GPIOHEN, RCC_AHB1ENR_GPIOIEN,
                   RCC_AHB1ENR_GPIOJEN, RCC_AHB1ENR_GPIOKEN},
    .initialised = false};

gpio_err_t gpio_init(void)
{
    if (true == s_gpio.initialised)
    {
        // idempotent
        return GPIO_OK;
    }

    // initialize clock
    for (uint8_t i = 0; i < GPIO_PORT_COUNT; i++)
    {
        // enable port clock
        RCC->AHB1ENR |= s_gpio.clock_bits[i];
    }
    // dummy read, just to stabilise the clock
    (void) RCC->AHB1ENR;

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

    if (config->port >= GPIO_PORT_COUNT)
    {
        return GPIO_ERR_INVALID_PORT;
    }

    if (config->pin > 15)
    {
        return GPIO_ERR_INVALID_PIN;
    }

    if (config->mode > GPIO_MODE_ANALOGUE)
    {
        return GPIO_ERR_INVALID_MODE;
    }

    if (GPIO_MODE_ALTERNATE == config->mode && config->alternate > 15)
    {
        return GPIO_ERR_INVALID_CONFIG;
    }

    GPIO_TypeDef *gpio_port = s_gpio.port_map[config->port];

    gpio_port->OTYPER &= ~(1 << config->pin);
    gpio_port->OTYPER |= ((uint32_t) config->otype << config->pin);

    gpio_port->OSPEEDR &= ~(0x3u << (2 * config->pin));
    gpio_port->OSPEEDR |= ((uint32_t) config->speed << (2 * config->pin));

    gpio_port->PUPDR &= ~(0x3u << (2 * config->pin));
    gpio_port->PUPDR |= ((uint32_t) config->pull << (2 * config->pin));

    const uint8_t afr_idx = config->pin / 8u;
    const uint8_t afr_shift = 4u * (config->pin % 8u);
    gpio_port->AFR[afr_idx] &= ~(0xFu << afr_shift);
    gpio_port->AFR[afr_idx] |= ((uint32_t) config->alternate << afr_shift);

    gpio_port->MODER &= ~(0x3u << (2 * config->pin));
    gpio_port->MODER |= ((uint32_t) config->mode << (2 * config->pin));

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

    if (port >= GPIO_PORT_COUNT)
    {
        return GPIO_ERR_INVALID_PORT;
    }

    if (pin > 15)
    {
        return GPIO_ERR_INVALID_PIN;
    }

    GPIO_TypeDef *gpio_port = s_gpio.port_map[port];

    if ((gpio_port->IDR & (1 << pin)) != 0u)
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

    if (port >= GPIO_PORT_COUNT)
    {
        return GPIO_ERR_INVALID_PORT;
    }

    if (pin > 15)
    {
        return GPIO_ERR_INVALID_PIN;
    }

    GPIO_TypeDef *gpio_port = s_gpio.port_map[port];

    const uint8_t shift = (level == GPIO_LEVEL_HIGH) ? pin : (pin + 16u);
    gpio_port->BSRR = (1u << shift);

    return GPIO_OK;
}

gpio_err_t gpio_toggle_pin(gpio_port_t port, uint8_t pin)
{
    /*
     * Reject if s_initialised is false.
       Validate port and pin.
       Read the port's ODR, XOR the pin bit, write back. (Read-modify-write on ODR — see threading
     note in §2.2.) Return GPIO_OK.
     */
    if (false == s_gpio.initialised)
    {
        return GPIO_ERR_NOT_INITIALISED;
    }

    if (port >= GPIO_PORT_COUNT)
    {
        return GPIO_ERR_INVALID_PORT;
    }

    if (pin > 15)
    {
        return GPIO_ERR_INVALID_PIN;
    }

    GPIO_TypeDef *gpio_port = s_gpio.port_map[port];
    gpio_port->ODR ^= (1u << pin);

    return GPIO_OK;
}

#ifdef TEST
void gpio_driver_reset_for_test(void)
{
    s_gpio.initialised = false;
}
#endif
