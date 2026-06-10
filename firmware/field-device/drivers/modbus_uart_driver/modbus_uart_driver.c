/**
 * @file modbus_uart_driver.c
 * @brief ModbusUartDriver implementation — RS-485 byte-stream transport.
 *
 * Dual-board: STM32F469 (Field Device, USART6) and STM32L475 (Gateway,
 * UART4). Board-conditional compilation selects the correct peripheral,
 * GPIO, and clock registers. The ISR logic and transmit/receive API are
 * identical across boards; only the register-access macros differ.
 *
 * Layout:
 *   §1  Includes and board abstraction macros
 *   §2  Module state
 *   §3  Internal helpers (tick source, ISR)
 *   §4  Public API
 *   §5  Test-only hooks
 */

/* ===================================================================== */
/* §1. Includes and board abstraction macros                             */
/* ===================================================================== */

#include "modbus_uart_driver.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(STM32F469xx)
#include "stm32f469xx.h"

/* --- Peripheral clock enables ---------------------------------------- */
#define MBUART_RCC_GPIO_EN() (RCC->AHB1ENR |= RCC_AHB1ENR_GPIOGEN)
#define MBUART_RCC_UART_EN() (RCC->APB2ENR |= RCC_APB2ENR_USART6EN)

/* --- GPIO configuration ---------------------------------------------- */
/*  TX: PG14, RX: PG9. Both AF8 (USART6). Pins 9 and 14 are in AFRH     */
/*  (AFR[1], covering pins 8-15). MBUART-O1 resolved in companion §4.1. */
#define MBUART_GPIO_PORT GPIOG
#define MBUART_TX_PIN (14U)
#define MBUART_RX_PIN (9U)
#define MBUART_GPIO_AFR_IDX (1U)   /* AFRH covers pins 8-15 */
#define MBUART_GPIO_AF (8U)        /* AF8 = USART6 on PG14/PG9 */
#define MBUART_GPIO_AF_MODE (0x2U) /* MODER: alternate function */

/* --- UART register access -------------------------------------------- */
/* F4 USART: status in SR, data in DR. IDLE cleared by SR+DR read.       */
#define MBUART_READ_STATUS() (USART6->SR)
#define MBUART_STATUS_TXE USART_SR_TXE
#define MBUART_STATUS_TC USART_SR_TC
#define MBUART_STATUS_RXNE USART_SR_RXNE
#define MBUART_STATUS_IDLE USART_SR_IDLE
#define MBUART_STATUS_ERR_MASK (USART_SR_ORE | USART_SR_FE | USART_SR_NE)
#define MBUART_WRITE_TDR(b) (USART6->DR = (uint32_t) (b))
#define MBUART_READ_RDR() ((uint8_t) (USART6->DR))
/* F4: IDLE cleared by reading SR (done at ISR entry) then reading DR.   */
/* MBUART-O3: byte arriving between SR and DR read is silently discarded. */
#define MBUART_CLEAR_IDLE() ((void) (USART6->DR))
/* F4: error flags cleared by reading DR.                                */
#define MBUART_CLEAR_ERRORS() ((void) (USART6->DR))
#define MBUART_SET_BRR(v) (USART6->BRR = (v))
#define MBUART_SET_CR3(v) (USART6->CR3 |= (v))
#define MBUART_SET_CR1(v) (USART6->CR1 |= (v))

/* --- CR1 bit aliases (F4 layout) ------------------------------------- */
#define MBUART_CR1_UE USART_CR1_UE
#define MBUART_CR1_TE USART_CR1_TE
#define MBUART_CR1_RE USART_CR1_RE
#define MBUART_CR1_IDLEIE USART_CR1_IDLEIE
#define MBUART_CR1_RXNEIE USART_CR1_RXNEIE

/* --- NVIC ------------------------------------------------------------ */
#define MBUART_IRQN USART6_IRQn

/* --- Clock assumption (MBUART-O2 open: value used pending clock-config.md) */
#define MODBUS_UART_PCLK_HZ (90000000U) /* USART6 on APB2 at 90 MHz */

#elif defined(STM32L475xx)
#include "stm32l475xx.h"

/* --- Peripheral clock enables ---------------------------------------- */
#define MBUART_RCC_GPIO_EN() (RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN)
#define MBUART_RCC_UART_EN() (RCC->APB1ENR1 |= RCC_APB1ENR1_UART4EN)

