#include "unity.h"
#include "stm32_cmsis_mock.h"
#include "gpio_driver.h"


extern void gpio_driver_reset_for_test(void);

void setUp(void)
{
    stm32_cmsis_mock_reset();
    gpio_driver_reset_for_test();
}

void tearDown(void)
{
}

/* Proves: the GPIOA macro resolves to a real, writable, readable backing
 * cell, and the mock storage is volatile-correct (host compiler doesn't
 * optimise the write away). */
void test_mock_gpio_write_read_round_trip(void)
{
    GPIOA->MODER = 0xDEADBEEFu;
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, GPIOA->MODER);
}

/* Proves: stm32_cmsis_mock_reset() clears GPIO state between tests.
 * If this fails, GPIO bug 2 (missing RCC zeroing) had a GPIO cousin. */
void test_mock_reset_clears_gpio_moder(void)
{
    GPIOK->MODER = 0xFFFFFFFFu;
    stm32_cmsis_mock_reset();
    TEST_ASSERT_EQUAL_HEX32(0u, GPIOK->MODER);
}

/* Proves: the RCC fix works. Catches the bug we just patched. */
void test_mock_reset_clears_rcc_ahb1enr(void)
{
    RCC->AHB1ENR = RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOHEN;
    stm32_cmsis_mock_reset();
    TEST_ASSERT_EQUAL_HEX32(0u, RCC->AHB1ENR);
}

/* gpio_init set test */

/* Proves: GPIO initialisation succeeds on the first call. */
void test_gpio_init_succeeds_first_call(void)
{
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_init());
}

/* Proves: GPIO initialisation idempotent after the first call. */
void test_gpio_init_idempotent_second_call_returns_ok(void)
{
	TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_init());
	TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_init());
}

/* Proves: The clock initialization for each gpio port*/
void test_gpio_init_sets_rcc_ahb1enr_gpio_a_through_k_bits(void)
{
	gpio_init();

	// 0x7FF = first 11 bits setted
	TEST_ASSERT_EQUAL_HEX32(0x7FF, RCC->AHB1ENR);
}

/* configure_pin set test */
void test_gpio_configure_pin_null_config_takes_priority_over_not_initialised(void)
{
    /* No gpio_init() called. */
    TEST_ASSERT_EQUAL_INT(GPIO_ERR_NULL_POINTER, gpio_configure_pin(NULL));
}

void test_gpio_configure_pin_returns_not_initialised_before_init(void)
{

	gpio_pin_config_t config =
	{
			.alternate = 0,
			.mode = GPIO_MODE_OUTPUT,
			.otype = GPIO_OTYPE_PUSH_PULL,
			.pin = 1,
			.port = GPIO_PORT_A,
			.pull = GPIO_PULL_UP,
			.speed = GPIO_SPEED_LOW
	};

	TEST_ASSERT_EQUAL_INT(GPIO_ERR_NOT_INITIALISED, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_output_push_pull_succeeds(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port      = GPIO_PORT_A,
        .pin       = 5,
        .mode      = GPIO_MODE_OUTPUT,
        .otype     = GPIO_OTYPE_PUSH_PULL,
        .speed     = GPIO_SPEED_LOW,
        .pull      = GPIO_PULL_NONE,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));

    /* MODER[11:10] = 01 (OUTPUT), all other MODER bits unchanged from reset (0) */
    TEST_ASSERT_EQUAL_HEX32(0x1u << 10, GPIOA->MODER);

    /* OTYPER[5] = 0 (push-pull) */
    TEST_ASSERT_EQUAL_HEX32(0x0u, GPIOA->OTYPER);
}

void test_gpio_configure_pin_input_pull_up_succeeds(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port      = GPIO_PORT_B,
        .pin       = 3,
        .mode      = GPIO_MODE_INPUT,
        .otype     = GPIO_OTYPE_OPEN_DRAIN,
        .speed     = GPIO_SPEED_LOW,
        .pull      = GPIO_PULL_UP,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));

    /* MODER[11:10] = 00 (INPUT), all other MODER bits unchanged from reset (0) */
    TEST_ASSERT_EQUAL_HEX32(0x0u, GPIOB->MODER);

    /* PUPDR[7:6] = 01 (PULL_UP), expected: 0x40 */
    TEST_ASSERT_EQUAL_HEX32(0x40, GPIOB->PUPDR);
}

