/**
 * @file test_modbus_uart_driver.c
 * @brief Unity unit tests for ModbusUartDriver — Gateway (STM32L475, UART4).
 *
 * Compiled with -DSTM32L475xx against the L475 CMSIS mock (g_mock_uart4).
 * The ISR is exercised by calling UART4_IRQHandler() directly with the mock
 * register bank in predetermined states.
 *
 * Test IDs: T-MBUART-01..11 (companion §7) plus T-MBUART-12..19 for the
 * Gateway-specific behaviour added in the companion's Gateway section.
 */

#include "unity.h"

#include "modbus_uart_driver.h"
#include "stm32l475_cmsis_mock.h"

extern void UART4_IRQHandler(void);

/* ===================================================================== */
/* Fixtures                                                              */
/* ===================================================================== */

#define TEST_BRR_9600_AT_80MHZ (8333UL)

static uint32_t s_test_ms_value;
static uint32_t s_reentrant_calls;
static modbus_uart_err_t s_reentrant_result;
static const uint8_t k_reentrant_frame[1] = {0x55U};

static struct
{
    uint32_t invocation_count;
    modbus_uart_event_t last_event;
    void *last_context;
} s_cb_capture;

static uint32_t test_get_ms_auto_advance(void)
{
    return s_test_ms_value++;
}

/** Tick source that re-enters modbus_uart_transmit() from inside the TXE wait. */
static uint32_t test_get_ms_reentrant(void)
{
    if (s_reentrant_calls == 0U)
    {
        s_reentrant_calls++;
        s_reentrant_result = modbus_uart_transmit(k_reentrant_frame, 1U);
    }
    return s_test_ms_value++;
}

static void test_rx_callback(modbus_uart_event_t event, void *context)
{
    s_cb_capture.invocation_count++;
    s_cb_capture.last_event = event;
    s_cb_capture.last_context = context;
}

static void feed_byte(uint8_t byte)
{
    UART4->ISR = USART_ISR_RXNE;
    UART4->RDR = byte;
    UART4_IRQHandler();
}

static void fire_idle(void)
{
    UART4->ISR = USART_ISR_IDLE;
    UART4_IRQHandler();
}

static void init_and_attach(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());
    modbus_uart_attach_rx(test_rx_callback, NULL);
}

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    modbus_uart_reset_for_test();
    s_test_ms_value = 0U;
    s_reentrant_calls = 0U;
    s_reentrant_result = MODBUS_UART_OK;
    s_cb_capture.invocation_count = 0U;
    s_cb_capture.last_event = MODBUS_UART_EVENT_RX_DONE;
    s_cb_capture.last_context = NULL;
}

void tearDown(void)
{
}

/* ===================================================================== */
/* T-MBUART-01: init configures UART4 8N1, DEM, BRR, pins                */
/* ===================================================================== */

void test_T_MBUART_01_init_configures_uart4_8n1_dem_brr(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());

    TEST_ASSERT_BITS_HIGH(RCC_AHB2ENR_GPIOAEN, RCC->AHB2ENR);
    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR1_UART4EN, RCC->APB1ENR1);

    /* UE and TE set; RE and the RX interrupts are NOT enabled until attach. */
    TEST_ASSERT_BITS_HIGH(USART_CR1_UE | USART_CR1_TE, UART4->CR1);
    TEST_ASSERT_BITS_LOW(USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_IDLEIE, UART4->CR1);

    /* Hardware RS-485 DE mode. */
    TEST_ASSERT_BITS_HIGH(USART_CR3_DEM, UART4->CR3);

    TEST_ASSERT_EQUAL_UINT32(TEST_BRR_9600_AT_80MHZ, UART4->BRR);
}

void test_T_MBUART_12_init_routes_pa0_pa1_pa15_to_af8(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());

    /* MODER = alternate function (0b10) for pins 0, 1 and 15. */
    TEST_ASSERT_EQUAL_UINT32(0x2UL, (GPIOA->MODER >> 0U) & 0x3UL);
    TEST_ASSERT_EQUAL_UINT32(0x2UL, (GPIOA->MODER >> 2U) & 0x3UL);
    TEST_ASSERT_EQUAL_UINT32(0x2UL, (GPIOA->MODER >> 30U) & 0x3UL);

    /* AF8: PA0/PA1 in AFRL nibbles 0 and 1, PA15 in AFRH nibble 7. */
    TEST_ASSERT_EQUAL_UINT32(0x8UL, (GPIOA->AFR[0] >> 0U) & 0xFUL);
    TEST_ASSERT_EQUAL_UINT32(0x8UL, (GPIOA->AFR[0] >> 4U) & 0xFUL);
    TEST_ASSERT_EQUAL_UINT32(0x8UL, (GPIOA->AFR[1] >> 28U) & 0xFUL);
}

