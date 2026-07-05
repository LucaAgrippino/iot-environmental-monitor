/**
 * @file main_test_cpu.c
 * @brief Hardware bring-up test for CpuDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate:
 *   1. Copy this file into Src/ (or add integration-tests/cpu/ as a
 *      source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c → Properties →
 *      C/C++ Build → check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Flash and run with a debugger attached (J-Link or ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6  115 200 8N1  Human-readable test results
 *   LD2 green  PA5               Heartbeat and delay-boundary markers
 *
 * Automated test sequence:
 *   TC-HW-CPU-001  cpu_init() returns STATUS_OK
 *   TC-HW-CPU-002  cpu_get_sysclk_hz() == 80 000 000
 *   TC-HW-CPU-003  cpu_get_pclk1_hz()  == 80 000 000
 *   TC-HW-CPU-004  cpu_get_pclk2_hz()  == 80 000 000
 *   TC-HW-CPU-005  cpu_delay_us(1000) within 1 % of 80 000 DWT cycles
 *   TC-HW-CPU-006  cpu_delay_ms(100)  within 1 % of 8 000 000 DWT cycles
 *
 * Manual / destructive tests — see comments at the bottom of main():
 *   TC-HW-CPU-007  cpu_panic() — panic report on UART; post-mortem on next boot
 *   TC-HW-CPU-008  cpu_reset() — clean reset; no post-mortem expected on reboot
 */

#include <stdbool.h>
#include <stdint.h>

#include "cpu/cpu.h"
#include "status.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

/** LD2 (green user LED) on B-L475E-IOT01A — active high. */
#define BRINGUP_LED_PIN          (5U)

/** USART1 TX on PB6, alternate function AF7. */
#define BRINGUP_UART_TX_PIN      (6U)
#define BRINGUP_UART_TX_AF       (7U)

/**
 * USART1 BRR = PCLK2 / baud = 80 000 000 / 115 200 ≈ 694.
 * Only valid after cpu_init() has switched SYSCLK to 80 MHz.
 */
#define BRINGUP_UART_BRR         (694U)

/** Expected DWT cycles for cpu_delay_us(1000) at 80 MHz: 1000 × 80. */
#define BRINGUP_DELAY_US_EXPECTED    (80000U)

/** Expected DWT cycles for cpu_delay_ms(100) at 80 MHz: 100 × 80 000. */
#define BRINGUP_DELAY_MS_EXPECTED    (8000000U)

/** Tolerance denominator: error must be < expected / 100 (1 %). */
#define BRINGUP_TOLERANCE_DIV        (100U)

/* ---------------------------------------------------------------------- */
/* Raw spin delay — used only in the TC-001 failure path before DWT is    */
/* available.                                                              */
/* ---------------------------------------------------------------------- */

static void bringup_raw_spin(uint32_t n)
{
    volatile uint32_t c = n;
    while (c > 0U)
    {
        --c;
    }
}

/* ---------------------------------------------------------------------- */
/* LED helpers (GPIOA, AHB2ENR — L475 GPIO clock register)                */
/* ---------------------------------------------------------------------- */

static void bringup_led_init(void)
{
    RCC->AHB2ENR   |=  RCC_AHB2ENR_GPIOAEN;
    GPIOA->MODER   &= ~(3UL << (BRINGUP_LED_PIN * 2U));
    GPIOA->MODER   |=  (1UL << (BRINGUP_LED_PIN * 2U));  /* general output */
    GPIOA->OTYPER  &= ~(1UL <<  BRINGUP_LED_PIN);         /* push-pull      */
    GPIOA->OSPEEDR &= ~(3UL << (BRINGUP_LED_PIN * 2U));   /* low speed      */
    GPIOA->PUPDR   &= ~(3UL << (BRINGUP_LED_PIN * 2U));   /* no pull        */
    GPIOA->BSRR     =  (1UL << (BRINGUP_LED_PIN + 16U));  /* off initially  */
}

static void bringup_led_on(void)
{
    GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
}

static void bringup_led_off(void)
{
    GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
}

static void bringup_led_toggle(void)
{
    GPIOA->ODR ^= (1UL << BRINGUP_LED_PIN);
}

/* ---------------------------------------------------------------------- */
/* UART helpers (USART1, PB6, 115 200 8N1)                                */
/* ---------------------------------------------------------------------- */

static void bringup_uart_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    GPIOB->MODER  &= ~(3UL  << (BRINGUP_UART_TX_PIN * 2U));
    GPIOB->MODER  |=  (2UL  << (BRINGUP_UART_TX_PIN * 2U));  /* AF mode    */
    GPIOB->OTYPER &= ~(1UL  <<  BRINGUP_UART_TX_PIN);         /* push-pull  */
    GPIOB->OSPEEDR|=  (3UL  << (BRINGUP_UART_TX_PIN * 2U));   /* very high  */
    GPIOB->PUPDR  &= ~(3UL  << (BRINGUP_UART_TX_PIN * 2U));   /* no pull    */
    GPIOB->AFR[0] &= ~(0xFUL << (BRINGUP_UART_TX_PIN * 4U));
    GPIOB->AFR[0] |=  ((uint32_t)BRINGUP_UART_TX_AF << (BRINGUP_UART_TX_PIN * 4U));

    USART1->CR1 = 0U;
    USART1->BRR = BRINGUP_UART_BRR;
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void bringup_putc(char c)
{
    while (!(USART1->ISR & USART_ISR_TXE))
    {
        /* spin */
    }
    USART1->TDR = (uint32_t)(uint8_t)c;
}