void test_gpio_configure_pin_alternate_function_writes_afr_correctly(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port      = GPIO_PORT_A,
        .pin       = 9,
        .mode      = GPIO_MODE_ALTERNATE,
        .otype     = GPIO_OTYPE_OPEN_DRAIN,
        .speed     = GPIO_SPEED_LOW,
        .pull      = GPIO_PULL_NONE,
        .alternate = 0x07u, // AF7
    };

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));

    /* MODER[19:18] = 10 (ALTERNATE) */
    TEST_ASSERT_EQUAL_HEX32(0x2u << 18, GPIOA->MODER);


    /* AFR[0] = pins 0..7 (untouched). */
    TEST_ASSERT_EQUAL_HEX32(0x0u, GPIOA->AFR[0]);

    /* AFR[1] = pins 8..15. Pin 9 is at bits [7:4]. AF7 = 0x7. */
    TEST_ASSERT_EQUAL_HEX32(0x7u << 4, GPIOA->AFR[1]);
}

void test_gpio_configure_pin_clears_mode_bits_before_setting(void)
{
    gpio_init();
    GPIOA->MODER = 0xFFFFFFFFu;     /* every pin starts in ANALOGUE state */

    gpio_pin_config_t config = {
        .port      = GPIO_PORT_A,
        .pin       = 0,
        .mode      = GPIO_MODE_OUTPUT,  /* value 0x01 — not 0x3, so we can see the clear */
        .otype     = GPIO_OTYPE_PUSH_PULL,
        .speed     = GPIO_SPEED_LOW,
        .pull      = GPIO_PULL_NONE,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));

    /* Bits [1:0] = 01 (OUTPUT), bits [31:2] unchanged (still all 1s).
     * Expected: 0xFFFFFFFD = 0b1111...1111_01 */
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

    gpio_pin_config_t config = {
        .port      = GPIO_PORT_COUNT,
        .pin       = 0,
        .mode      = GPIO_MODE_OUTPUT,
        .otype     = GPIO_OTYPE_PUSH_PULL,
        .speed     = GPIO_SPEED_LOW,
        .pull      = GPIO_PULL_NONE,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PORT, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_rejects_pin_above_15(void)
{
    gpio_init();

    gpio_pin_config_t config = {
        .port      = GPIO_PORT_A,
        .pin       = 16,
        .mode      = GPIO_MODE_OUTPUT,
        .otype     = GPIO_OTYPE_PUSH_PULL,
        .speed     = GPIO_SPEED_LOW,
        .pull      = GPIO_PULL_NONE,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PIN, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_rejects_invalid_mode(void)
{
    gpio_init();

    gpio_pin_config_t config = {
        .port      = GPIO_PORT_A,
        .pin       = 1,
        .mode      = GPIO_MODE_ANALOGUE + 1,
        .otype     = GPIO_OTYPE_PUSH_PULL,
        .speed     = GPIO_SPEED_LOW,
        .pull      = GPIO_PULL_NONE,
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_MODE, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_rejects_alternate_above_15(void)
{
    gpio_init();

    gpio_pin_config_t config = {
        .port      = GPIO_PORT_A,
        .pin       = 1,
        .mode      = GPIO_MODE_ALTERNATE,
        .otype     = GPIO_OTYPE_PUSH_PULL,
        .speed     = GPIO_SPEED_LOW,
        .pull      = GPIO_PULL_NONE,
        .alternate = 16,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_CONFIG, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_writes_all_attribute_registers(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port      = GPIO_PORT_C,
        .pin       = 4,
        .mode      = GPIO_MODE_OUTPUT,        /* MODER[9:8] = 01 */
        .otype     = GPIO_OTYPE_OPEN_DRAIN,   /* OTYPER[4]  = 1  */
        .speed     = GPIO_SPEED_HIGH,         /* OSPEEDR[9:8] = 10 */
        .pull      = GPIO_PULL_DOWN,          /* PUPDR[9:8]   = 10 */
        .alternate = 0,
    };

    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));

    TEST_ASSERT_EQUAL_HEX32(0x1u << 8, GPIOC->MODER);
    TEST_ASSERT_EQUAL_HEX32(0x1u << 4, GPIOC->OTYPER);
    TEST_ASSERT_EQUAL_HEX32(0x2u << 8, GPIOC->OSPEEDR);
    TEST_ASSERT_EQUAL_HEX32(0x2u << 8, GPIOC->PUPDR);
}

void test_gpio_configure_pin_accepts_pin_15(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port = GPIO_PORT_A, .pin = 15, .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL, .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE, .alternate = 0,
    };
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));
}

void test_gpio_configure_pin_accepts_analogue_mode(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port = GPIO_PORT_A, .pin = 0, .mode = GPIO_MODE_ANALOGUE,
        .otype = GPIO_OTYPE_PUSH_PULL, .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE, .alternate = 0,
    };
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));
    TEST_ASSERT_EQUAL_HEX32(0x3u, GPIOA->MODER);
}

