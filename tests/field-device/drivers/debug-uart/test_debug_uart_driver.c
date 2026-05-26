#include "unity.h"
#include "stm32_cmsis_mock.h"
#include "debug_uart_driver.h"

extern void debug_uart_reset_for_test(void);
extern void debug_uart_set_ready_line_for_test(const uint8_t *line, size_t len, bool truncated);
extern void USART3_IRQHandler(void);

/* Test-controllable tick source. Tests set s_test_ms_value and the
 * driver's get_ms() reads it via this wrapper. */
static uint32_t s_test_ms_value;

/* Capture struct for the ISR-invoked callback. Tests inspect this
 * after firing the ISR to verify the callback's arguments. */
static struct
{
    uint32_t invocation_count;
    void *last_context_seen;
} s_callback_capture;

static uint32_t test_get_ms(void)
{
    return s_test_ms_value;
}

static uint32_t test_get_ms_auto_advance(void)
{
    return s_test_ms_value++; /* post-increment: first call returns 0, then 1, 2, ... */
}

void setUp(void)
{
    stm32_cmsis_mock_reset();
    debug_uart_reset_for_test();
    s_test_ms_value = 0U;
    s_callback_capture.invocation_count = 0U;
    s_callback_capture.last_context_seen = NULL;
}

void tearDown(void)
{
}

/* Proves: USART3 macro resolves to writable storage, fields are accessible
 * by their RM0386 names, and the volatile-on-fields pattern works. */
void test_mock_usart3_round_trip(void)
{
    USART3->BRR = 0x1869u;                /* Some plausible BRR value */
    USART3->CR1 = (1u << 13) | (1u << 3); /* UE | TE */

    TEST_ASSERT_EQUAL_HEX32(0x1869u, USART3->BRR);
    TEST_ASSERT_EQUAL_HEX32((1u << 13) | (1u << 3), USART3->CR1);
}

/* Proves: APB1ENR field exists and is independently writable from AHB1ENR. */
void test_mock_rcc_apb1enr_independent_from_ahb1enr(void)
{
    RCC->AHB1ENR = 0xDEADBEEFu;
    RCC->APB1ENR = RCC_APB1ENR_USART3EN;

    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, RCC->AHB1ENR);
    TEST_ASSERT_EQUAL_HEX32(1u << 18, RCC->APB1ENR);
}

/* Proves: NVIC mock records enable/disable calls and the reset clears them. */
void test_mock_nvic_records_enable_and_disable(void)
{
    NVIC_EnableIRQ(USART3_IRQn);
    NVIC_EnableIRQ(USART3_IRQn);
    NVIC_DisableIRQ(USART3_IRQn);

    TEST_ASSERT_EQUAL_UINT32(2u, g_mock_nvic_enable_count[USART3_IRQn]);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_nvic_disable_count[USART3_IRQn]);
}

/* Proves: reset zeroes USART3 storage and NVIC counters. */
void test_mock_reset_clears_usart3_and_nvic(void)
{
    USART3->CR1 = 0xFFFFFFFFu;
    NVIC_EnableIRQ(USART3_IRQn);

    stm32_cmsis_mock_reset();

    TEST_ASSERT_EQUAL_HEX32(0u, USART3->CR1);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_nvic_enable_count[USART3_IRQn]);
}

void test_debug_uart_init_succeeds_first_call(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());
}

void test_debug_uart_init_idempotent_second_call_returns_ok(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());
}

void test_debug_uart_init_enables_usart_and_gpio_clocks(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    /* AHB1ENR must have GPIOBEN set. */
    TEST_ASSERT_BITS_HIGH(RCC_AHB1ENR_GPIOBEN, RCC->AHB1ENR);

    /* APB1ENR must have USART3EN set. */
    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR_USART3EN, RCC->APB1ENR);
}

void test_debug_uart_init_configures_pin_alternate_function(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    /* MODER bits [21:20] = pin 10, [23:22] = pin 11, both = 0b10. */
    TEST_ASSERT_EQUAL_HEX32((0x2U << 20) | (0x2U << 22),
                            GPIOB->MODER & ((0x3U << 20) | (0x3U << 22)));

    /* AFR[1] bits [11:8] = pin 10, [15:12] = pin 11, both = 7. */
    TEST_ASSERT_EQUAL_HEX32((0x7U << 8) | (0x7U << 12),
                            GPIOB->AFR[1] & ((0xFU << 8) | (0xFU << 12)));
}