static void bringup_puts(const char *s)
{
    while (*s != '\0')
    {
        bringup_putc(*s);
        ++s;
    }
}

static void bringup_put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    bringup_puts("0x");
    for (int8_t shift = 28; shift >= 0; shift -= 4)
    {
        bringup_putc(hex[(v >> (uint32_t)shift) & 0xFU]);
    }
}

static void bringup_put_dec(uint32_t v)
{
    if (v == 0U)
    {
        bringup_putc('0');
        return;
    }
    char    buf[10];
    uint8_t n = 0U;
    while (v > 0U)
    {
        buf[n++] = (char)('0' + (uint8_t)(v % 10U));
        v /= 10U;
    }
    while (n > 0U)
    {
        bringup_putc(buf[--n]);
    }
}

/* ---------------------------------------------------------------------- */
/* Test reporting                                                          */
/* ---------------------------------------------------------------------- */

static void bringup_pass(const char *label)
{
    bringup_puts("[PASS] ");
    bringup_puts(label);
    bringup_puts("\r\n");
    bringup_led_toggle();
}

/**
 * @brief Emit a FAIL banner and enter a fast-blink halt loop.
 *
 * Only call this after TC-HW-CPU-001 has passed — it relies on
 * cpu_delay_ms(), which requires the DWT cycle counter enabled by
 * cpu_init().
 */
static void bringup_fail(const char *label)
{
    bringup_puts("[FAIL] ");
    bringup_puts(label);
    bringup_puts(" — HALTED\r\n");
    for (;;)
    {
        bringup_led_on();
        cpu_delay_ms(100U);
        bringup_led_off();
        cpu_delay_ms(100U);
    }
}

