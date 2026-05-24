
#include "gpio_driver.h"
#include <stdbool.h>
#include "stm32f469xx.h"



typedef struct {
    bool          initialised;                  /**< Set by gpio_init(); guards all entry points. */
    GPIO_TypeDef *port_map[GPIO_PORT_COUNT];    /**< CMSIS peripheral pointer per gpio_port_t. */
    uint32_t      clock_bits[GPIO_PORT_COUNT];  /**< RCC AHB enable bit per gpio_port_t. */
} gpio_driver_t;

static gpio_driver_t s_gpio = {
		.port_map = {
				GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOI, GPIOJ, GPIOK
		},
		.clock_bits = {
		    RCC_AHB1ENR_GPIOAEN, RCC_AHB1ENR_GPIOBEN, RCC_AHB1ENR_GPIOCEN,
		    RCC_AHB1ENR_GPIODEN, RCC_AHB1ENR_GPIOEEN, RCC_AHB1ENR_GPIOFEN,
		    RCC_AHB1ENR_GPIOGEN, RCC_AHB1ENR_GPIOHEN, RCC_AHB1ENR_GPIOIEN,
		    RCC_AHB1ENR_GPIOJEN, RCC_AHB1ENR_GPIOKEN
		},
		.initialised = false
};




gpio_err_t gpio_init(void)
{
	if(true == s_gpio.initialised)
	{
		// idempotent
		return GPIO_OK;
	}

	// initialize clock
	for (uint8_t i = 0; i< GPIO_PORT_COUNT; i++)
	{
		// enable port clock
		RCC->AHB1ENR |= s_gpio.clock_bits[i];
	}
	// dummy read, just to stabilise the clock
	(void)RCC->AHB1ENR;

	s_gpio.initialised = true;

	return GPIO_OK;
}


#ifdef TEST
void gpio_driver_reset_for_test(void)
{
    s_gpio.initialised = false;
}
#endif
