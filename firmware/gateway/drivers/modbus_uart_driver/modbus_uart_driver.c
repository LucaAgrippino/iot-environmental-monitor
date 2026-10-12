/**
 * @file modbus_uart_driver.c
 * @brief ModbusUartDriver (Gateway) — UART4 register-level implementation.
 *
 * CMSIS only.  The ISR is UART4_IRQHandler(); everything it touches is
 * file-static state shared with task context.
 *
 * Layout:
 *   §1  Board constants
 *   §2  Module state
 *   §3  Helpers
 *   §4  ISR
 *   §5  Public API
 *   §6  Test hooks
 */

#include "modbus_uart_driver.h"

#include <stddef.h>

#include "stm32l475xx.h"

/* ===================================================================== */
/* §1. Board constants                                                   */
/* ===================================================================== */

/** UART4 kernel clock: PCLK1 (reset default UART4SEL) at 80 MHz. */
#define MBUART_PCLK_HZ (80000000UL)

/** BRR for 16x oversampling: fCK / baud, rounded to nearest (= 8333). */
#define MBUART_BRR_VALUE ((MBUART_PCLK_HZ + (MODBUS_UART_BAUD / 2UL)) / MODBUS_UART_BAUD)

#define MBUART_TX_PIN (0U)   /* PA0, UART4_TX */
#define MBUART_RX_PIN (1U)   /* PA1, UART4_RX */
#define MBUART_RTS_PIN (15U) /* PA15, UART4_RTS_DE (MBUART-O4) */
#define MBUART_GPIO_AF (8U)  /* AF8 = UART4 */
#define MBUART_MODER_AF (0x2UL)
#define MBUART_AFR_PINS_PER_REG (8U)
#define MBUART_MODER_BITS (2U)
#define MBUART_AFR_BITS (4U)
#define MBUART_MODER_MASK (0x3UL)
#define MBUART_AFR_MASK (0xFUL)

#define MBUART_ISR_ERR_MASK (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)
#define MBUART_ICR_ERR_MASK (USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF)

/* ===================================================================== */
/* §2. Module state                                                      */
/* ===================================================================== */

typedef struct
{
    uint8_t rx_buf[MODBUS_UART_BUF_SIZE]; /**< ISR-written receive buffer. */
    volatile uint16_t rx_len;             /**< Valid byte count in rx_buf. */
    volatile bool rx_overflow;            /**< A byte arrived with the buffer full. */
    modbus_uart_rx_cb_t rx_cb;            /**< Callback invoked from the ISR. */
    void *rx_ctx;                         /**< Opaque context passed to rx_cb. */
    volatile bool tx_busy;                /**< True while a polled transmit runs. */
    uint32_t (*get_ms)(void);             /**< Injected tick source; NULL = unbounded. */
} modbus_uart_driver_t;

static modbus_uart_driver_t s_modbus_uart;

/* ===================================================================== */
/* §3. Helpers                                                           */
/* ===================================================================== */

/** Route one PA pin to AF8: push-pull, no pull (PA15 resets with a pull-up as JTDI). */
static void mbuart_pin_to_af(uint32_t pin)
{
    const uint32_t afr_idx = pin / MBUART_AFR_PINS_PER_REG;
    const uint32_t afr_shift = (pin % MBUART_AFR_PINS_PER_REG) * MBUART_AFR_BITS;
    const uint32_t moder_shift = pin * MBUART_MODER_BITS;

    GPIOA->OTYPER &= ~(1UL << pin);
    GPIOA->PUPDR &= ~(MBUART_MODER_MASK << moder_shift);
    GPIOA->AFR[afr_idx] &= ~(MBUART_AFR_MASK << afr_shift);
    GPIOA->AFR[afr_idx] |= ((uint32_t) MBUART_GPIO_AF << afr_shift);
    GPIOA->MODER &= ~(MBUART_MODER_MASK << moder_shift);
    GPIOA->MODER |= (MBUART_MODER_AF << moder_shift);
}

/**
 * @brief Poll the ISR register until @p flag is set or the timeout expires.
 *
 * @return true if the flag was seen, false on timeout.  Without a tick
 *         source the wait is unbounded.
 */
static bool mbuart_wait_flag(uint32_t flag, uint32_t timeout_ms)
{
    const uint32_t start = (s_modbus_uart.get_ms != NULL) ? s_modbus_uart.get_ms() : 0U;

    while ((UART4->ISR & flag) == 0U)
    {
        if ((s_modbus_uart.get_ms != NULL) && ((s_modbus_uart.get_ms() - start) >= timeout_ms))
        {
            return false;
        }
    }
    return true;
}

static void mbuart_notify(modbus_uart_event_t event)
{
    if (s_modbus_uart.rx_cb != NULL)
    {
        s_modbus_uart.rx_cb(event, s_modbus_uart.rx_ctx);
    }
}

/* ===================================================================== */
/* §4. ISR                                                               */
/* ===================================================================== */

/**
 * @brief UART4 interrupt handler — RXNE, IDLE and receive errors (companion §3.2).
 *
 * Priority is set by the system configurator (companion §4 NVIC table).  An
 * error discards the partial frame and wins over a coincident IDLE, so a
 * damaged frame is never reported as RX_DONE.  IDLE with nothing buffered
 * (e.g. straight after an error reset) is ignored.  On L4 every flag is
 * cleared atomically through ICR, so no received byte can be lost to the
 * clear sequence (contrast the F469, MBUART-O3).
 */