void test_T_MBUART_13_init_leaves_other_pa_pins_untouched(void)
{
    GPIOA->MODER = 0xFFFFFFFFUL;
    GPIOA->AFR[0] = 0x11111111UL;
    GPIOA->AFR[1] = 0x11111111UL;
    GPIOA->PUPDR = 0xFFFFFFFFUL;

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());

    /* Pin 2 (MODER bits 5:4, AFRL nibble 2) is not ours. */
    TEST_ASSERT_EQUAL_UINT32(0x3UL, (GPIOA->MODER >> 4U) & 0x3UL);
    TEST_ASSERT_EQUAL_UINT32(0x1UL, (GPIOA->AFR[0] >> 8U) & 0xFUL);
    TEST_ASSERT_EQUAL_UINT32(0x1UL, (GPIOA->AFR[1] >> 24U) & 0xFUL);

    /* Pull-ups cleared only on PA0, PA1 and PA15. */
    TEST_ASSERT_EQUAL_UINT32(0x0UL, (GPIOA->PUPDR >> 30U) & 0x3UL);
    TEST_ASSERT_EQUAL_UINT32(0x3UL, (GPIOA->PUPDR >> 4U) & 0x3UL);
}

/* ===================================================================== */
/* T-MBUART-02: attach_rx                                                */
/* ===================================================================== */

void test_T_MBUART_02_attach_rx_enables_re_rxneie_idleie_and_nvic(void)
{
    int marker = 0;

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());
    modbus_uart_attach_rx(test_rx_callback, &marker);

    TEST_ASSERT_BITS_HIGH(USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_IDLEIE, UART4->CR1);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_nvic_enable_count[UART4_IRQn]);

    /* The registered callback and context are the ones the ISR uses. */
    feed_byte(0x01U);
    fire_idle();
    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_capture.invocation_count);
    TEST_ASSERT_EQUAL_PTR(&marker, s_cb_capture.last_context);
}

void test_T_MBUART_14_attach_rx_null_callback_is_ignored(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());
    modbus_uart_attach_rx(NULL, NULL);

    TEST_ASSERT_BITS_LOW(USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_IDLEIE, UART4->CR1);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_nvic_enable_count[UART4_IRQn]);
}

/* ===================================================================== */
/* T-MBUART-03..05: transmit                                             */
/* ===================================================================== */

void test_T_MBUART_03_transmit_happy_path_8_bytes(void)
{
    const uint8_t frame[8] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x02U, 0xC4U, 0x0BU};

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());
    UART4->ISR = USART_ISR_TXE | USART_ISR_TC;

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_transmit(frame, 8U));
    TEST_ASSERT_EQUAL_HEX8(0x0BU, (uint8_t) UART4->TDR);
}

void test_T_MBUART_04_transmit_txe_timeout(void)
{
    const uint8_t frame[1] = {0xAAU};

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());
    modbus_uart_set_tick_source(test_get_ms_auto_advance);

    /* TXE never asserts. */
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_TIMEOUT, modbus_uart_transmit(frame, 1U));
}

void test_T_MBUART_05_transmit_tc_timeout(void)
{
    const uint8_t frame[1] = {0xBBU};

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());
    modbus_uart_set_tick_source(test_get_ms_auto_advance);

    /* Byte accepted, TC never asserts. */
    UART4->ISR = USART_ISR_TXE;

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_TIMEOUT, modbus_uart_transmit(frame, 1U));
    TEST_ASSERT_EQUAL_HEX8(0xBBU, (uint8_t) UART4->TDR);
}