/* ---------------------------------------------------------------------- */
/* main()                                                                  */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /*
     * TC-HW-CPU-001 — cpu_init() must be the very first call.
     *
     * Configures: MSI → PLL → 80 MHz SYSCLK, 4 WS Flash, DWT cycle
     * counter.  Also checks the RTC backup registers for a post-mortem
     * panic record from the previous boot; if found, emits the record
     * over USART1 before returning.
     */
    status_t init_st = cpu_init();

    /*
     * Initialise the bring-up UART (USART1, PB6, 115 200 8N1).
     * Reinitialises the same peripheral used by the panic path — safe
     * to call here because cpu_init() has already returned.
     * BRR = 694 is correct only at 80 MHz.
     */
    bringup_uart_init();
    bringup_led_init();

    bringup_puts("\r\n======= CpuDriver Hardware Bring-up =======\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("UART  : USART1 PB6 TX   115 200 8N1\r\n");
    bringup_puts("LED   : LD2 PA5\r\n");
    bringup_puts("============================================\r\n\r\n");

    /* Report TC-HW-CPU-001 ------------------------------------------- */
    if (init_st != STATUS_OK)
    {
        bringup_puts("[FAIL] TC-HW-CPU-001  cpu_init() returned ");
        bringup_put_hex32((uint32_t)init_st);
        bringup_puts("\r\n[FAIL] Clock/DWT unavailable — fast blink on PA5.\r\n");
        /*
         * DWT is not running and SYSCLK may still be MSI 4 MHz —
         * use a raw spin loop so the LED blink does not depend on
         * any calibrated delay.
         */
        for (;;)
        {
            bringup_led_on();
            bringup_raw_spin(200000U);
            bringup_led_off();
            bringup_raw_spin(200000U);
        }
    }
    bringup_pass("TC-HW-CPU-001  cpu_init() returned STATUS_OK");

    /* TC-HW-CPU-002..004 — clock frequency queries -------------------- */
    {
        const uint32_t expected = 80000000U;
        uint32_t       hz;

        hz = cpu_get_sysclk_hz();
        bringup_puts("[INFO] TC-HW-CPU-002  SYSCLK = ");
        bringup_put_dec(hz);
        bringup_puts(" Hz\r\n");
        if (hz != expected)
        {
            bringup_fail("TC-HW-CPU-002  cpu_get_sysclk_hz() != 80 000 000");
        }
        bringup_pass("TC-HW-CPU-002  cpu_get_sysclk_hz() == 80 000 000");

        hz = cpu_get_pclk1_hz();
        bringup_puts("[INFO] TC-HW-CPU-003  PCLK1  = ");
        bringup_put_dec(hz);
        bringup_puts(" Hz\r\n");
        if (hz != expected)
        {
            bringup_fail("TC-HW-CPU-003  cpu_get_pclk1_hz() != 80 000 000");
        }
        bringup_pass("TC-HW-CPU-003  cpu_get_pclk1_hz() == 80 000 000");

        hz = cpu_get_pclk2_hz();
        bringup_puts("[INFO] TC-HW-CPU-004  PCLK2  = ");
        bringup_put_dec(hz);
        bringup_puts(" Hz\r\n");
        if (hz != expected)
        {
            bringup_fail("TC-HW-CPU-004  cpu_get_pclk2_hz() != 80 000 000");
        }
        bringup_pass("TC-HW-CPU-004  cpu_get_pclk2_hz() == 80 000 000");
    }

    /* TC-HW-CPU-005 — cpu_delay_us(1000) ------------------------------ */
    /*
     * LED PA5 is driven high for the duration of the delay.
     * A logic analyser or oscilloscope on PA5 can verify the ~1 ms
     * pulse width independently of the UART count.
     */
    {
        uint32_t t0, elapsed, err;

        bringup_led_on();
        t0      = DWT->CYCCNT;
        cpu_delay_us(1000U);
        elapsed = DWT->CYCCNT - t0;
        bringup_led_off();

        bringup_puts("[INFO] TC-HW-CPU-005  cpu_delay_us(1000): ");
        bringup_put_dec(elapsed);
        bringup_puts(" cycles  (expected ~");
        bringup_put_dec(BRINGUP_DELAY_US_EXPECTED);
        bringup_puts(")\r\n");

        err = (elapsed > BRINGUP_DELAY_US_EXPECTED)
              ? (elapsed - BRINGUP_DELAY_US_EXPECTED)
              : (BRINGUP_DELAY_US_EXPECTED - elapsed);

        if (err > (BRINGUP_DELAY_US_EXPECTED / BRINGUP_TOLERANCE_DIV))
        {
            bringup_fail("TC-HW-CPU-005  delay error exceeds 1 %");
        }
        bringup_pass("TC-HW-CPU-005  cpu_delay_us(1000) within 1 % tolerance");
    }

    /* TC-HW-CPU-006 — cpu_delay_ms(100) ------------------------------- */
    /*
     * DWT->CYCCNT is a 32-bit counter; it wraps every ~53.7 s at 80 MHz.
     * A 100 ms measurement (8 000 000 cycles) is well within range.
     * Unsigned subtraction (elapsed = CYCCNT - t0) handles a mid-test
     * wrap correctly.
     */
    {
        uint32_t t0, elapsed, err;

        bringup_led_on();
        t0      = DWT->CYCCNT;
        cpu_delay_ms(100U);
        elapsed = DWT->CYCCNT - t0;
        bringup_led_off();

        bringup_puts("[INFO] TC-HW-CPU-006  cpu_delay_ms(100): ");
        bringup_put_dec(elapsed);
        bringup_puts(" cycles  (expected ~");
        bringup_put_dec(BRINGUP_DELAY_MS_EXPECTED);
        bringup_puts(")\r\n");

        err = (elapsed > BRINGUP_DELAY_MS_EXPECTED)
              ? (elapsed - BRINGUP_DELAY_MS_EXPECTED)
              : (BRINGUP_DELAY_MS_EXPECTED - elapsed);

        if (err > (BRINGUP_DELAY_MS_EXPECTED / BRINGUP_TOLERANCE_DIV))
        {
            bringup_fail("TC-HW-CPU-006  delay error exceeds 1 %");
        }
        bringup_pass("TC-HW-CPU-006  cpu_delay_ms(100) within 1 % tolerance");
    }

    /* ------------------------------------------------------------------ */
    bringup_puts("\r\n[PASS] All automated tests complete.\r\n");
    bringup_puts("[INFO] Heartbeat: LD2 PA5 toggles every 500 ms.\r\n\r\n");

    /*
     * TC-HW-CPU-007 — cpu_panic() + post-mortem recovery (manual step)
     *
     * Procedure:
     *   1. Uncomment the cpu_panic() call below and reflash.
     *   2. Observe the PANIC banner on USART1 (source, CFSR, PC, LR ...).
     *   3. In a DEBUG build, the debugger halts at __BKPT(0).
     *   4. Reset or power-cycle the board.
     *   5. On the next boot, cpu_init() detects the RTC backup magic
     *      (0xDEADC0DE) and emits a POST-MORTEM banner before this
     *      harness prints its own banner.
     *   6. Re-comment the call and reflash to restore normal operation.
     */
     cpu_panic(CPU_PANIC_USER, "TC-HW-CPU-007: deliberate panic test");

    /*
     * TC-HW-CPU-008 — cpu_reset() (manual step)
     *
     * Uncomment to exercise the clean software reset path.
     * On the subsequent boot, NO post-mortem record will appear because
     * cpu_reset() does not write the RTC backup registers.
     */
//     cpu_reset();

    /* Heartbeat -------------------------------------------------------- */
    for (;;)
    {
        bringup_led_toggle();
        cpu_delay_ms(500U);
    }
}