void UART4_IRQHandler(void)
{
    const uint32_t status = UART4->ISR;

    if ((status & USART_ISR_RXNE) != 0U)
    {
        const uint8_t byte = (uint8_t) UART4->RDR;

        if (s_modbus_uart.rx_len < MODBUS_UART_BUF_SIZE)
        {
            s_modbus_uart.rx_buf[s_modbus_uart.rx_len] = byte;
            s_modbus_uart.rx_len++;
        }
        else
        {
            s_modbus_uart.rx_overflow = true;
        }
    }

    if ((status & MBUART_ISR_ERR_MASK) != 0U)
    {
        UART4->ICR =
            MBUART_ICR_ERR_MASK | (((status & USART_ISR_IDLE) != 0U) ? USART_ICR_IDLECF : 0U);
        s_modbus_uart.rx_len = 0U;
        s_modbus_uart.rx_overflow = false;
        mbuart_notify(MODBUS_UART_EVENT_RX_ERROR);
    }
    else if ((status & USART_ISR_IDLE) != 0U)
    {
        UART4->ICR = USART_ICR_IDLECF;

        if (s_modbus_uart.rx_overflow)
        {
            s_modbus_uart.rx_len = 0U;
            s_modbus_uart.rx_overflow = false;
            mbuart_notify(MODBUS_UART_EVENT_RX_ERROR);
        }
        else if (s_modbus_uart.rx_len > 0U)
        {
            mbuart_notify(MODBUS_UART_EVENT_RX_DONE);
        }
        else
        {
            /* Spurious IDLE with an empty buffer — nothing to report. */
        }
    }
    else
    {
        /* RXNE only: byte stored above. */
    }
}

/* ===================================================================== */
/* §5. Public API                                                        */
/* ===================================================================== */

modbus_uart_err_t modbus_uart_init(void)
{
    /* Clocks before any register access. */
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_UART4EN;

    mbuart_pin_to_af(MBUART_TX_PIN);
    mbuart_pin_to_af(MBUART_RX_PIN);
    mbuart_pin_to_af(MBUART_RTS_PIN);

    UART4->BRR = (uint32_t) MBUART_BRR_VALUE;
    UART4->CR3 |= USART_CR3_DEM;
    UART4->CR1 |= (USART_CR1_TE | USART_CR1_UE); /* 8N1 is the reset default */

    return MODBUS_UART_OK;
}

void modbus_uart_attach_rx(modbus_uart_rx_cb_t callback, void *context)
{
    if (callback == NULL)
    {
        return;
    }

    s_modbus_uart.rx_cb = callback;
    s_modbus_uart.rx_ctx = context;

    UART4->CR1 |= (USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_IDLEIE);
    NVIC_EnableIRQ(UART4_IRQn);
}

modbus_uart_err_t modbus_uart_transmit(const uint8_t *frame, uint16_t len)
{
    modbus_uart_err_t result = MODBUS_UART_OK;

    if ((frame == NULL) || (len == 0U) || (len > MODBUS_UART_BUF_SIZE))
    {
        return MODBUS_UART_ERR_INVALID_ARG;
    }

    if (s_modbus_uart.tx_busy)
    {
        return MODBUS_UART_ERR_BUSY;
    }

    s_modbus_uart.tx_busy = true;

    for (uint16_t i = 0U; (i < len) && (result == MODBUS_UART_OK); i++)
    {
        if (mbuart_wait_flag(USART_ISR_TXE, MODBUS_UART_TXE_TIMEOUT_MS))
        {
            UART4->TDR = (uint32_t) frame[i];
        }
        else
        {
            result = MODBUS_UART_ERR_TIMEOUT;
        }
    }

    /* TC, not TXE, proves the last stop bit has left the wire (MBUART-D4). */
    if ((result == MODBUS_UART_OK) && !mbuart_wait_flag(USART_ISR_TC, MODBUS_UART_TC_TIMEOUT_MS))
    {
        result = MODBUS_UART_ERR_TIMEOUT;
    }

    s_modbus_uart.tx_busy = false;
    return result;
}

modbus_uart_err_t modbus_uart_get_rx_frame(uint8_t *buf, uint16_t *len)
{
    if ((buf == NULL) || (len == NULL))
    {
        return MODBUS_UART_ERR_INVALID_ARG;
    }

    const uint16_t frame_len = s_modbus_uart.rx_len;

    for (uint16_t i = 0U; i < frame_len; i++)
    {
        buf[i] = s_modbus_uart.rx_buf[i];
    }
    *len = frame_len;
    s_modbus_uart.rx_len = 0U;

    return MODBUS_UART_OK;
}

void modbus_uart_set_tick_source(uint32_t (*get_ms)(void))
{
    s_modbus_uart.get_ms = get_ms;
}

/* ===================================================================== */
/* §6. Test hooks                                                        */
/* ===================================================================== */

#ifdef TEST
void modbus_uart_reset_for_test(void)
{
    for (uint16_t i = 0U; i < MODBUS_UART_BUF_SIZE; i++)
    {
        s_modbus_uart.rx_buf[i] = 0U;
    }
    s_modbus_uart.rx_len = 0U;
    s_modbus_uart.rx_overflow = false;
    s_modbus_uart.rx_cb = NULL;
    s_modbus_uart.rx_ctx = NULL;
    s_modbus_uart.tx_busy = false;
    s_modbus_uart.get_ms = NULL;
}
#endif /* TEST */
