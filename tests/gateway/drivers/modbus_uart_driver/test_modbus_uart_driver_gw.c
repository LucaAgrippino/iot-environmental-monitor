/**
 * @file test_modbus_uart_driver_gw.c
 * @brief Unity unit tests for ModbusUartDriver — Gateway (STM32L475, UART4).
 *
 * Compiled with -DSTM32L475xx (stm32l475xx.h mock, g_mock_uart4).
 * The ISR is tested by calling UART4_IRQHandler() directly with the
 * mock register bank in predetermined states.
 *
 * Test IDs: T-MBUART-01 to T-MBUART-11 (companion §7), Gateway variant.
 */

#include "unity.h"
#include "stm32l475_cmsis_mock.h"
#include "modbus_uart_driver.h"

/* Declare ISR and test-only hooks. */
extern void UART4_IRQHandler(void);
extern void modbus_uart_reset_for_test(void);

/* ===================================================================== */
/* Test fixtures                                                         */
/* ===================================================================== */

static uint32_t s_test_ms_value;

static uint32_t test_get_ms(void)
{
    return s_test_ms_value;
}

static uint32_t test_get_ms_auto_advance(void)
{
    return s_test_ms_value++;
}

static struct
{
    uint32_t              invocation_count;
    modbus_uart_event_t   last_event;
    void                 *last_context;
} s_cb_capture;

static void test_rx_callback(modbus_uart_event_t event, void *context)
{
    s_cb_capture.invocation_count++;
    s_cb_capture.last_event   = event;
    s_cb_capture.last_context = context;
}

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    modbus_uart_reset_for_test();
    s_test_ms_value               = 0U;
    s_cb_capture.invocation_count = 0U;
    s_cb_capture.last_event       = MODBUS_UART_EVENT_RX_DONE;
    s_cb_capture.last_context     = NULL;
}

void tearDown(void)
{
}

/* ===================================================================== */
/* T-MBUART-01 (GW): modbus_uart_init configures UART4 8N1 + DEM       */
/* ===================================================================== */

void test_T_MBUART_01_gw_init_configures_uart4_8n1_dem(void)
{
    modbus_uart_err_t err = modbus_uart_init();

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, err);

    /* GPIOA clock enabled on AHB2. */
    TEST_ASSERT_BITS_HIGH(RCC_AHB2ENR_GPIOAEN, RCC->AHB2ENR);

    /* UART4 clock enabled on APB1. */
    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR1_UART4EN, RCC->APB1ENR1);

    /* CR1: UE=1 (bit 0 on L4), TE=1. */
    TEST_ASSERT_BITS_HIGH(USART_CR1_UE | USART_CR1_TE, UART4->CR1);

    /* CR3: DEM=1 (hardware RS-485 DE mode). */
    TEST_ASSERT_BITS_HIGH(USART_CR3_DEM, UART4->CR3);

    /* BRR: value pending MBUART-O2 — not asserted here. */
    TEST_IGNORE_MESSAGE("T-MBUART-01 (GW): BRR value deferred — MBUART-O2 unresolved");
}

/* ===================================================================== */
/* T-MBUART-02 (GW): attach_rx enables RXNEIE and IDLEIE               */
/* ===================================================================== */

void test_T_MBUART_02_gw_attach_rx_enables_rxneie_and_idleie(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_init());

    modbus_uart_attach_rx(test_rx_callback, NULL);

    /* RE, RXNEIE, IDLEIE in CR1. */
    TEST_ASSERT_BITS_HIGH(USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_IDLEIE, UART4->CR1);

    /* NVIC vector for UART4 must be enabled. */
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_nvic_enable_count[UART4_IRQn]);
}

/* ===================================================================== */
/* T-MBUART-03 (GW): transmit happy path — 8-byte frame                 */
/* ===================================================================== */

void test_T_MBUART_03_gw_transmit_happy_path_8_bytes(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_init());

    /* Pre-assert TXE and TC in ISR (L4 status register). */
    UART4->ISR = USART_ISR_TXE | USART_ISR_TC;

    const uint8_t frame[8] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x02U, 0xC4U, 0x0BU};

    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_transmit(frame, 8U));

    /* TDR holds last byte written. */
    TEST_ASSERT_EQUAL_HEX8(0x0BU, (uint8_t) UART4->TDR);
}

/* ===================================================================== */
/* T-MBUART-04 (GW): transmit TXE timeout                              */
/* ===================================================================== */

void test_T_MBUART_04_gw_transmit_txe_timeout(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_init());
    modbus_uart_set_tick_source(test_get_ms_auto_advance);

    /* TXE never set — timeout fires. */
    const uint8_t frame[1] = {0xAAU};
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_TIMEOUT, modbus_uart_transmit(frame, 1U));
}

/* ===================================================================== */
/* T-MBUART-05 (GW): transmit TC timeout                               */
/* ===================================================================== */

void test_T_MBUART_05_gw_transmit_tc_timeout(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_init());
    modbus_uart_set_tick_source(test_get_ms_auto_advance);

    /* TXE set (byte accepted), TC never set. */
    UART4->ISR = USART_ISR_TXE;

    const uint8_t frame[1] = {0xBBU};
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_TIMEOUT, modbus_uart_transmit(frame, 1U));
}