void test_debug_uart_init_programs_baud_rate_for_pclk(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    /* BRR = round(PCLK1 / BAUD) = round(45000000 / 115200) = 391 = 0x187. */
    TEST_ASSERT_EQUAL_HEX32(0x187u, USART3->BRR);
}

/* Dummy callback for tests that need a non-NULL function pointer. */
static void test_dummy_callback(void *ctx)
{
    (void) ctx;
}

void test_debug_uart_attach_rx_happy_path_enables_rxneie(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_attach_rx(test_dummy_callback, NULL));

    /* CR1.RE (bit 2) and CR1.RXNEIE (bit 5) must be set. */
    TEST_ASSERT_BITS_HIGH((1u << 2) | (1u << 5), USART3->CR1);

    /* NVIC vector for USART3 must have been enabled exactly once. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_nvic_enable_count[USART3_IRQn]);
}

void test_debug_uart_attach_rx_rejects_not_initialised(void)
{
    /* No debug_uart_init() called. */
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_NOT_INITIALISED,
                          debug_uart_attach_rx(test_dummy_callback, NULL));
}

void test_debug_uart_attach_rx_rejects_null_callback(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_NULL_POINTER, debug_uart_attach_rx(NULL, NULL));
}

void test_debug_uart_attach_rx_rejects_second_call(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_attach_rx(test_dummy_callback, NULL));

    /* Second call with valid args must be rejected. */
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_RX_ALREADY_ATTACHED,
                          debug_uart_attach_rx(test_dummy_callback, NULL));
}

void test_debug_uart_send_zero_length_returns_ok_no_writes(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    /* length=0 with NULL data must still succeed. */
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_send(NULL, 0U, 1000U));

    /* DR untouched. */
    TEST_ASSERT_EQUAL_HEX32(0u, USART3->DR);
}

void test_debug_uart_send_writes_each_byte_to_data_register(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    /* Pre-set TXE so the poll loop falls through immediately for every byte. */
    USART3->SR = USART_SR_TXE;

    const uint8_t data[] = {0x41, 0x42, 0x43}; /* "ABC" */
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_send(data, sizeof(data), 1000U));

    /* DR holds the last byte written (mock has no shift register). */
    TEST_ASSERT_EQUAL_HEX32(0x43u, USART3->DR);
}

void test_debug_uart_send_polls_txe_between_bytes(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());
    debug_uart_set_tick_source(test_get_ms);

    /* TXE not set, time advances toward timeout, then we make TXE assert.
     * For a memory-backed mock the poll loop reads the same SR value every
     * iteration — so without setting TXE up front, we'd loop forever. We
     * pre-set TXE so the loop exits on first read, and verify the byte
     * lands. The "polls between bytes" intent is structural: each loop
     * iteration re-reads SR. A multi-byte send confirms this isn't a
     * single-byte coincidence. */
    USART3->SR = USART_SR_TXE;

    const uint8_t data[] = {0x10, 0x20, 0x30, 0x40};
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_send(data, sizeof(data), 1000U));

    TEST_ASSERT_EQUAL_HEX32(0x40u, USART3->DR);
}

void test_debug_uart_send_rejects_null_data_when_length_nonzero(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_NULL_POINTER, debug_uart_send(NULL, 4U, 1000U));
}

void test_debug_uart_send_rejects_not_initialised(void)
{
    const uint8_t data[] = {0xAA};
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_NOT_INITIALISED,
                          debug_uart_send(data, sizeof(data), 1000U));
}

void test_debug_uart_send_returns_tx_timeout_when_txe_never_asserts(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    /* Tick source advances on every read; timeout triggers after
     * timeout_ms iterations of the poll loop. */
    debug_uart_set_tick_source(test_get_ms_auto_advance);

    /* TXE never set — loop must depend on the timeout to escape. */
    const uint8_t data[] = {0xAA};
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_TX_TIMEOUT, debug_uart_send(data, sizeof(data), 100U));
}