void test_gpio_configure_pin_accepts_af15(void)
{
    gpio_init();
    gpio_pin_config_t config = {
        .port = GPIO_PORT_A, .pin = 0, .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL, .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE, .alternate = 15,
    };
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_configure_pin(&config));
    TEST_ASSERT_EQUAL_HEX32(0xFu, GPIOA->AFR[0]);
}

/* read_pin set test */
void test_gpio_read_pin_returns_not_initialised_before_init(void)
{
	gpio_level_t out_level = GPIO_LEVEL_UNDEF;
	TEST_ASSERT_EQUAL_INT(GPIO_ERR_NOT_INITIALISED, gpio_read_pin(GPIO_PORT_A, 3, &out_level));
}

void test_gpio_read_pin_rejects_invalid_port(void)
{
	gpio_init();
	gpio_level_t out_level = GPIO_LEVEL_UNDEF;
	TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PORT, gpio_read_pin(GPIO_PORT_COUNT, 5, &out_level));
}

void test_gpio_read_pin_rejects_pin_above_15(void)
{
	gpio_init();
	gpio_level_t out_level = GPIO_LEVEL_UNDEF;
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
	GPIOK->IDR = 0x800u;

	gpio_level_t out_level = GPIO_LEVEL_UNDEF;
	TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_read_pin(GPIO_PORT_K, 11, &out_level));
	TEST_ASSERT_EQUAL_INT(GPIO_LEVEL_HIGH, out_level);
}

void test_gpio_read_pin_low_when_idr_bit_clear(void)
{
	gpio_init();
	GPIOI->IDR = 0x0u;

	gpio_level_t out_level = GPIO_LEVEL_UNDEF;
	TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_read_pin(GPIO_PORT_I, 7, &out_level));
	TEST_ASSERT_EQUAL_INT(GPIO_LEVEL_LOW, out_level);
}

/* write_pin set test */
void test_gpio_write_pin_high_sets_lower_bsrr_bit(void)
{
	gpio_init();
	/* No pre-load — BSRR is write-only, mock starts at 0. */

	gpio_level_t level = GPIO_LEVEL_HIGH;
	TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_write_pin(GPIO_PORT_G, 0, level));
	TEST_ASSERT_EQUAL_HEX32(0x1u, GPIOG->BSRR);
}

void test_gpio_write_pin_low_sets_upper_bsrr_bit(void)
{
	gpio_init();
	/* No pre-load — BSRR is write-only, mock starts at 0. */

	gpio_level_t level = GPIO_LEVEL_LOW;
	TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_write_pin(GPIO_PORT_J, 10, level));
	/* Pin 10 LOW → bit 10+16 = 26 set in BSRR. Expected: 1u << 26 = 0x04000000. */
	TEST_ASSERT_EQUAL_HEX32((1u<<26), GPIOJ->BSRR);
}

void test_gpio_write_pin_rejects_invalid_port(void)
{
	gpio_init();

	gpio_level_t level = GPIO_LEVEL_HIGH;
	TEST_ASSERT_EQUAL_INT(GPIO_ERR_INVALID_PORT, gpio_write_pin(GPIO_PORT_COUNT, 4, level));

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
