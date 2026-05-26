#include "debug_uart_driver.h"
#include "stm32f469xx.h"
#include <stdbool.h>
#include <stddef.h>

/* Pin numbers within GPIOB. */
#define DEBUG_UART_TX_PIN (10U)
#define DEBUG_UART_RX_PIN (11U)

/* Alternate-function 7 = USART1..3 on F4 family. */
#define DEBUG_UART_AF (7U)

/* PCLK1 assumed value. Resolved properly when clock-config lands; see DUART-O2. */
#define DEBUG_UART_PCLK1_HZ (45000000U)
#define DEBUG_UART_BAUD (115200U)

/* GPIO MODER value for alternate function. */
#define GPIO_MODER_AF (0x2U)
/* GPIO PUPDR value for pull-up. */
#define GPIO_PUPDR_UP (0x1U)
/* GPIO OSPEEDR value for high speed. */
#define GPIO_OSPEEDR_HIGH (0x2U)

/* UART error mask */
#define USART_SR_ERR_MASK (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)

/* Line-terminator characters. */
#define DEBUG_UART_CR ((uint8_t) '\r')
#define DEBUG_UART_LF ((uint8_t) '\n')

typedef struct
{
    bool initialised;                              /**< Set by debug_uart_init(). */
    bool rx_attached;                              /**< Set by debug_uart_attach_rx(). */
    debug_uart_line_callback_t line_callback;      /**< ISR invokes on line-complete. */
    void *line_callback_context;                   /**< Caller context for the callback. */
    uint32_t (*get_ms)(void);                      /**< Tick source for send timeout, or NULL. */
    uint8_t rx_accum_buf[DEBUG_UART_LINE_MAX_LEN]; /**< ISR accumulation buffer. */
    volatile size_t rx_accum_len;                  /**< Bytes currently in accum_buf. */
    volatile bool rx_overflow;                     /**< Set if accum_buf filled before EOL. */
    uint8_t rx_ready_buf[DEBUG_UART_LINE_MAX_LEN + 1U]; /**< Frozen line for caller. */
    volatile size_t rx_ready_len;                       /**< Length of frozen line. */
    volatile bool rx_ready_truncated;                   /**< Set if frozen line was truncated. */
    volatile bool rx_ready_flag; /**< Set when a frozen line awaits collection. */
} debug_uart_driver_t;

static debug_uart_driver_t s_debug_uart;

debug_uart_err_t debug_uart_init(void)
{
    if (s_debug_uart.initialised)
    {
        return DEBUG_UART_OK; /* idempotent */
    }

    /* 1. Enable peripheral clocks: GPIOB and USART3. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;

    /* 2. Configure TX and RX pins (PB10, PB11) for AF7. */
    /*    MODER: alternate function (2 bits per pin). */
    GPIOB->MODER &= ~((0x3U << (2U * DEBUG_UART_TX_PIN)) | (0x3U << (2U * DEBUG_UART_RX_PIN)));
    GPIOB->MODER |=
        ((GPIO_MODER_AF << (2U * DEBUG_UART_TX_PIN)) | (GPIO_MODER_AF << (2U * DEBUG_UART_RX_PIN)));

    /*    AFR[1] = AFRH covers pins 8..15. Pin 10 is at bits [11:8], pin 11 at [15:12]. */
    GPIOB->AFR[1] &=
        ~((0xFU << (4U * (DEBUG_UART_TX_PIN - 8U))) | (0xFU << (4U * (DEBUG_UART_RX_PIN - 8U))));
    GPIOB->AFR[1] |= ((DEBUG_UART_AF << (4U * (DEBUG_UART_TX_PIN - 8U))) |
                      (DEBUG_UART_AF << (4U * (DEBUG_UART_RX_PIN - 8U))));

    /*    OSPEEDR: high speed. */
    GPIOB->OSPEEDR &= ~((0x3U << (2U * DEBUG_UART_TX_PIN)) | (0x3U << (2U * DEBUG_UART_RX_PIN)));
    GPIOB->OSPEEDR |= ((GPIO_OSPEEDR_HIGH << (2U * DEBUG_UART_TX_PIN)) |
                       (GPIO_OSPEEDR_HIGH << (2U * DEBUG_UART_RX_PIN)));

    /*    PUPDR: pull-up. */
    GPIOB->PUPDR &= ~((0x3U << (2U * DEBUG_UART_TX_PIN)) | (0x3U << (2U * DEBUG_UART_RX_PIN)));
    GPIOB->PUPDR |=
        ((GPIO_PUPDR_UP << (2U * DEBUG_UART_TX_PIN)) | (GPIO_PUPDR_UP << (2U * DEBUG_UART_RX_PIN)));

    /*    OTYPER: push-pull is 0; clear bits. */
    GPIOB->OTYPER &= ~((1U << DEBUG_UART_TX_PIN) | (1U << DEBUG_UART_RX_PIN));

    /* 3. Configure USART3: disable, BRR, CR1/CR2/CR3, enable. */
    USART3->CR1 = 0U; /* Disable USART, clear M, PCE, TE, RE, etc. */

    /* BRR = PCLK1 / BAUD, with 12 fractional bits + 4 fraction bits.
     * For PCLK1=45 MHz, BAUD=115200: USARTDIV = 24.4140625
     *   Mantissa = 24 = 0x18; Fraction = round(0.4140625 * 16) = 7
     *   BRR = (0x18 << 4) | 0x7 = 0x187 */
    USART3->BRR = (DEBUG_UART_PCLK1_HZ + (DEBUG_UART_BAUD / 2U)) / DEBUG_UART_BAUD;

    USART3->CR2 = 0U;                          /* STOP = 00 (1 stop bit) */
    USART3->CR3 = 0U;                          /* No flow control */
    USART3->CR1 = USART_CR1_TE | USART_CR1_UE; /* Enable TX and USART */

    s_debug_uart.initialised = true;
    return DEBUG_UART_OK;
}