void test_T_MBUART_15_tc_wait_uses_10ms_not_txe_5ms(void)
{
    const uint8_t frame[1] = {0xCCU};

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());
    UART4->ISR = USART_ISR_TXE;
    modbus_uart_set_tick_source(test_get_ms_auto_advance);

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_TIMEOUT, modbus_uart_transmit(frame, 1U));

    /* Byte wait reads the tick twice (start, check). TC wait then runs from
     * its own start until elapsed >= MODBUS_UART_TC_TIMEOUT_MS, so the tick
     * source must have advanced at least that far during the TC wait alone. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(MODBUS_UART_TC_TIMEOUT_MS + 2U, s_test_ms_value);
}

void test_T_MBUART_16_transmit_rejects_invalid_arguments(void)
{
    const uint8_t frame[1] = {0x01U};

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_INVALID_ARG, modbus_uart_transmit(NULL, 1U));
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_INVALID_ARG, modbus_uart_transmit(frame, 0U));
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_INVALID_ARG,
                          modbus_uart_transmit(frame, MODBUS_UART_BUF_SIZE + 1U));
}

void test_T_MBUART_17_transmit_maximum_length_frame(void)
{
    static uint8_t frame[MODBUS_UART_BUF_SIZE];

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());
    UART4->ISR = USART_ISR_TXE | USART_ISR_TC;
    frame[MODBUS_UART_BUF_SIZE - 1U] = 0x5AU;

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_transmit(frame, MODBUS_UART_BUF_SIZE));
    TEST_ASSERT_EQUAL_HEX8(0x5AU, (uint8_t) UART4->TDR);
}

void test_T_MBUART_18_transmit_clears_busy_after_timeout(void)
{
    const uint8_t frame[1] = {0xAAU};

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());
    modbus_uart_set_tick_source(test_get_ms_auto_advance);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_TIMEOUT, modbus_uart_transmit(frame, 1U));

    /* A failed transmit must not leave the driver wedged in BUSY. */
    UART4->ISR = USART_ISR_TXE | USART_ISR_TC;
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_transmit(frame, 1U));
}

/* ===================================================================== */
/* T-MBUART-06..10: ISR and get_rx_frame                                 */
/* ===================================================================== */

void test_T_MBUART_06_isr_rxne_4_bytes_no_callback(void)
{
    const uint8_t bytes[4] = {0x01U, 0x03U, 0x00U, 0x00U};

    init_and_attach();
    for (uint8_t i = 0U; i < 4U; i++)
    {
        feed_byte(bytes[i]);
    }

    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_capture.invocation_count);
}

void test_T_MBUART_07_isr_idle_after_4_bytes_rx_done(void)
{
    const uint8_t bytes[4] = {0x01U, 0x03U, 0xAAU, 0xBBU};
    uint8_t buf[MODBUS_UART_BUF_SIZE];
    uint16_t len = 0U;

    init_and_attach();
    for (uint8_t i = 0U; i < 4U; i++)
    {
        feed_byte(bytes[i]);
    }
    fire_idle();

    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_capture.invocation_count);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_EVENT_RX_DONE, s_cb_capture.last_event);
    TEST_ASSERT_BITS_HIGH(USART_ICR_IDLECF, UART4->ICR);

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_get_rx_frame(buf, &len));
    TEST_ASSERT_EQUAL_UINT16(4U, len);
    TEST_ASSERT_EQUAL_MEMORY(bytes, buf, 4U);
}

void test_T_MBUART_08_isr_ore_calls_rx_error_and_resets_length(void)
{
    uint8_t buf[MODBUS_UART_BUF_SIZE];
    uint16_t len = 0xFFFFU;

    init_and_attach();
    feed_byte(0x01U);

    UART4->ISR = USART_ISR_ORE;
    UART4_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_capture.invocation_count);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_EVENT_RX_ERROR, s_cb_capture.last_event);
    TEST_ASSERT_BITS_HIGH(USART_ICR_ORECF, UART4->ICR);

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_get_rx_frame(buf, &len));
    TEST_ASSERT_EQUAL_UINT16(0U, len);
}

void test_T_MBUART_19_isr_fe_and_ne_each_report_rx_error(void)
{
    init_and_attach();

    UART4->ISR = USART_ISR_FE;
    UART4_IRQHandler();
    TEST_ASSERT_BITS_HIGH(USART_ICR_FECF, UART4->ICR);

    UART4->ISR = USART_ISR_NE;
    UART4_IRQHandler();
    TEST_ASSERT_BITS_HIGH(USART_ICR_NECF, UART4->ICR);

    TEST_ASSERT_EQUAL_UINT32(2U, s_cb_capture.invocation_count);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_EVENT_RX_ERROR, s_cb_capture.last_event);
}

void test_T_MBUART_20_error_with_coincident_idle_never_reports_rx_done(void)
{
    init_and_attach();
    feed_byte(0x01U);

    UART4->ISR = USART_ISR_IDLE | USART_ISR_FE;
    UART4_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_capture.invocation_count);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_EVENT_RX_ERROR, s_cb_capture.last_event);
    TEST_ASSERT_BITS_HIGH(USART_ICR_IDLECF | USART_ICR_FECF, UART4->ICR);
}