/* ===================================================================== */
/* T-MBUART-06 (GW): ISR RXNE only — 4 bytes, no callback              */
/* ===================================================================== */

void test_T_MBUART_06_gw_isr_rxne_4_bytes_no_callback(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_init());
    modbus_uart_attach_rx(test_rx_callback, NULL);

    const uint8_t bytes[4] = {0x01U, 0x03U, 0x00U, 0x00U};

    for (uint8_t i = 0U; i < 4U; i++)
    {
        UART4->ISR = USART_ISR_RXNE;
        UART4->RDR = bytes[i];
        UART4_IRQHandler();
    }

    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_capture.invocation_count);
}

/* ===================================================================== */
/* T-MBUART-07 (GW): ISR IDLE after 4 bytes — RX_DONE callback         */
/* ===================================================================== */

void test_T_MBUART_07_gw_isr_idle_after_4_bytes_rx_done(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_init());
    modbus_uart_attach_rx(test_rx_callback, NULL);

    const uint8_t bytes[4] = {0x01U, 0x03U, 0xAAU, 0xBBU};

    for (uint8_t i = 0U; i < 4U; i++)
    {
        UART4->ISR = USART_ISR_RXNE;
        UART4->RDR = bytes[i];
        UART4_IRQHandler();
    }

    /* Fire IDLE. */
    UART4->ISR = USART_ISR_IDLE;
    UART4_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_capture.invocation_count);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_EVENT_RX_DONE, s_cb_capture.last_event);

    /* Verify ICR.IDLECF was written (L4 atomic clear). */
    TEST_ASSERT_BITS_HIGH(USART_ICR_IDLECF, UART4->ICR);

    uint8_t  buf[MODBUS_UART_BUF_SIZE];
    uint16_t len = 0U;
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_get_rx_frame(buf, &len));
    TEST_ASSERT_EQUAL_UINT16(4U, len);
    TEST_ASSERT_EQUAL_HEX8(0x01U, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x03U, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0xAAU, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0xBBU, buf[3]);
}

/* ===================================================================== */
/* T-MBUART-08 (GW): ISR ORE — RX_ERROR, rx_len reset                  */
/* ===================================================================== */

void test_T_MBUART_08_gw_isr_ore_calls_rx_error(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_init());
    modbus_uart_attach_rx(test_rx_callback, NULL);

    UART4->ISR = USART_ISR_ORE;
    UART4_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_capture.invocation_count);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_EVENT_RX_ERROR, s_cb_capture.last_event);

    /* ORECF must have been written to ICR. */
    TEST_ASSERT_BITS_HIGH(USART_ICR_ORECF, UART4->ICR);

    uint8_t  buf[MODBUS_UART_BUF_SIZE];
    uint16_t len = 0xFFFFU;
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_get_rx_frame(buf, &len));
    TEST_ASSERT_EQUAL_UINT16(0U, len);
}

/* ===================================================================== */
/* T-MBUART-09 (GW): buffer overrun (257 bytes before IDLE)            */
/* ===================================================================== */

void test_T_MBUART_09_gw_isr_buffer_overrun_257_bytes(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_init());
    modbus_uart_attach_rx(test_rx_callback, NULL);

    for (uint16_t i = 0U; i < MODBUS_UART_BUF_SIZE; i++)
    {
        UART4->ISR = USART_ISR_RXNE;
        UART4->RDR = (uint32_t)(uint8_t) i;
        UART4_IRQHandler();
    }

    /* 257th byte. */
    UART4->ISR = USART_ISR_RXNE;
    UART4->RDR = 0xFFU;
    UART4_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_capture.invocation_count);

    UART4->ISR = USART_ISR_IDLE;
    UART4_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_capture.invocation_count);
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_EVENT_RX_ERROR, s_cb_capture.last_event);
}

/* ===================================================================== */
/* T-MBUART-10 (GW): get_rx_frame correct data                         */
/* ===================================================================== */

void test_T_MBUART_10_gw_get_rx_frame_correct_data(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_init());
    modbus_uart_attach_rx(test_rx_callback, NULL);

    const uint8_t frame[6] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x02U};

    for (uint8_t i = 0U; i < 6U; i++)
    {
        UART4->ISR = USART_ISR_RXNE;
        UART4->RDR = frame[i];
        UART4_IRQHandler();
    }
    UART4->ISR = USART_ISR_IDLE;
    UART4_IRQHandler();

    uint8_t  buf[MODBUS_UART_BUF_SIZE];
    uint16_t len = 0U;
    TEST_ASSERT_EQUAL_INT(MODBUS_UART_ERR_OK, modbus_uart_get_rx_frame(buf, &len));
    TEST_ASSERT_EQUAL_UINT16(6U, len);
    TEST_ASSERT_EQUAL_MEMORY(frame, buf, 6U);
}

/* ===================================================================== */
/* T-MBUART-11 (GW): transmit BUSY guard                               */
/* ===================================================================== */

void test_T_MBUART_11_gw_transmit_busy_guard(void)
{
    (void) test_get_ms; /* suppress unused-function warning */
    TEST_IGNORE_MESSAGE("T-MBUART-11 (GW): concurrent-call BUSY guard requires two simultaneous "
                        "callers; deferred to integration test. Guard implementation "
                        "verified by code inspection.");
}
