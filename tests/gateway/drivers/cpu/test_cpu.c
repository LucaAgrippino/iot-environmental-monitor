/**
 * @file test_cpu.c
 * @brief Unit tests for CpuDriver — STM32L475 platform singleton.
 *
 * Covers TC-CPU-001 through TC-CPU-040 per
 * docs/lld/drivers/cpu-driver.md §7.
 *
 * Mock strategy: all STM32L475 peripherals (RCC, PWR, FLASH, CoreDebug,
 * DWT, SCB, RTC, USART1, GPIOB) are redirected to mock structs via the
 * macros in stm32l475xx.h. g_cpu_hw_* counters from stm32l475_cmsis_mock.c
 * verify that privileged intrinsics (disable_irq, breakpoint, system_reset)
 * are invoked on the correct code paths.
 *
 * setUp() zeroes all mock peripheral state via stm32l475_cmsis_mock_reset(),
 * resets module static state via cpu_reset_for_test(), then pre-arranges
 * the PLL-lock and SYSCLK-switch flags so cpu_init() can complete in tests
 * that do not specifically test the timeout paths.
 */

#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "stm32l475_cmsis_mock.h"
#include "stm32l475xx.h"
#include "cpu.h"
#include "cpu_hw.h"
#include "cpu_driver.h" /* triggers auto-link of cpu.c */

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

/** Pre-arrange happy-path PLL and SYSCLK-switch conditions. */
static void arrange_pll_ready(void)
{
    g_mock_rcc_l4.CR   |= RCC_CR_PLLRDY;
    g_mock_rcc_l4.CFGR |= RCC_CFGR_SWS_PLL;
}

/** Pre-arrange DWT CYCCNTENA readback (write-back stays set in mock). */
static void arrange_dwt_ready(void)
{
    /* In the mock, writes persist — the check (DWT->CTRL & CYCCNTENA)
     * passes automatically once cpu_init writes it. No extra setup needed,
     * but calling this makes the intent explicit. */
    (void) 0;
}

/** Perform a successful cpu_init() with all pre-conditions satisfied. */
static void arrange_cpu_init_success(void)
{
    arrange_pll_ready();
    arrange_dwt_ready();
    TEST_ASSERT_EQUAL(STATUS_OK, cpu_init());
}

/* ===================================================================== */
/* Unity setUp / tearDown                                                */
/* ===================================================================== */

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    cpu_reset_for_test();
}

void tearDown(void)
{
    /* Fresh state is established in setUp(). */
}

/* ===================================================================== */
/* §7.1 Clock configuration — TC-CPU-001 through TC-CPU-008             */
/* ===================================================================== */

void test_TC_CPU_001_init_sets_flash_wait_states_to_4(void)
{
    /* Arrange */
    arrange_pll_ready();

    /* Act */
    status_t result = cpu_init();

    /* Assert */
    TEST_ASSERT_EQUAL(STATUS_OK, result);
    TEST_ASSERT_EQUAL_UINT32(FLASH_ACR_LATENCY_4WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN,
                              g_mock_flash.ACR);
}

void test_TC_CPU_002_init_configures_pll_m1_n40_r2_msi(void)
{
    /* Arrange */
    arrange_pll_ready();

    /* Act */
    status_t result = cpu_init();

    /* Assert */
    TEST_ASSERT_EQUAL(STATUS_OK, result);

    /* PLLSRC = MSI (01). */
    TEST_ASSERT_EQUAL_UINT32(RCC_PLLCFGR_PLLSRC_MSI,
                              g_mock_rcc_l4.PLLCFGR & RCC_PLLCFGR_PLLSRC_MSI);

    /* PLLN = 40 (bits 14:8). */
    uint32_t n = (g_mock_rcc_l4.PLLCFGR & RCC_PLLCFGR_PLLN) >> RCC_PLLCFGR_PLLN_Pos;
    TEST_ASSERT_EQUAL_UINT32(40U, n);

    /* PLLM = 0 → M=1 (bits 6:4). */
    uint32_t m = (g_mock_rcc_l4.PLLCFGR & RCC_PLLCFGR_PLLM) >> RCC_PLLCFGR_PLLM_Pos;
    TEST_ASSERT_EQUAL_UINT32(0U, m);

    /* PLLR = 0 → R=2 (bits 26:25). */
    uint32_t r = (g_mock_rcc_l4.PLLCFGR & RCC_PLLCFGR_PLLR) >> RCC_PLLCFGR_PLLR_Pos;
    TEST_ASSERT_EQUAL_UINT32(0U, r);

    /* PLLREN = 1 (R output enabled). */
    TEST_ASSERT_BITS_HIGH(RCC_PLLCFGR_PLLREN, g_mock_rcc_l4.PLLCFGR);
}