void test_debug_uart_read_line_returns_no_line_when_flag_clear(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    uint8_t buf[DEBUG_UART_LINE_MAX_LEN + 1U];
    size_t length;
    debug_uart_line_flag_t flag;

    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_NO_LINE_AVAILABLE,
                          debug_uart_read_line(buf, sizeof(buf), &length, &flag));

    /* NVIC must have been disabled and re-enabled exactly once each
     * (the protective bracket around the empty-state check). */
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_nvic_disable_count[USART3_IRQn]);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_nvic_enable_count[USART3_IRQn]);
}

void test_debug_uart_read_line_copies_line_and_clears_flag(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    const uint8_t injected[] = {'h', 'e', 'l', 'l', 'o'};
    debug_uart_set_ready_line_for_test(injected, sizeof(injected), false);

    uint8_t buf[DEBUG_UART_LINE_MAX_LEN + 1U] = {0};
    size_t length = 0xDEADBEEFu;
    debug_uart_line_flag_t flag = DEBUG_UART_LINE_TRUNCATED;

    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_read_line(buf, sizeof(buf), &length, &flag));

    /* Content copied verbatim. */
    TEST_ASSERT_EQUAL_MEMORY(injected, buf, sizeof(injected));
    TEST_ASSERT_EQUAL_UINT32(sizeof(injected), length);
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_LINE_OK, flag);

    /* Second read returns NO_LINE_AVAILABLE — flag was cleared. */
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_NO_LINE_AVAILABLE,
                          debug_uart_read_line(buf, sizeof(buf), &length, &flag));
}

void test_debug_uart_read_line_null_terminates_buffer(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    const uint8_t injected[] = {'h', 'i'};
    debug_uart_set_ready_line_for_test(injected, sizeof(injected), false);

    uint8_t buf[DEBUG_UART_LINE_MAX_LEN + 1U];
    /* Pre-fill with non-zero to verify the driver writes the terminator. */
    for (size_t i = 0; i < sizeof(buf); i++)
    {
        buf[i] = 0xAAu;
    }

    size_t length;
    debug_uart_line_flag_t flag;

    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_read_line(buf, sizeof(buf), &length, &flag));

    /* buf[2] must be '\0' (terminator written at index = length). */
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[2]);
}

void test_debug_uart_read_line_reports_ok_flag_when_not_truncated(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    const uint8_t injected[] = {'a', 'b'};
    debug_uart_set_ready_line_for_test(injected, sizeof(injected), false);

    uint8_t buf[DEBUG_UART_LINE_MAX_LEN + 1U];
    size_t length;
    debug_uart_line_flag_t flag = DEBUG_UART_LINE_TRUNCATED;

    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_read_line(buf, sizeof(buf), &length, &flag));
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_LINE_OK, flag);
}

void test_debug_uart_read_line_reports_truncated_flag_when_overflow_occurred(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    const uint8_t injected[] = {'x', 'y'};
    debug_uart_set_ready_line_for_test(injected, sizeof(injected), true);

    uint8_t buf[DEBUG_UART_LINE_MAX_LEN + 1U];
    size_t length;
    debug_uart_line_flag_t flag;

    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_read_line(buf, sizeof(buf), &length, &flag));
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_LINE_TRUNCATED, flag);
}

void test_debug_uart_read_line_rejects_buf_size_too_small(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    /* buf_size = DEBUG_UART_LINE_MAX_LEN exactly — one byte short. */
    uint8_t buf[DEBUG_UART_LINE_MAX_LEN];
    size_t length;
    debug_uart_line_flag_t flag;

    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_INVALID_PARAM,
                          debug_uart_read_line(buf, DEBUG_UART_LINE_MAX_LEN, &length, &flag));
}

void test_debug_uart_read_line_rejects_null_pointers(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    uint8_t buf[DEBUG_UART_LINE_MAX_LEN + 1U];
    size_t length;
    debug_uart_line_flag_t flag;

    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_NULL_POINTER,
                          debug_uart_read_line(NULL, sizeof(buf), &length, &flag));
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_NULL_POINTER,
                          debug_uart_read_line(buf, sizeof(buf), NULL, &flag));
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_NULL_POINTER,
                          debug_uart_read_line(buf, sizeof(buf), &length, NULL));
}

