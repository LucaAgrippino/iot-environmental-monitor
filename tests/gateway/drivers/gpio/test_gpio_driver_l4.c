/**
 * @file test_gpio_driver_l4.c
 * @brief Unit tests for GpioDriver — STM32L475 (Gateway) implementation.
 *
 * Covers the test plan in docs/lld/drivers/gpio-driver.md §7.3, adapted for
 * the L475's eight GPIO ports (GPIOA..GPIOH on RCC->AHB2ENR) versus the
 * F469's eleven. GPIO_PORT_I is used as the "port not on this board" case
 * for the invalid-port validation tests, exercising the L475-specific
 * board-target check (companion §3.2 step 3) rather than a generic
 * out-of-range enum value.
 */

#include "unity.h"

#include "stm32l475_cmsis_mock.h"
#include "stm32l475xx.h"
#include "gpio_driver_l4.h" /* real API + triggers auto-link of gpio_driver_l4.c */

extern void gpio_driver_reset_for_test(void);

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    gpio_driver_reset_for_test();
}

void tearDown(void)
{
}

/* --------------------------------------------------------------------- */
/* Mock infrastructure                                                    */
/* --------------------------------------------------------------------- */

/* Proves: the GPIOA macro resolves to a real, writable, readable backing
 * cell, and the mock storage is volatile-correct. */
void test_mock_gpio_write_read_round_trip(void)
{
    GPIOA->MODER = 0xDEADBEEFu;
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, GPIOA->MODER);
}

/* Proves: stm32l475_cmsis_mock_reset() clears GPIO state between tests. */
void test_mock_reset_clears_gpio_moder(void)
{
    GPIOH->MODER = 0xFFFFFFFFu;
    stm32l475_cmsis_mock_reset();
    TEST_ASSERT_EQUAL_HEX32(0u, GPIOH->MODER);
}

/* Proves: the reset routine also clears RCC AHB2ENR (L4 GPIO clock gate). */
void test_mock_reset_clears_rcc_ahb2enr(void)
{
    RCC->AHB2ENR = RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOHEN;
    stm32l475_cmsis_mock_reset();
    TEST_ASSERT_EQUAL_HEX32(0u, RCC->AHB2ENR);
}

/* --------------------------------------------------------------------- */
/* gpio_init                                                              */
/* --------------------------------------------------------------------- */

void test_gpio_init_succeeds_first_call(void)
{
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_init());
}

void test_gpio_init_idempotent_second_call_returns_ok(void)
{
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_init());
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_init());
}

void test_gpio_init_sets_rcc_ahb2enr_gpio_a_through_h_bits(void)
{
    gpio_init();

    /* 0xFF = bits 0..7 set (GPIOA..GPIOH). L475 has no GPIOI..GPIOK. */
    TEST_ASSERT_EQUAL_HEX32(0xFFu, RCC->AHB2ENR);
}

/* --------------------------------------------------------------------- */
/* gpio_configure_pin                                                     */
/* --------------------------------------------------------------------- */

void test_gpio_configure_pin_null_config_takes_priority_over_not_initialised(void)
{
    /* No gpio_init() called. */
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_NULL_POINTER, gpio_configure_pin(NULL));
}

void test_gpio_configure_pin_returns_not_initialised_before_init(void)
{
    gpio_pin_config_t config = {.alternate = 0,
                                .mode = GPIO_MODE_OUTPUT,
                                .otype = GPIO_OTYPE_PUSH_PULL,
                                .pin = 1,
                                .port = GPIO_PORT_A,
                                .pull = GPIO_PULL_UP,
                                .speed = GPIO_SPEED_LOW};

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_NOT_INITIALISED, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_output_push_pull_succeeds(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port = GPIO_PORT_A,
        .pin = 5,
        .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));

    /* MODER[11:10] = 01 (OUTPUT), all other MODER bits unchanged from reset (0). */
    TEST_ASSERT_EQUAL_HEX32(0x1u << 10, GPIOA->MODER);

    /* OTYPER[5] = 0 (push-pull). */
    TEST_ASSERT_EQUAL_HEX32(0x0u, GPIOA->OTYPER);
}

void test_gpio_configure_pin_input_pull_up_succeeds(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port = GPIO_PORT_B,
        .pin = 3,
        .mode = GPIO_MODE_INPUT,
        .otype = GPIO_OTYPE_OPEN_DRAIN,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_UP,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));

    /* MODER[7:6] = 00 (INPUT), all other MODER bits unchanged from reset (0). */
    TEST_ASSERT_EQUAL_HEX32(0x0u, GPIOB->MODER);

    /* PUPDR[7:6] = 01 (PULL_UP), expected: 0x40. */
    TEST_ASSERT_EQUAL_HEX32(0x40u, GPIOB->PUPDR);
}