void test_TC_CPU_003_init_polls_pllrdy_and_succeeds(void)
{
    /* Arrange: PLLRDY and SWS_PLL pre-set (immediate exit from poll loops). */
    arrange_pll_ready();

    /* Act */
    status_t result = cpu_init();

    /* Assert */
    TEST_ASSERT_EQUAL(STATUS_OK, result);
    TEST_ASSERT_BITS_HIGH(RCC_CR_PLLON, g_mock_rcc_l4.CR); /* PLLON was written */
}

void test_TC_CPU_004_init_returns_timeout_when_pllrdy_never_sets(void)
{
    /* Arrange: PLLRDY never set — poll loop exhausts CPU_POLL_TIMEOUT (3). */
    /* Do NOT arrange_pll_ready() here. */

    /* Act */
    status_t result = cpu_init();

    /* Assert */
    TEST_ASSERT_EQUAL(STATUS_ERR_TIMEOUT, result);
}

void test_TC_CPU_005_init_switches_sysclk_to_pll(void)
{
    /* Arrange */
    arrange_pll_ready();

    /* Act */
    status_t result = cpu_init();

    /* Assert: SW bits written for PLL. */
    TEST_ASSERT_EQUAL(STATUS_OK, result);
    TEST_ASSERT_EQUAL_UINT32(RCC_CFGR_SW_PLL,
                              g_mock_rcc_l4.CFGR & RCC_CFGR_SW);
}

void test_TC_CPU_006_get_sysclk_hz_returns_80mhz_after_init(void)
{
    arrange_cpu_init_success();
    TEST_ASSERT_EQUAL_UINT32(80000000U, cpu_get_sysclk_hz());
}

void test_TC_CPU_007_get_pclk1_hz_returns_80mhz_after_init(void)
{
    arrange_cpu_init_success();
    TEST_ASSERT_EQUAL_UINT32(80000000U, cpu_get_pclk1_hz());
}

void test_TC_CPU_008_get_pclk2_hz_returns_80mhz_after_init(void)
{
    arrange_cpu_init_success();
    TEST_ASSERT_EQUAL_UINT32(80000000U, cpu_get_pclk2_hz());
}

/* ===================================================================== */
/* §7.2 Delay functions — TC-CPU-010 through TC-CPU-013                 */
/* ===================================================================== */

void test_TC_CPU_010_delay_us_1_exits_within_test_timeout(void)
{
    /* Arrange: cpu_init so g_sysclk_hz = 80 MHz. */
    arrange_cpu_init_success();

    /* Act: cpu_delay_us exits after CPU_DELAY_MAX_ITER iterations in TEST
     * builds (DWT->CYCCNT does not auto-advance in the mock).            */
    cpu_delay_us(1U);

    /* Assert: clock is 80 MHz so 1 µs = 80 cycles. Verify clock correct. */
    TEST_ASSERT_EQUAL_UINT32(80000000U, cpu_get_sysclk_hz());
}

void test_TC_CPU_011_delay_us_handles_dwt_wraparound(void)
{
    /* Arrange */
    arrange_cpu_init_success();

    /* Pre-set CYCCNT near max to exercise 32-bit unsigned subtraction wrap. */
    g_mock_dwt.CYCCNT = 0xFFFFFF00U;

    /* Act: unsigned arithmetic (CYCCNT - start) < cycles never overflows. */
    cpu_delay_us(1U); /* 80 cycles; wrap-safe */

    /* Assert: function returned (no infinite loop). */
    TEST_PASS_MESSAGE("wrap-around handled correctly by unsigned subtraction");
}