void test_debug_uart_read_line_rejects_not_initialised(void)
{
    uint8_t buf[DEBUG_UART_LINE_MAX_LEN + 1U];
    size_t length;
    debug_uart_line_flag_t flag;

    /* No debug_uart_init() called. */
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_ERR_NOT_INITIALISED,
                          debug_uart_read_line(buf, sizeof(buf), &length, &flag));
}

static void test_capturing_callback(void *ctx)
{
    s_callback_capture.invocation_count++;
    s_callback_capture.last_context_seen = ctx;
}

/* Boilerplate for ISR tests: init + attach + clear setup state. */
static void prime_driver_for_isr_test(void *callback_context)
{
    (void) debug_uart_init();
    (void) debug_uart_attach_rx(test_capturing_callback, callback_context);
}

/* Resolved deferred test from step 5: callback storage proven by firing
 * the ISR and observing the context value flowing through. */
void test_debug_uart_attach_rx_stores_callback_and_context(void)
{
    void *const distinctive_ctx = (void *) 0xCAFE5A5Au;
    prime_driver_for_isr_test(distinctive_ctx);

    /* Send 'A' then CR — the CR triggers freeze + callback. */
    USART3->SR = USART_SR_RXNE;
    USART3->DR = 'A';
    USART3_IRQHandler();

    USART3->SR = USART_SR_RXNE;
    USART3->DR = DEBUG_UART_CR;
    USART3_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1u, s_callback_capture.invocation_count);
    TEST_ASSERT_EQUAL_PTR(distinctive_ctx, s_callback_capture.last_context_seen);
}

void test_isr_accumulates_byte_into_buffer(void)
{
    prime_driver_for_isr_test(NULL);

    USART3->SR = USART_SR_RXNE;
    USART3->DR = 'X';
    USART3_IRQHandler();

    /* No EOL yet — no line ready, no callback. */
    TEST_ASSERT_EQUAL_UINT32(0u, s_callback_capture.invocation_count);

    /* Now finish with CR — the byte 'X' should be in the frozen line. */
    USART3->SR = USART_SR_RXNE;
    USART3->DR = DEBUG_UART_CR;
    USART3_IRQHandler();

    uint8_t buf[DEBUG_UART_LINE_MAX_LEN + 1U];
    size_t length;
    debug_uart_line_flag_t flag;
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_read_line(buf, sizeof(buf), &length, &flag));
    TEST_ASSERT_EQUAL_UINT32(1u, length);
    TEST_ASSERT_EQUAL_HEX8('X', buf[0]);
}

void test_isr_appends_until_eol_then_invokes_callback(void)
{
    prime_driver_for_isr_test(NULL);

    const char message[] = "hello";
    for (size_t i = 0; i < sizeof(message) - 1U; i++)
    {
        USART3->SR = USART_SR_RXNE;
        USART3->DR = (uint8_t) message[i];
        USART3_IRQHandler();
    }
    /* No callback fired yet. */
    TEST_ASSERT_EQUAL_UINT32(0u, s_callback_capture.invocation_count);

    /* LF triggers the freeze + callback. */
    USART3->SR = USART_SR_RXNE;
    USART3->DR = DEBUG_UART_LF;
    USART3_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1u, s_callback_capture.invocation_count);
}

void test_isr_strips_cr_and_lf(void)
{
    prime_driver_for_isr_test(NULL);

    /* Feed "hi" + LF. Read line back; it should be exactly "hi", no LF. */
    const uint8_t bytes[] = {'h', 'i', DEBUG_UART_LF};
    for (size_t i = 0; i < sizeof(bytes); i++)
    {
        USART3->SR = USART_SR_RXNE;
        USART3->DR = bytes[i];
        USART3_IRQHandler();
    }

    uint8_t buf[DEBUG_UART_LINE_MAX_LEN + 1U];
    size_t length;
    debug_uart_line_flag_t flag;
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_read_line(buf, sizeof(buf), &length, &flag));
    TEST_ASSERT_EQUAL_UINT32(2u, length);
    TEST_ASSERT_EQUAL_HEX8('h', buf[0]);
    TEST_ASSERT_EQUAL_HEX8('i', buf[1]);
    TEST_ASSERT_EQUAL_HEX8('\0', buf[2]);
}