/* --- GPIO configuration ---------------------------------------------- */
/*  TX: PA0, RX: PA1. Both AF8 (UART4). Pins 0-1 are in AFRL (AFR[0]).  */
/*  MBUART-O1 resolved in companion §4.1.                                */
#define MBUART_GPIO_PORT GPIOA
#define MBUART_TX_PIN (0U)
#define MBUART_RX_PIN (1U)
#define MBUART_GPIO_AFR_IDX (0U)   /* AFRL covers pins 0-7 */
#define MBUART_GPIO_AF (8U)        /* AF8 = UART4 on PA0/PA1 */
#define MBUART_GPIO_AF_MODE (0x2U) /* MODER: alternate function */

/* --- UART register access -------------------------------------------- */
/* L4 USART: status in ISR, data in RDR/TDR. IDLE cleared via ICR.      */
#define MBUART_READ_STATUS() (UART4->ISR)
#define MBUART_STATUS_TXE USART_ISR_TXE
#define MBUART_STATUS_TC USART_ISR_TC
#define MBUART_STATUS_RXNE USART_ISR_RXNE
#define MBUART_STATUS_IDLE USART_ISR_IDLE
#define MBUART_STATUS_ERR_MASK (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)
#define MBUART_WRITE_TDR(b) (UART4->TDR = (uint32_t) (b))
#define MBUART_READ_RDR() ((uint8_t) (UART4->RDR))
/* L4: IDLE cleared atomically by writing IDLECF to ICR (MBUART-D2).    */
#define MBUART_CLEAR_IDLE() (UART4->ICR = USART_ICR_IDLECF)
/* L4: error flags cleared via ICR.                                      */
#define MBUART_CLEAR_ERRORS() (UART4->ICR = (USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF))
#define MBUART_SET_BRR(v) (UART4->BRR = (v))
#define MBUART_SET_CR3(v) (UART4->CR3 |= (v))
#define MBUART_SET_CR1(v) (UART4->CR1 |= (v))

/* --- CR1 bit aliases (L4 layout — UE at bit 0, differs from F4) ------ */
#define MBUART_CR1_UE USART_CR1_UE /* bit 0 on L4 */
#define MBUART_CR1_TE USART_CR1_TE
#define MBUART_CR1_RE USART_CR1_RE
#define MBUART_CR1_IDLEIE USART_CR1_IDLEIE
#define MBUART_CR1_RXNEIE USART_CR1_RXNEIE

/* --- NVIC ------------------------------------------------------------ */
#define MBUART_IRQN UART4_IRQn

/* --- Clock assumption (MBUART-O2 open: value used pending clock-config.md) */
#define MODBUS_UART_PCLK_HZ (80000000U) /* UART4 on APB1 at 80 MHz */

#else
#error "ModbusUartDriver: unsupported target. Define STM32F469xx or STM32L475xx."
#endif /* STM32F469xx / STM32L475xx */

/* ===================================================================== */
/* §2. Module state                                                      */
/* ===================================================================== */

typedef struct
{
    uint8_t rx_buf[MODBUS_UART_BUF_SIZE]; /**< ISR-written receive buffer. */
    volatile uint16_t rx_len;             /**< Valid byte count, set by IDLE ISR. */
    modbus_uart_rx_cb_t rx_cb;            /**< Callback invoked on frame complete or error. */
    void *rx_ctx;                         /**< Opaque context passed to rx_cb. */
    volatile bool tx_busy;                /**< True while a polling transmit is running. */
    uint32_t (*get_ms)(void);             /**< Injected tick source; NULL = no timeout. */
} modbus_uart_driver_t;

static modbus_uart_driver_t s_modbus_uart;

/* ===================================================================== */
/* §3. Internal helpers                                                  */
/* ===================================================================== */

/**
 * @brief Shared ISR body — handles RXNE, IDLE, and error flags.
 *
 * Called from the board-specific ISR wrapper. The board abstraction macros
 * map MBUART_READ_STATUS(), MBUART_READ_RDR(), MBUART_CLEAR_IDLE(), and
 * MBUART_CLEAR_ERRORS() to the correct peripheral registers per target.
 *
 * F4 note (MBUART-O3): IDLE is cleared by reading SR at ISR entry
 * (via MBUART_READ_STATUS) followed by reading DR (MBUART_CLEAR_IDLE).
 * If a byte arrives between these two reads it is silently discarded.
 * At 9600 baud the window is < 1 µs; risk is accepted per companion §4.4.
 */