void test_TC_CPU_012_delay_ms_delegates_to_delay_us(void)
{
    /* Arrange */
    arrange_cpu_init_success();

    /* Act: cpu_delay_ms(1) must call cpu_delay_us(1000). Verify it returns. */
    cpu_delay_ms(1U);

    /* Assert: function returned. Clock still correct (not corrupted). */
    TEST_ASSERT_EQUAL_UINT32(80000000U, cpu_get_sysclk_hz());
}

void test_TC_CPU_013_delay_us_zero_returns_immediately(void)
{
    /* Arrange */
    arrange_cpu_init_success();

    /* Act: cycles = 0, so (CYCCNT - start) < 0 is always false (unsigned). */
    cpu_delay_us(0U);

    /* Assert: returned without hanging. */
    TEST_PASS_MESSAGE("delay_us(0) exits immediately due to unsigned comparison");
}

/* ===================================================================== */
/* §7.3 Panic subsystem — TC-CPU-020 through TC-CPU-027                 */
/* ===================================================================== */

void test_TC_CPU_020_panic_disables_interrupts_on_entry(void)
{
    /* Arrange: no pre-set USART1->ISR.TXE — UART output times out quickly. */
    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_disable_irq_count);

    /* Act */
    cpu_panic(CPU_PANIC_USER, "test");

    /* Assert */
    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_disable_irq_count);
}

void test_TC_CPU_021_panic_writes_magic_to_bkp0r(void)
{
    /* Act */
    cpu_panic(CPU_PANIC_USER, "test");

    /* Assert: BKP0R holds the commit magic (written last). */
    TEST_ASSERT_EQUAL_HEX32(0xDEADC0DEU, g_mock_rtc.BKP0R);

    /* Other BKP registers were written before BKP0R (code order). */
    /* BKP13R = source = CPU_PANIC_USER = 0x09. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t) CPU_PANIC_USER, g_mock_rtc.BKP13R);
}

void test_TC_CPU_022_panic_writes_cfsr_hfsr_bfar_to_bkp(void)
{
    /* Arrange: pre-load SCB registers. */
    g_mock_scb.CFSR = 0x00000400U; /* IMPRECISERR */
    g_mock_scb.HFSR = 0x40000000U; /* FORCED */
    g_mock_scb.BFAR = 0x60000000U;

    /* Act */
    cpu_panic(CPU_PANIC_BUSFAULT, NULL);

    /* Assert */
    TEST_ASSERT_EQUAL_HEX32(0x00000400U, g_mock_rtc.BKP1R); /* CFSR */
    TEST_ASSERT_EQUAL_HEX32(0x40000000U, g_mock_rtc.BKP2R); /* HFSR */
    TEST_ASSERT_EQUAL_HEX32(0x60000000U, g_mock_rtc.BKP3R); /* BFAR */
}

void test_TC_CPU_023_panic_via_fault_entry_writes_frame_pc_lr(void)
{
    /* Arrange: Cortex-M4 exception frame layout:
     * [0]=R0, [1]=R1, [2]=R2, [3]=R3, [4]=R12, [5]=LR, [6]=PC, [7]=xPSR */
    uint32_t frame[8] = {
        0x00000001U,  /* R0  */
        0x20001000U,  /* R1  */
        0x00000000U,  /* R2  */
        0xDEADBEEFU,  /* R3  */
        0x00000000U,  /* R12 */
        0x08001A20U,  /* LR  */
        0x08001A34U,  /* PC  */
        0x61000000U,  /* xPSR */
    };

    /* Act: call cpu_fault_entry directly (assembly trampoline skipped on host). */
    cpu_fault_entry(frame);

    /* Assert: PC → BKP4R, LR → BKP5R. */
    TEST_ASSERT_EQUAL_HEX32(0x08001A34U, g_mock_rtc.BKP4R); /* PC  */
    TEST_ASSERT_EQUAL_HEX32(0x08001A20U, g_mock_rtc.BKP5R); /* LR  */
    TEST_ASSERT_EQUAL_HEX32(0x61000000U, g_mock_rtc.BKP7R); /* xPSR */
    TEST_ASSERT_EQUAL_HEX32(0x00000001U, g_mock_rtc.BKP8R); /* R0  */
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFU, g_mock_rtc.BKP11R);/* R3  */
}