debug_uart_err_t debug_uart_attach_rx(debug_uart_line_callback_t callback, void *context)
{
    if (!s_debug_uart.initialised)
    {
        return DEBUG_UART_ERR_NOT_INITIALISED;
    }
    if (callback == NULL)
    {
        return DEBUG_UART_ERR_NULL_POINTER;
    }
    if (s_debug_uart.rx_attached)
    {
        return DEBUG_UART_ERR_RX_ALREADY_ATTACHED;
    }

    /* Store callback and context. */
    s_debug_uart.line_callback = callback;
    s_debug_uart.line_callback_context = context;

    /* Reset RX state (defensive — module-load already zeroed these,
     * but documents the entry-state contract). */
    s_debug_uart.rx_accum_len = 0U;
    s_debug_uart.rx_overflow = false;
    s_debug_uart.rx_ready_flag = false;

    /* Enable RX in the peripheral: RE and RXNEIE. */
    USART3->CR1 |= USART_CR1_RE | USART_CR1_RXNEIE;

    /* Enable the USART NVIC vector. The consumer is responsible for
     * setting an appropriate priority via NVIC_SetPriority() before
     * the first interrupt arrives (see DUART-O5). */
    NVIC_EnableIRQ(USART3_IRQn);

    s_debug_uart.rx_attached = true;
    return DEBUG_UART_OK;
}

void debug_uart_set_tick_source(uint32_t (*get_ms)(void))
{
    s_debug_uart.get_ms = get_ms;
}

debug_uart_err_t debug_uart_send(const uint8_t *data, size_t length, uint32_t timeout_ms)
{
    if (!s_debug_uart.initialised)
    {
        return DEBUG_UART_ERR_NOT_INITIALISED;
    }
    if (length == 0U)
    {
        return DEBUG_UART_OK; /* no-op, data may be NULL */
    }
    if (data == NULL)
    {
        return DEBUG_UART_ERR_NULL_POINTER;
    }

    for (size_t i = 0U; i < length; i++)
    {
        uint32_t start = (s_debug_uart.get_ms != NULL) ? s_debug_uart.get_ms() : 0U;

        /* Poll TXE. If a tick source is wired, bound the wait. */
        while ((USART3->SR & USART_SR_TXE) == 0U)
        {
            if (s_debug_uart.get_ms != NULL)
            {
                uint32_t elapsed = s_debug_uart.get_ms() - start;
                if (elapsed >= timeout_ms)
                {
                    return DEBUG_UART_ERR_TX_TIMEOUT;
                }
            }
        }

        USART3->DR = data[i];
    }

    return DEBUG_UART_OK;
}