void test_gpio_configure_pin_alternate_function_writes_afr_correctly(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port = GPIO_PORT_C,
        .pin = 9,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_OPEN_DRAIN,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0x07u, /* AF7 */
    };

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));

    /* MODER[19:18] = 10 (ALTERNATE). */
    TEST_ASSERT_EQUAL_HEX32(0x2u << 18, GPIOC->MODER);

    /* AFR[0] = pins 0..7 (untouched). */
    TEST_ASSERT_EQUAL_HEX32(0x0u, GPIOC->AFR[0]);

    /* AFR[1] = pins 8..15. Pin 9 is at bits [7:4]. AF7 = 0x7. */
    TEST_ASSERT_EQUAL_HEX32(0x7u << 4, GPIOC->AFR[1]);
}

void test_gpio_configure_pin_clears_mode_bits_before_setting(void)
{
    gpio_init();
    GPIOA->MODER = 0xFFFFFFFFu; /* every pin starts in ANALOGUE state */

    gpio_pin_config_t config = {
        .port = GPIO_PORT_A,
        .pin = 0,
        .mode = GPIO_MODE_OUTPUT, /* value 0x01 — not 0x3, so we can see the clear */
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));

    /* Bits [1:0] = 01 (OUTPUT), bits [31:2] unchanged (still all 1s).
     * Expected: 0xFFFFFFFD = 0b1111...1111_01. */
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFDu, GPIOA->MODER);
}

void test_gpio_configure_pin_writes_moder_last(void)
{
    TEST_IGNORE_MESSAGE("Not host-testable with memory-backed mock; "
                        "ordering enforced by code review per gpio-driver.md §3.2 step 8.");
}

void test_gpio_configure_pin_rejects_null_config(void)
{
    gpio_init();

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_NULL_POINTER, gpio_configure_pin(NULL));
}

void test_gpio_configure_pin_rejects_invalid_port(void)
{
    gpio_init();

    /* GPIO_PORT_I exists in the shared enum (F469-only) but is not present
     * on the L475 — proves the board-target validation, not just a bounds
     * check against GPIO_PORT_COUNT. */
    gpio_pin_config_t config = {
        .port = GPIO_PORT_I,
        .pin = 0,
        .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PORT, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_rejects_pin_above_15(void)
{
    gpio_init();

    gpio_pin_config_t config = {
        .port = GPIO_PORT_A,
        .pin = 16,
        .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PIN, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_rejects_invalid_mode(void)
{
    gpio_init();

    gpio_pin_config_t config = {
        .port = GPIO_PORT_A,
        .pin = 1,
        .mode = GPIO_MODE_ANALOGUE + 1,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_MODE, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_rejects_alternate_above_15(void)
{
    gpio_init();

    gpio_pin_config_t config = {
        .port = GPIO_PORT_A,
        .pin = 1,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 16,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_CONFIG, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_writes_all_attribute_registers(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port = GPIO_PORT_D,
        .pin = 4,
        .mode = GPIO_MODE_OUTPUT,       /* MODER[9:8]   = 01 */
        .otype = GPIO_OTYPE_OPEN_DRAIN, /* OTYPER[4]    = 1  */
        .speed = GPIO_SPEED_HIGH,       /* OSPEEDR[9:8] = 10 */
        .pull = GPIO_PULL_DOWN,         /* PUPDR[9:8]   = 10 */
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));

    TEST_ASSERT_EQUAL_HEX32(0x1u << 8, GPIOD->MODER);
    TEST_ASSERT_EQUAL_HEX32(0x1u << 4, GPIOD->OTYPER);
    TEST_ASSERT_EQUAL_HEX32(0x2u << 8, GPIOD->OSPEEDR);
    TEST_ASSERT_EQUAL_HEX32(0x2u << 8, GPIOD->PUPDR);
}

void test_gpio_configure_pin_accepts_pin_15(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port = GPIO_PORT_A,
        .pin = 15,
        .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_accepts_analogue_mode(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port = GPIO_PORT_A,
        .pin = 0,
        .mode = GPIO_MODE_ANALOGUE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));
    TEST_ASSERT_EQUAL_HEX32(0x3u, GPIOA->MODER);
}

void test_gpio_configure_pin_accepts_af15(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port = GPIO_PORT_A,
        .pin = 0,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 15,
    };
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));
    TEST_ASSERT_EQUAL_HEX32(0xFu, GPIOA->AFR[0]);
}

/* --------------------------------------------------------------------- */
/* gpio_read_pin                                                          */
/* --------------------------------------------------------------------- */

void test_gpio_read_pin_returns_not_initialised_before_init(void)
{
    gpio_level_t out_level = GPIO_LEVEL_LOW;
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_NOT_INITIALISED, gpio_read_pin(GPIO_PORT_A, 3, &out_level));
}

void test_gpio_read_pin_rejects_invalid_port(void)
{
    gpio_init();
    gpio_level_t out_level = GPIO_LEVEL_LOW;
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PORT, gpio_read_pin(GPIO_PORT_I, 5, &out_level));
}