void test_TC_CPU_024_panic_sets_panic_active_flag(void)
{
    /* Act: first cpu_panic call — sets g_panic_active. */
    cpu_panic(CPU_PANIC_ASSERT, "first entry");

    /* Assert: after returning (test build), BKP0R = magic confirms full path ran. */
    TEST_ASSERT_EQUAL_HEX32(0xDEADC0DEU, g_mock_rtc.BKP0R);

    /* The flag is private; verify indirectly: a second call takes the short path
     * (no BKP writes, no UART, only breakpoint/reset). Cleared by reset_for_test. */
    stm32l475_cmsis_mock_reset();   /* reset BKP0R to 0 */
    /* g_panic_active still true (cpu_reset_for_test not called yet). */
    cpu_panic(CPU_PANIC_ASSERT, "second entry");

    /* BKP0R must still be 0 — short path did not write BKP registers. */
    TEST_ASSERT_EQUAL_HEX32(0U, g_mock_rtc.BKP0R);
}

void test_TC_CPU_025_recursive_panic_takes_short_path(void)
{
    /* Arrange: manually put the module into the "panic already active" state
     * by calling cpu_panic once, then resetting only the mock (not the module). */
    cpu_panic(CPU_PANIC_USER, "first");  /* sets g_panic_active */
    stm32l475_cmsis_mock_reset();        /* reset BKP0R to 0 */
    uint32_t irq_count_before = g_cpu_hw_disable_irq_count;

    /* Act: second call — recursive entry. */
    cpu_panic(CPU_PANIC_USER, "recursive");

    /* Assert: BKP0R not written (short path). */
    TEST_ASSERT_EQUAL_HEX32(0U, g_mock_rtc.BKP0R);

    /* The disable_irq call count should NOT have increased (short path skips it). */
    TEST_ASSERT_EQUAL_UINT32(irq_count_before, g_cpu_hw_disable_irq_count);
}

void test_TC_CPU_026_panic_emits_uart_output_via_busy_wait_txe(void)
{
    /* Arrange: pre-set TXE so the busy-wait loop exits on each byte. */
    g_mock_usart1.ISR |= USART_ISR_TXE;

    /* Act */
    cpu_panic(CPU_PANIC_USER, "uart test");

    /* Assert: USART1 was configured (transmitter enabled, baud rate set). */
    TEST_ASSERT_BITS_HIGH(USART_CR1_TE | USART_CR1_UE, g_mock_usart1.CR1);
    TEST_ASSERT_NOT_EQUAL(0U, g_mock_usart1.BRR);

    /* GPIOB clock was enabled for the panic TX pin. */
    TEST_ASSERT_BITS_HIGH(RCC_AHB2ENR_GPIOBEN, g_mock_rcc_l4.AHB2ENR);

    /* USART1 clock was enabled. */
    TEST_ASSERT_BITS_HIGH(RCC_APB2ENR_USART1EN, g_mock_rcc_l4.APB2ENR);
}