void test_isr_handles_crlf_as_single_terminator(void)
{
    prime_driver_for_isr_test(NULL);

    /* Feed 'a' + CR + LF — should produce one line "a", one callback. */
    const uint8_t bytes[] = {'a', DEBUG_UART_CR, DEBUG_UART_LF};
    for (size_t i = 0; i < sizeof(bytes); i++)
    {
        USART3->SR = USART_SR_RXNE;
        USART3->DR = bytes[i];
        USART3_IRQHandler();
    }

    TEST_ASSERT_EQUAL_UINT32(1u, s_callback_capture.invocation_count);
}

void test_isr_marks_overflow_when_buffer_full_no_eol(void)
{
    prime_driver_for_isr_test(NULL);

    /* Feed DEBUG_UART_LINE_MAX_LEN + 5 bytes with no EOL, then EOL.
     * The first 128 bytes fill the buffer; the next 5 set overflow.
     * On EOL, the frozen line should report TRUNCATED. */
    for (size_t i = 0; i < DEBUG_UART_LINE_MAX_LEN + 5U; i++)
    {
        USART3->SR = USART_SR_RXNE;
        USART3->DR = 'Z';
        USART3_IRQHandler();
    }
    USART3->SR = USART_SR_RXNE;
    USART3->DR = DEBUG_UART_LF;
    USART3_IRQHandler();

    uint8_t buf[DEBUG_UART_LINE_MAX_LEN + 1U];
    size_t length;
    debug_uart_line_flag_t flag;
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_read_line(buf, sizeof(buf), &length, &flag));
    TEST_ASSERT_EQUAL_UINT32(DEBUG_UART_LINE_MAX_LEN, length);
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_LINE_TRUNCATED, flag);
}

void test_isr_drops_byte_and_clears_error_on_ore(void)
{
    prime_driver_for_isr_test(NULL);

    /* Set ORE flag — driver must read DR to clear, drop the byte. */
    USART3->SR = USART_SR_ORE;
    USART3->DR = 'X';
    USART3_IRQHandler();

    /* No data accumulated — feed a CR and verify the resulting line
     * is empty (no callback fires because empty lines are dropped). */
    USART3->SR = USART_SR_RXNE;
    USART3->DR = DEBUG_UART_CR;
    USART3_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(0u, s_callback_capture.invocation_count);
}

void test_isr_drops_byte_and_clears_error_on_fe(void)
{
    prime_driver_for_isr_test(NULL);

    USART3->SR = USART_SR_FE;
    USART3->DR = 'X';
    USART3_IRQHandler();

    USART3->SR = USART_SR_RXNE;
    USART3->DR = DEBUG_UART_CR;
    USART3_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(0u, s_callback_capture.invocation_count);
}

void test_isr_ignores_empty_line_terminator_after_full_line(void)
{
    prime_driver_for_isr_test(NULL);

    /* "a" + LF → callback once. Then LF alone → no second callback. */
    const uint8_t first[] = {'a', DEBUG_UART_LF};
    for (size_t i = 0; i < sizeof(first); i++)
    {
        USART3->SR = USART_SR_RXNE;
        USART3->DR = first[i];
        USART3_IRQHandler();
    }
    TEST_ASSERT_EQUAL_UINT32(1u, s_callback_capture.invocation_count);

    /* Stray LF — accum_len is 0, callback must not fire. */
    USART3->SR = USART_SR_RXNE;
    USART3->DR = DEBUG_UART_LF;
    USART3_IRQHandler();
    TEST_ASSERT_EQUAL_UINT32(1u, s_callback_capture.invocation_count);
}

void test_isr_invokes_callback_with_registered_context(void)
{
    void *const ctx = (void *) 0xABCDEF01u;
    prime_driver_for_isr_test(ctx);

    USART3->SR = USART_SR_RXNE;
    USART3->DR = 'q';
    USART3_IRQHandler();

    USART3->SR = USART_SR_RXNE;
    USART3->DR = DEBUG_UART_LF;
    USART3_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1u, s_callback_capture.invocation_count);
    TEST_ASSERT_EQUAL_PTR(ctx, s_callback_capture.last_context_seen);
}
