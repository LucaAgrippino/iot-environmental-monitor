/**
 * @file main_test_cpu.c
 * @brief Hardware bring-up test for CpuDriver on the STM32F469I-DISCO (Field Device).
 *
 * NOT compiled by default (listed in the integration-tests excluding= of .cproject). Build and
 * flash it with:
 *
 *   MAIN=firmware/field-device/integration-tests/cpu/main_test_cpu.c \
 *   EXTRA_CFLAGS=-UDEBUG scripts/build-fd-app.sh --flash
 *
 * -UDEBUG gives the release behaviour (reset after a panic). With DEBUG a panic stops at a
 * breakpoint, which is the right behaviour under a debugger but ends the unattended run.
 *
 * Console: USART3 PB10 (ST-LINK virtual COM port), 115 200 8N1.
 *
 * The run needs no operator. A step counter and a sentinel live in RTC backup registers, so every
 * boot runs the next step; each step ends in a reset and the next boot prints the post-mortem that
 * cpu_init() found.
 *
 *   step 0  read of a reserved address        expect BusFault, "Precise data bus error"
 *   step 1  undefined instruction             expect UsageFault, "Undefined instruction"
 *   step 2  divide by zero (trap enabled)     expect UsageFault, "Divide by zero"
 *   step 3  cpu_panic(CPU_PANIC_USER, ...)    expect User, no frame (PC 0x00000000)
 *   step 4  cpu_reset()                       expect NO post-mortem on the next boot
 *   step 5  task overruns its stack           expect StackOverflow, reason = the task name
 *   step 6  end
 *
 *   TC-HW-CPU-007  steps 0 to 3 print a report and the next boot prints a post-mortem
 *   TC-HW-CPU-008  step 4 leaves no post-mortem
 *   TC-HW-CPU-010  step 5 goes through vApplicationStackOverflowHook
 *   TC-HW-CPU-011  backup register 0 (TimeProvider's sync flag) holds its sentinel after every
 *                  panic reset
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cpu/cpu.h"
#include "stm32f469xx.h"
#include "system_clock.h"

#define STEP_BKP_IDX (19U)
#define STEP_MARKER (0x57E90000U)
#define SENTINEL_BKP_IDX (0U)
#define SENTINEL (0xA5A50001U)
#define BRINGUP_UART_BRR (391U) /* 45 MHz / 115 200 */
#define RESERVED_ADDRESS (0x2FFF0000U)
#define SCB_CCR_DIV_0_TRP (1UL << 4U)

static volatile uint32_t *bkp(uint32_t idx)
{
    return &RTC->BKP0R + idx;
}

static void uart_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 20U)) | (2UL << 20U);
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(0xFUL << 8U)) | (7UL << 8U);
    GPIOB->OSPEEDR |= (3UL << 20U);
    USART3->CR1 = 0U;
    USART3->BRR = BRINGUP_UART_BRR;
    USART3->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void putc_(char c)
{
    while ((USART3->SR & USART_SR_TXE) == 0U)
    {
    }
    USART3->DR = (uint32_t) (uint8_t) c;
}

static void puts_(const char *s)
{
    while (*s != '\0')
    {
        putc_(*s++);
    }
}

static void put_hex(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    puts_("0x");
    for (int32_t shift = 28; shift >= 0; shift -= 4)
    {
        putc_(hex[(v >> (uint32_t) shift) & 0xFU]);
    }
}

static void drain(void)
{
    while ((USART3->SR & USART_SR_TC) == 0U)
    {
    }
}

/* The guard area below the task stack takes the overrun, so nothing else is corrupted. */
static struct
{
    uint32_t guard[256];
    StackType_t stack[96];
} s_ovf_mem __attribute__((aligned(8)));
static StaticTask_t s_ovf_tcb;

static void overflow_task(void *arg)
{
    (void) arg;
    for (;;)
    {
        volatile uint8_t big[600]; /* more than the 96-word (384 byte) stack */
        for (uint32_t i = 0U; i < sizeof(big); ++i)
        {
            big[i] = 0x11U;
        }
        vTaskDelay(1U); /* the stack check runs at the context switch */
    }
}

static void trigger(uint32_t step)
{
    switch (step)
    {
    case 0U:
        puts_("[EXPECT] BusFault, Precise data bus error\r\n");
        drain();
        {
            volatile uint32_t v = *(volatile uint32_t *) RESERVED_ADDRESS;
            (void) v;
        }
        break;
    case 1U:
        puts_("[EXPECT] UsageFault, Undefined instruction\r\n");
        drain();
        __asm volatile("udf #0");
        break;
    case 2U:
        puts_("[EXPECT] UsageFault, Divide by zero\r\n");
        drain();
        SCB->CCR |= SCB_CCR_DIV_0_TRP;
        {
            volatile uint32_t d = 0U;
            /* cppcheck-suppress zerodiv */ /* the divide trap is what this step tests */
            volatile uint32_t r = 100U / d;
            (void) r;
        }
        break;
    case 3U:
        puts_("[EXPECT] User, harness panic, no frame\r\n");
        drain();
        cpu_panic(CPU_PANIC_USER, "TC-HW-CPU-007 harness panic");
        break;
    case 4U:
        puts_("[EXPECT] clean reset, NO post-mortem on the next boot\r\n");
        drain();
        cpu_reset();
        break;
    case 5U:
        puts_("[EXPECT] StackOverflow, Reason: ovf_task\r\n");
        drain();
        (void) xTaskCreateStatic(overflow_task, "ovf_task", 96U, NULL, 1U, s_ovf_mem.stack,
                                 &s_ovf_tcb);
        vTaskStartScheduler();
        break;
    default:
        break;
    }
    puts_("[FAIL] step returned without a reset\r\n");
}

int main(void)
{
    system_clock_init();
    const status_t init_st = cpu_init(); /* prints the previous boot's post-mortem, if any */
    uart_init();

    puts_("\r\n======= CpuDriver Hardware Bring-up (Field Device) =======\r\n");
    puts_((init_st == STATUS_OK) ? "[PASS] TC-HW-CPU-001  cpu_init() returned STATUS_OK\r\n"
                                 : "[FAIL] TC-HW-CPU-001  cpu_init() failed\r\n");

    uint32_t step = *bkp(STEP_BKP_IDX);
    if ((step & 0xFFFF0000U) != STEP_MARKER)
    {
        step = 0U; /* first run */
        *bkp(SENTINEL_BKP_IDX) = SENTINEL;
    }
    else
    {
        step &= 0xFFFFU;
        puts_((*bkp(SENTINEL_BKP_IDX) == SENTINEL)
                  ? "[PASS] TC-HW-CPU-011  backup register 0 survived the reset\r\n"
                  : "[FAIL] TC-HW-CPU-011  backup register 0 was overwritten\r\n");
    }

    puts_("[STEP ");
    put_hex(step);
    puts_("]\r\n");

    if (step >= 6U)
    {
        *bkp(STEP_BKP_IDX) = 0U; /* arm the next run */
        puts_("[PASS] sequence complete, no operator action was needed\r\n");
        for (;;)
        {
        }
    }

    *bkp(STEP_BKP_IDX) = STEP_MARKER | (step + 1U);
    trigger(step);
    for (;;)
    {
    }
}