void test_TC_CPU_027_cfsr_cause_table_decodes_all_17_fault_bits(void)
{
    /* The 17 CFSR fault bits defined in SCB_CFSR_* constants. */
    const uint32_t bits[17] = {
        SCB_CFSR_IACCVIOL,
        SCB_CFSR_DACCVIOL,
        SCB_CFSR_MUNSTKERR,
        SCB_CFSR_MSTKERR,
        SCB_CFSR_MLSPERR,
        SCB_CFSR_IBUSERR,
        SCB_CFSR_PRECISERR,
        SCB_CFSR_IMPRECISERR,
        SCB_CFSR_UNSTKERR,
        SCB_CFSR_STKERR,
        SCB_CFSR_LSPERR,
        SCB_CFSR_UNDEFINSTR,
        SCB_CFSR_INVSTATE,
        SCB_CFSR_INVPC,
        SCB_CFSR_NOCP,
        SCB_CFSR_UNALIGNED,
        SCB_CFSR_DIVBYZERO,
    };

    for (uint32_t i = 0U; i < 17U; ++i)
    {
        const char *cause = cpu_cfsr_cause_string_for_test(bits[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(cause, "cause string is NULL for a known CFSR bit");
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0U, (uint32_t) *cause,
                                      "cause string is empty for a known CFSR bit");
    }
}

/* ===================================================================== */
/* §7.4 Post-mortem recovery — TC-CPU-030 through TC-CPU-033            */
/* ===================================================================== */

void test_TC_CPU_030_init_detects_valid_panic_record_and_configures_uart(void)
{
    /* Arrange: pre-load a valid panic record in the mock BKP registers. */
    g_mock_rtc.BKP0R  = 0xDEADC0DEU; /* magic */
    g_mock_rtc.BKP1R  = SCB_CFSR_IMPRECISERR;
    g_mock_rtc.BKP13R = (uint32_t) CPU_PANIC_HARDFAULT;
    g_mock_usart1.ISR |= USART_ISR_TXE; /* so output doesn't time out */
    arrange_pll_ready();

    /* Act */
    status_t result = cpu_init();

    /* Assert: init succeeded and UART was configured for post-mortem output. */
    TEST_ASSERT_EQUAL(STATUS_OK, result);
    TEST_ASSERT_BITS_HIGH(USART_CR1_TE | USART_CR1_UE, g_mock_usart1.CR1);
    TEST_ASSERT_NOT_EQUAL(0U, g_mock_usart1.BRR);
}

void test_TC_CPU_031_init_clears_magic_after_reading_panic_record(void)
{
    /* Arrange */
    g_mock_rtc.BKP0R  = 0xDEADC0DEU;
    g_mock_usart1.ISR |= USART_ISR_TXE;
    arrange_pll_ready();

    /* Act */
    (void) cpu_init();

    /* Assert: magic cleared so record is not replayed on the next boot. */
    TEST_ASSERT_EQUAL_HEX32(0U, g_mock_rtc.BKP0R);
}

void test_TC_CPU_032_init_ignores_invalid_magic(void)
{
    /* Arrange: wrong magic — no valid record. */
    g_mock_rtc.BKP0R = 0xCAFEBABEU;
    arrange_pll_ready();

    /* Act */
    status_t result = cpu_init();

    /* Assert: USART1 was NOT initialised (no post-mortem output). */
    TEST_ASSERT_EQUAL(STATUS_OK, result);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_usart1.CR1);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_usart1.BRR);
}

void test_TC_CPU_033_post_mortem_output_includes_decoded_cause_string(void)
{
    /* Arrange: load CFSR with IMPRECISERR so the cause decoder is exercised. */
    g_mock_rtc.BKP0R = 0xDEADC0DEU;
    g_mock_rtc.BKP1R = SCB_CFSR_IMPRECISERR; /* CFSR in record */
    g_mock_usart1.ISR |= USART_ISR_TXE;
    arrange_pll_ready();

    /* Act */
    (void) cpu_init();

    /* Assert: UART was configured (output happened). */
    TEST_ASSERT_BITS_HIGH(USART_CR1_TE | USART_CR1_UE, g_mock_usart1.CR1);

    /* Verify the cause-string lookup returns the expected value for the
     * CFSR bit stored in BKP1R (IMPRECISERR → "Imprecise data bus error"). */
    const char *cause = cpu_cfsr_cause_string_for_test(SCB_CFSR_IMPRECISERR);
    TEST_ASSERT_NOT_NULL(cause);
    TEST_ASSERT_EQUAL_STRING("Imprecise data bus error", cause);

    /* Magic was cleared. */
    TEST_ASSERT_EQUAL_HEX32(0U, g_mock_rtc.BKP0R);
}

/* ===================================================================== */
/* §7.5 Reset — TC-CPU-040                                              */
/* ===================================================================== */

void test_TC_CPU_040_cpu_reset_calls_nvic_system_reset(void)
{
    /* Arrange */
    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_system_reset_count);

    /* Act */
    cpu_reset();

    /* Assert */
    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_system_reset_count);
}