void test_gpio_read_pin_rejects_pin_above_15(void)
{
    gpio_init();
    gpio_level_t out_level = GPIO_LEVEL_LOW;
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PIN, gpio_read_pin(GPIO_PORT_F, 16, &out_level));
}

void test_gpio_read_pin_rejects_null_out_level(void)
{
    gpio_init();
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_NULL_POINTER, gpio_read_pin(GPIO_PORT_A, 0, NULL));
}

void test_gpio_read_pin_high_when_idr_bit_set(void)
{
    gpio_init();
    GPIOH->IDR = 0x800u;

    gpio_level_t out_level = GPIO_LEVEL_LOW;
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_read_pin(GPIO_PORT_H, 11, &out_level));
    TEST_ASSERT_EQUAL_INT(GPIO_LEVEL_HIGH, out_level);
}

void test_gpio_read_pin_low_when_idr_bit_clear(void)
{
    gpio_init();
    GPIOG->IDR = 0x0u;

    gpio_level_t out_level = GPIO_LEVEL_HIGH;
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_read_pin(GPIO_PORT_G, 7, &out_level));
    TEST_ASSERT_EQUAL_INT(GPIO_LEVEL_LOW, out_level);
}

/* --------------------------------------------------------------------- */
/* gpio_write_pin                                                         */
/* --------------------------------------------------------------------- */

void test_gpio_write_pin_high_sets_lower_bsrr_bit(void)
{
    gpio_init();
    /* No pre-load — BSRR is write-only, mock starts at 0. */

    gpio_level_t level = GPIO_LEVEL_HIGH;
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_write_pin(GPIO_PORT_E, 0, level));
    TEST_ASSERT_EQUAL_HEX32(0x1u, GPIOE->BSRR);
}

void test_gpio_write_pin_low_sets_upper_bsrr_bit(void)
{
    gpio_init();
    /* No pre-load — BSRR is write-only, mock starts at 0. */

    gpio_level_t level = GPIO_LEVEL_LOW;
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_write_pin(GPIO_PORT_F, 10, level));
    /* Pin 10 LOW → bit 10+16 = 26 set in BSRR. Expected: 1u << 26 = 0x04000000. */
    TEST_ASSERT_EQUAL_HEX32((1u << 26), GPIOF->BSRR);
}

void test_gpio_write_pin_rejects_invalid_port(void)
{
    gpio_init();

    gpio_level_t level = GPIO_LEVEL_HIGH;
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PORT, gpio_write_pin(GPIO_PORT_I, 4, level));
}

void test_gpio_write_pin_rejects_pin_above_15(void)
{
    gpio_init();

    gpio_level_t level = GPIO_LEVEL_LOW;
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PIN, gpio_write_pin(GPIO_PORT_D, 16, level));
}

void test_gpio_write_pin_returns_not_initialised_before_init(void)
{
    gpio_level_t level = GPIO_LEVEL_LOW;
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_NOT_INITIALISED, gpio_write_pin(GPIO_PORT_A, 1, level));
}

void test_gpio_write_pin_accepts_pin_15(void)
{
    gpio_init();

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_write_pin(GPIO_PORT_C, 15, GPIO_LEVEL_HIGH));
    TEST_ASSERT_EQUAL_HEX32(1u << 15, GPIOC->BSRR);
}

/* --------------------------------------------------------------------- */
/* gpio_toggle_pin                                                        */
/* --------------------------------------------------------------------- */

void test_gpio_toggle_pin_inverts_odr_bit(void)
{
    gpio_init();
    GPIOH->ODR = (1u << 15);

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_toggle_pin(GPIO_PORT_H, 15));
    TEST_ASSERT_EQUAL_HEX32(0x0u, GPIOH->ODR);
}

void test_gpio_toggle_pin_preserves_other_odr_bits(void)
{
    gpio_init();
    GPIOA->ODR = 0xA5A5;

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_toggle_pin(GPIO_PORT_A, 5));
    TEST_ASSERT_EQUAL_HEX32(0xA5A5u ^ (1u << 5), GPIOA->ODR);
}

void test_gpio_toggle_pin_rejects_invalid_port(void)
{
    gpio_init();

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PORT, gpio_toggle_pin(GPIO_PORT_I, 2));
}

void test_gpio_toggle_pin_rejects_pin_above_15(void)
{
    gpio_init();

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PIN, gpio_toggle_pin(GPIO_PORT_F, 16));
}

void test_gpio_toggle_pin_returns_not_initialised_before_init(void)
{
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_NOT_INITIALISED, gpio_toggle_pin(GPIO_PORT_H, 1));
}

void test_gpio_toggle_pin_two_calls_return_to_original(void)
{
    gpio_init();
    GPIOB->ODR = (1u << 5);
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_toggle_pin(GPIO_PORT_B, 5));
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_toggle_pin(GPIO_PORT_B, 5));
    TEST_ASSERT_EQUAL_HEX32((1u << 5), GPIOB->ODR);
}