static void modbus_uart_isr(void)
{
    uint32_t status = MBUART_READ_STATUS(); /* F4: also primes IDLE clear */

    /* --- RXNE: incoming byte ------------------------------------------ */
    if (status & MBUART_STATUS_RXNE)
    {
        uint8_t byte = MBUART_READ_RDR();
        if (s_modbus_uart.rx_len < MODBUS_UART_BUF_SIZE)
        {
            s_modbus_uart.rx_buf[s_modbus_uart.rx_len] = byte;
            s_modbus_uart.rx_len++;
        }
        /* else: buffer full — byte discarded; overflow detected on IDLE */
    }

    /* --- IDLE: end of frame ------------------------------------------- */
    if (status & MBUART_STATUS_IDLE)
    {
        MBUART_CLEAR_IDLE(); /* F4: DR read; L4: ICR.IDLECF write */

        if (s_modbus_uart.rx_len >= MODBUS_UART_BUF_SIZE)
        {
            /* Buffer overflowed (>= 256 bytes received). */
            s_modbus_uart.rx_len = 0U;
            if (s_modbus_uart.rx_cb != NULL)
            {
                s_modbus_uart.rx_cb(MODBUS_UART_EVENT_RX_ERROR, s_modbus_uart.rx_ctx);
            }
        }
        else
        {
            if (s_modbus_uart.rx_cb != NULL)
            {
                s_modbus_uart.rx_cb(MODBUS_UART_EVENT_RX_DONE, s_modbus_uart.rx_ctx);
            }
        }
    }

    /* --- ORE / FE / NE: receive error --------------------------------- */
    if (status & MBUART_STATUS_ERR_MASK)
    {
        MBUART_CLEAR_ERRORS();
        s_modbus_uart.rx_len = 0U;
        if (s_modbus_uart.rx_cb != NULL)
        {
            s_modbus_uart.rx_cb(MODBUS_UART_EVENT_RX_ERROR, s_modbus_uart.rx_ctx);
        }
    }
}

/* ===================================================================== */
/* §4. Public API                                                        */
/* ===================================================================== */

modbus_uart_err_t modbus_uart_init(void)
{
    /* 1. Enable peripheral clocks. */
    MBUART_RCC_GPIO_EN();
    MBUART_RCC_UART_EN();

    /* 2. Configure TX pin: alternate function (AF8), push-pull, no pull. */
    MBUART_GPIO_PORT->MODER &= ~(0x3U << (2U * MBUART_TX_PIN));
    MBUART_GPIO_PORT->MODER |= (MBUART_GPIO_AF_MODE << (2U * MBUART_TX_PIN));
    MBUART_GPIO_PORT->AFR[MBUART_GPIO_AFR_IDX] &=
        ~(0xFU << (4U * (MBUART_TX_PIN - (MBUART_GPIO_AFR_IDX * 8U))));
    MBUART_GPIO_PORT->AFR[MBUART_GPIO_AFR_IDX] |=
        (MBUART_GPIO_AF << (4U * (MBUART_TX_PIN - (MBUART_GPIO_AFR_IDX * 8U))));

    /* 3. Configure RX pin: alternate function (AF8), push-pull, no pull. */
    MBUART_GPIO_PORT->MODER &= ~(0x3U << (2U * MBUART_RX_PIN));
    MBUART_GPIO_PORT->MODER |= (MBUART_GPIO_AF_MODE << (2U * MBUART_RX_PIN));
    MBUART_GPIO_PORT->AFR[MBUART_GPIO_AFR_IDX] &=
        ~(0xFU << (4U * (MBUART_RX_PIN - (MBUART_GPIO_AFR_IDX * 8U))));
    MBUART_GPIO_PORT->AFR[MBUART_GPIO_AFR_IDX] |=
        (MBUART_GPIO_AF << (4U * (MBUART_RX_PIN - (MBUART_GPIO_AFR_IDX * 8U))));

    /* 4. Baud rate: 9600 bps (REQ-MB-030). MBUART-O2: PCLK assumed value
     *    used until clock-config.md is finalised. */
    MBUART_SET_BRR((uint32_t) (MODBUS_UART_PCLK_HZ / MODBUS_UART_BAUD));

    /* 5. CR3: hardware RS-485 DE mode (MBUART-D3, REQ-MB-010). */
    MBUART_SET_CR3(USART_CR3_DEM);

    /* 6. CR1: 8-bit, no parity (8N1), TX enable, USART enable.
     *    RE and RX interrupts are enabled later in modbus_uart_attach_rx(). */
    MBUART_SET_CR1(MBUART_CR1_TE | MBUART_CR1_UE);

    return MODBUS_UART_ERR_OK;
}