void test_T_MBUART_09_isr_buffer_overrun_257_bytes_reports_rx_error(void)
{
    init_and_attach();

    for (uint16_t i = 0U; i < MODBUS_UART_BUF_SIZE; i++)
    {
        feed_byte((uint8_t) i);
    }
    feed_byte(0xFFU); /* 257th byte — discarded */

    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_capture.invocation_count);

    fire_idle();

    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_capture.invocation_count);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_EVENT_RX_ERROR, s_cb_capture.last_event);
}

void test_T_MBUART_21_exactly_256_bytes_is_a_valid_frame(void)
{
    static uint8_t buf[MODBUS_UART_BUF_SIZE];
    uint16_t len = 0U;

    init_and_attach();
    for (uint16_t i = 0U; i < MODBUS_UART_BUF_SIZE; i++)
    {
        feed_byte((uint8_t) i);
    }
    fire_idle();

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_EVENT_RX_DONE, s_cb_capture.last_event);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_get_rx_frame(buf, &len));
    TEST_ASSERT_EQUAL_UINT16(MODBUS_UART_BUF_SIZE, len);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, buf[MODBUS_UART_BUF_SIZE - 1U]);
}

void test_T_MBUART_22_overflow_flag_does_not_poison_next_frame(void)
{
    uint8_t buf[MODBUS_UART_BUF_SIZE];
    uint16_t len = 0U;

    init_and_attach();
    for (uint16_t i = 0U; i <= MODBUS_UART_BUF_SIZE; i++)
    {
        feed_byte(0x11U);
    }
    fire_idle(); /* RX_ERROR */

    feed_byte(0x22U);
    fire_idle();

    TEST_ASSERT_EQUAL_UINT32(2U, s_cb_capture.invocation_count);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_EVENT_RX_DONE, s_cb_capture.last_event);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_get_rx_frame(buf, &len));
    TEST_ASSERT_EQUAL_UINT16(1U, len);
    TEST_ASSERT_EQUAL_HEX8(0x22U, buf[0]);
}

void test_T_MBUART_23_idle_with_empty_buffer_is_ignored(void)
{
    init_and_attach();
    fire_idle();

    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_capture.invocation_count);
    TEST_ASSERT_BITS_HIGH(USART_ICR_IDLECF, UART4->ICR);
}

void test_T_MBUART_24_isr_without_attached_callback_does_not_crash(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());

    feed_byte(0x01U);
    fire_idle();
    UART4->ISR = USART_ISR_ORE;
    UART4_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_capture.invocation_count);
}

void test_T_MBUART_10_get_rx_frame_returns_correct_bytes_and_clears(void)
{
    const uint8_t frame[6] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x02U};
    uint8_t buf[MODBUS_UART_BUF_SIZE];
    uint16_t len = 0U;

    init_and_attach();
    for (uint8_t i = 0U; i < 6U; i++)
    {
        feed_byte(frame[i]);
    }
    fire_idle();

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_get_rx_frame(buf, &len));
    TEST_ASSERT_EQUAL_UINT16(6U, len);
    TEST_ASSERT_EQUAL_MEMORY(frame, buf, 6U);

    /* A second read finds the buffer consumed. */
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_get_rx_frame(buf, &len));
    TEST_ASSERT_EQUAL_UINT16(0U, len);
}

void test_T_MBUART_25_get_rx_frame_rejects_null_arguments(void)
{
    uint8_t buf[MODBUS_UART_BUF_SIZE];
    uint16_t len = 0U;

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_INVALID_ARG, modbus_uart_get_rx_frame(NULL, &len));
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_INVALID_ARG, modbus_uart_get_rx_frame(buf, NULL));
}

/* ===================================================================== */
/* T-MBUART-11: BUSY guard                                               */
/* ===================================================================== */

void test_T_MBUART_11_transmit_busy_guard(void)
{
    const uint8_t frame[1] = {0xAAU};

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_OK, modbus_uart_init());
    modbus_uart_set_tick_source(test_get_ms_reentrant);

    /* TXE never asserts, so the outer call is inside its wait loop when the
     * tick source re-enters transmit(): the inner call must see BUSY. */
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_TIMEOUT, modbus_uart_transmit(frame, 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_reentrant_calls);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_BUSY, s_reentrant_result);
}