debug_uart_err_t debug_uart_read_line(uint8_t *out_buf, size_t buf_size, size_t *out_length,
                                      debug_uart_line_flag_t *out_flag)
{
    if (!s_debug_uart.initialised)
    {
        return DEBUG_UART_ERR_NOT_INITIALISED;
    }
    if ((out_buf == NULL) || (out_length == NULL) || (out_flag == NULL))
    {
        return DEBUG_UART_ERR_NULL_POINTER;
    }
    if (buf_size < (DEBUG_UART_LINE_MAX_LEN + 1U))
    {
        return DEBUG_UART_ERR_INVALID_PARAM;
    }

    /* Disable the USART NVIC vector around the read to prevent the ISR
     * from re-arming a new line while we're copying. */
    NVIC_DisableIRQ(USART3_IRQn);

    if (!s_debug_uart.rx_ready_flag)
    {
        NVIC_EnableIRQ(USART3_IRQn);
        return DEBUG_UART_ERR_NO_LINE_AVAILABLE;
    }

    /* Copy the frozen line and null-terminate. */
    for (size_t i = 0U; i < s_debug_uart.rx_ready_len; i++)
    {
        out_buf[i] = s_debug_uart.rx_ready_buf[i];
    }
    out_buf[s_debug_uart.rx_ready_len] = '\0';

    *out_length = s_debug_uart.rx_ready_len;
    *out_flag = s_debug_uart.rx_ready_truncated ? DEBUG_UART_LINE_TRUNCATED : DEBUG_UART_LINE_OK;

    /* Clear the ready state — next callback corresponds to a new line. */
    s_debug_uart.rx_ready_flag = false;
    s_debug_uart.rx_ready_truncated = false;

    NVIC_EnableIRQ(USART3_IRQn);
    return DEBUG_UART_OK;
}

void USART3_IRQHandler(void)
{
    /* 1. Read status register once. */
    const uint32_t sr = USART3->SR;

    /* 2. Error path: read DR to complete the F4 SR-DR clear sequence,
     *    drop the byte, exit. */
    if ((sr & USART_SR_ERR_MASK) != 0U)
    {
        (void) USART3->DR; /* completes the SR-DR error clear */
        return;
    }

    /* 3. RXNE path. */
    if ((sr & USART_SR_RXNE) == 0U)
    {
        return; /* spurious vector — nothing to do */
    }

    const uint8_t byte = (uint8_t) (USART3->DR & 0xFFU);

    /* 3b. Line terminator? */
    if ((byte == DEBUG_UART_CR) || (byte == DEBUG_UART_LF))
    {
        if (s_debug_uart.rx_accum_len > 0U)
        {
            /* Freeze the line. */
            for (size_t i = 0U; i < s_debug_uart.rx_accum_len; i++)
            {
                s_debug_uart.rx_ready_buf[i] = s_debug_uart.rx_accum_buf[i];
            }
            s_debug_uart.rx_ready_len = s_debug_uart.rx_accum_len;
            s_debug_uart.rx_ready_truncated = s_debug_uart.rx_overflow;
            s_debug_uart.rx_ready_flag = true;

            /* Reset accumulator. */
            s_debug_uart.rx_accum_len = 0U;
            s_debug_uart.rx_overflow = false;

            /* Notify consumer. */
            if (s_debug_uart.line_callback != NULL)
            {
                s_debug_uart.line_callback(s_debug_uart.line_callback_context);
            }
        }
        /* If accum_len == 0 (e.g. stray LF after a CR), silently ignore. */
        return;
    }

    /* 3c. Regular byte — append if room, else mark overflow and drop. */
    if (s_debug_uart.rx_accum_len < DEBUG_UART_LINE_MAX_LEN)
    {
        s_debug_uart.rx_accum_buf[s_debug_uart.rx_accum_len] = byte;
        s_debug_uart.rx_accum_len++;
    }
    else
    {
        s_debug_uart.rx_overflow = true;
    }
}

#ifdef TEST
void debug_uart_reset_for_test(void)
{
    /* Zero every field. Brute-force memset on a POD struct. */
    for (size_t i = 0; i < sizeof(s_debug_uart); i++)
    {
        ((uint8_t *) &s_debug_uart)[i] = 0U;
    }
}
void debug_uart_set_ready_line_for_test(const uint8_t *line, size_t len, bool truncated)
{
    /* Pre-condition the test must respect: len ≤ DEBUG_UART_LINE_MAX_LEN. */
    for (size_t i = 0U; i < len; i++)
    {
        s_debug_uart.rx_ready_buf[i] = line[i];
    }
    s_debug_uart.rx_ready_len = len;
    s_debug_uart.rx_ready_truncated = truncated;
    s_debug_uart.rx_ready_flag = true;
}
#endif