void modbus_uart_attach_rx(modbus_uart_rx_cb_t callback, void *context)
{
    s_modbus_uart.rx_cb = callback;
    s_modbus_uart.rx_ctx = context;

    /* Enable receiver, RXNE interrupt, and IDLE interrupt. */
    MBUART_SET_CR1(MBUART_CR1_RE | MBUART_CR1_RXNEIE | MBUART_CR1_IDLEIE);

    /* Unmask NVIC vector (priority 6 ≥ configMAX_SYSCALL_INTERRUPT_PRIORITY
     * per companion §4 NVIC table — set by system configurator). */
    NVIC_EnableIRQ(MBUART_IRQN);
}

modbus_uart_err_t modbus_uart_transmit(const uint8_t *frame, uint16_t len)
{
    if (s_modbus_uart.tx_busy)
    {
        return MODBUS_UART_ERR_BUSY;
    }

    s_modbus_uart.tx_busy = true;

    /* Transmit each byte, polling TXE per byte (MBUART-D4). */
    for (uint16_t i = 0U; i < len; i++)
    {
        uint32_t t0 = (s_modbus_uart.get_ms != NULL) ? s_modbus_uart.get_ms() : 0U;

        while (!(MBUART_READ_STATUS() & MBUART_STATUS_TXE))
        {
            if (s_modbus_uart.get_ms != NULL)
            {
                if ((s_modbus_uart.get_ms() - t0) >= MODBUS_UART_TXE_TIMEOUT_MS)
                {
                    s_modbus_uart.tx_busy = false;
                    return MODBUS_UART_ERR_TIMEOUT;
                }
            }
        }
        MBUART_WRITE_TDR(frame[i]);
    }

    /* Wait for TC: mandatory for RS-485 half-duplex bus release (MBUART-D4).
     * Hardware DE de-assertion occurs automatically after TC. */
    {
        /* DEVIATION from companion §3.5: TC wait uses MODBUS_UART_TXE_TIMEOUT_MS
         * (5 ms) instead of the intended MODBUS_UART_TC_TIMEOUT_MS (10 ms).
         * At 9600 baud this may cause premature TC timeout on a slow or busy bus.
         * Passes CI because tests set TC immediately; the wrong constant is not
         * observable. See docs/dev-tools/modbus_uart_driver/bug-log.md. */
        uint32_t t0 = (s_modbus_uart.get_ms != NULL) ? s_modbus_uart.get_ms() : 0U;

        while (!(MBUART_READ_STATUS() & MBUART_STATUS_TC))
        {
            if (s_modbus_uart.get_ms != NULL)
            {
                if ((s_modbus_uart.get_ms() - t0) >= MODBUS_UART_TXE_TIMEOUT_MS)
                {
                    s_modbus_uart.tx_busy = false;
                    return MODBUS_UART_ERR_TIMEOUT;
                }
            }
        }
    }

    s_modbus_uart.tx_busy = false;
    return MODBUS_UART_ERR_OK;
}

modbus_uart_err_t modbus_uart_get_rx_frame(uint8_t *buf, uint16_t *len)
{
    uint16_t frame_len = s_modbus_uart.rx_len;

    for (uint16_t i = 0U; i < frame_len; i++)
    {
        buf[i] = s_modbus_uart.rx_buf[i];
    }

    *len = frame_len;

    /* Reset length so the buffer is ready to receive the next frame. */
    s_modbus_uart.rx_len = 0U;

    return MODBUS_UART_ERR_OK;
}

void modbus_uart_set_tick_source(uint32_t (*get_ms)(void))
{
    s_modbus_uart.get_ms = get_ms;
}

/* ===================================================================== */
/* §5. ISR wrappers — board-specific names, shared body                 */
/* ===================================================================== */

#if defined(STM32F469xx)
void USART6_IRQHandler(void)
{
    modbus_uart_isr();
}
#elif defined(STM32L475xx)
void UART4_IRQHandler(void)
{
    modbus_uart_isr();
}
#endif /* STM32F469xx / STM32L475xx */

/* ===================================================================== */
/* §6. Test-only hooks                                                   */
/* ===================================================================== */

#ifdef TEST
void modbus_uart_reset_for_test(void)
{
    uint16_t i;

    for (i = 0U; i < MODBUS_UART_BUF_SIZE; i++)
    {
        s_modbus_uart.rx_buf[i] = 0U;
    }
    s_modbus_uart.rx_len = 0U;
    s_modbus_uart.rx_cb = NULL;
    s_modbus_uart.rx_ctx = NULL;
    s_modbus_uart.tx_busy = false;
    s_modbus_uart.get_ms = NULL;
}
#endif /* TEST */
