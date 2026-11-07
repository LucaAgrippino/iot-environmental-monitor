/**
 * @file test_cpu_fd.c
 * @brief Unit tests for CpuDriver, Field Device (STM32F469) panic subset.
 *
 * Covers TC-CPU-020..036, 038, 039 and 041 of docs/lld/drivers/cpu-driver.md section 7. The clock
 * and delay cases (001..014, 040) are Gateway only.
 *
 * Mock strategy: RCC, PWR, SCB, DWT, CoreDebug, RTC, USART3 and GPIOB are the mock structs of
 * stm32f469xx.h; the g_cpu_hw_* counters from stm32_cmsis_mock.c verify that the privileged
 * intrinsics (disable_irq, breakpoint, barrier, system_reset) run on the right paths.
 */

#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "cpu_hw.h"
#include "stm32_cmsis_mock.h"
#include "stm32f469xx.h"

/** Word n of the post-mortem record: backup register 2 + n (cpu-driver.md section 3.3). */
#define PANIC_REC(n) ((&g_mock_rtc.BKP0R)[2U + (n)])

void setUp(void)
{
    stm32_cmsis_mock_reset();
    cpu_reset_for_test();
}

void tearDown(void)
{
}

/* ===================================================================== */
/* Panic path: TC-CPU-020 to 027                                         */
/* ===================================================================== */

void test_TC_CPU_020_panic_disables_interrupts_on_entry(void)
{
    cpu_panic(CPU_PANIC_USER, "irq");

    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_disable_irq_count);
}

void test_TC_CPU_021_panic_writes_magic_last_and_the_source(void)
{
    cpu_panic(CPU_PANIC_USER, "magic");

    TEST_ASSERT_EQUAL_HEX32(0xDEADC0DEU, PANIC_REC(0));
    TEST_ASSERT_EQUAL_UINT32((uint32_t) CPU_PANIC_USER, PANIC_REC(13));
    TEST_ASSERT_NOT_EQUAL(0U, PANIC_REC(14)); /* reason hash */
}

void test_TC_CPU_022_panic_writes_cfsr_hfsr_bfar(void)
{
    g_mock_scb.CFSR = 0x00008400U; /* IMPRECISERR + BFARVALID */
    g_mock_scb.HFSR = 0x40000000U;
    g_mock_scb.BFAR = 0x60000000U;

    cpu_panic(CPU_PANIC_BUSFAULT, NULL);

    TEST_ASSERT_EQUAL_HEX32(0x00008400U, PANIC_REC(1));
    TEST_ASSERT_EQUAL_HEX32(0x40000000U, PANIC_REC(2));
    TEST_ASSERT_EQUAL_HEX32(0x60000000U, PANIC_REC(3));
    TEST_ASSERT_EQUAL_UINT32(0U, PANIC_REC(14)); /* NULL reason hashes to 0 */
}

void test_TC_CPU_022b_bfar_is_recorded_as_zero_when_not_valid(void)
{
    g_mock_scb.CFSR = 0x00010000U; /* UNDEFINSTR: BFARVALID clear */
    g_mock_scb.BFAR = 0xE000ED38U; /* stale register content */

    cpu_panic(CPU_PANIC_USAGEFAULT, NULL);

    TEST_ASSERT_EQUAL_HEX32(0U, PANIC_REC(3));
}

void test_TC_CPU_023_fault_entry_writes_frame_pc_lr_sp(void)
{
    uint32_t frame[8] = {
        0x00000001U, /* R0   */
        0x20001000U, /* R1   */
        0x00000002U, /* R2   */
        0xDEADBEEFU, /* R3   */
        0x0000000CU, /* R12  */
        0x08001A20U, /* LR   */
        0x08001A34U, /* PC   */
        0x61000000U, /* xPSR */
    };

    cpu_fault_entry(frame, CPU_PANIC_HARDFAULT);

    TEST_ASSERT_EQUAL_HEX32(0x08001A34U, PANIC_REC(4));
    TEST_ASSERT_EQUAL_HEX32(0x08001A20U, PANIC_REC(5));
    TEST_ASSERT_EQUAL_HEX32((uint32_t) (uintptr_t) frame, PANIC_REC(6));
    TEST_ASSERT_EQUAL_HEX32(0x61000000U, PANIC_REC(7));
    TEST_ASSERT_EQUAL_HEX32(0x00000001U, PANIC_REC(8));
    TEST_ASSERT_EQUAL_HEX32(0x20001000U, PANIC_REC(9));
    TEST_ASSERT_EQUAL_HEX32(0x00000002U, PANIC_REC(10));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFU, PANIC_REC(11));
    TEST_ASSERT_EQUAL_HEX32(0x0000000CU, PANIC_REC(12));
}

void test_TC_CPU_024_second_panic_takes_the_short_path(void)
{
    cpu_panic(CPU_PANIC_ASSERT, "first");
    TEST_ASSERT_EQUAL_HEX32(0xDEADC0DEU, PANIC_REC(0));

    stm32_cmsis_mock_reset(); /* clear the record and the counters, not the module state */
    cpu_panic(CPU_PANIC_ASSERT, "second");

    TEST_ASSERT_EQUAL_HEX32(0U, PANIC_REC(0));                   /* nothing written */
    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_disable_irq_count);    /* no second entry work */
    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_system_reset_count);   /* straight to the reset */
}

void test_TC_CPU_025_recursive_panic_skips_the_uart(void)
{
    cpu_panic(CPU_PANIC_USER, "first");
    stm32_cmsis_mock_reset();

    cpu_panic(CPU_PANIC_USER, "recursive");

    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_usart3.CR1); /* UART not configured again */
}

void test_TC_CPU_026_panic_configures_and_writes_the_uart(void)
{
    g_mock_usart3.SR |= USART_SR_TXE | USART_SR_TC;

    cpu_panic(CPU_PANIC_USER, "uart test");

    TEST_ASSERT_BITS_HIGH(USART_CR1_TE | USART_CR1_UE, g_mock_usart3.CR1);
    TEST_ASSERT_EQUAL_UINT32(391U, g_mock_usart3.BRR); /* 45 MHz / 115 200 */
    TEST_ASSERT_BITS_HIGH(RCC_AHB1ENR_GPIOBEN, g_mock_rcc.AHB1ENR);
    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR_USART3EN, g_mock_rcc.APB1ENR);
    /* PB10 is alternate function 7 (AFR[1] bits 11:8). */
    TEST_ASSERT_EQUAL_UINT32(7U, (g_mock_gpio[1].AFR[1] >> 8U) & 0xFU);
    TEST_ASSERT_EQUAL_UINT32(2U, (g_mock_gpio[1].MODER >> 20U) & 3U);
}

void test_TC_CPU_027_cfsr_table_decodes_all_17_fault_bits(void)
{
    const uint32_t bits[17] = {
        SCB_CFSR_IACCVIOL,   SCB_CFSR_DACCVIOL,    SCB_CFSR_MUNSTKERR, SCB_CFSR_MSTKERR,
        SCB_CFSR_MLSPERR,    SCB_CFSR_IBUSERR,     SCB_CFSR_PRECISERR, SCB_CFSR_IMPRECISERR,
        SCB_CFSR_UNSTKERR,   SCB_CFSR_STKERR,      SCB_CFSR_LSPERR,    SCB_CFSR_UNDEFINSTR,
        SCB_CFSR_INVSTATE,   SCB_CFSR_INVPC,       SCB_CFSR_NOCP,      SCB_CFSR_UNALIGNED,
        SCB_CFSR_DIVBYZERO,
    };

    for (uint32_t i = 0U; i < 17U; ++i)
    {
        const char *cause = cpu_cfsr_cause_string_for_test(bits[i]);
        TEST_ASSERT_NOT_NULL(cause);
        TEST_ASSERT_NOT_EQUAL(0, (int) *cause);
        TEST_ASSERT_NOT_EQUAL(0, strcmp("Unknown fault", cause));
    }
    TEST_ASSERT_EQUAL_STRING("Unknown fault", cpu_cfsr_cause_string_for_test(0U));
    TEST_ASSERT_EQUAL_STRING("Divide by zero", cpu_cfsr_cause_string_for_test(SCB_CFSR_DIVBYZERO));
}

/* ===================================================================== */
/* Post-mortem: TC-CPU-030 to 033                                        */
/* ===================================================================== */

void test_TC_CPU_030_init_prints_a_valid_record_on_the_uart(void)
{
    PANIC_REC(0) = 0xDEADC0DEU;
    PANIC_REC(1) = SCB_CFSR_IMPRECISERR;
    PANIC_REC(13) = (uint32_t) CPU_PANIC_HARDFAULT;
    g_mock_usart3.SR |= USART_SR_TXE | USART_SR_TC;

    TEST_ASSERT_EQUAL(STATUS_OK, cpu_init());

    TEST_ASSERT_BITS_HIGH(USART_CR1_TE | USART_CR1_UE, g_mock_usart3.CR1);
    TEST_ASSERT_NOT_EQUAL(0U, g_mock_usart3.BRR);
}

void test_TC_CPU_031_init_clears_the_magic_after_reporting(void)
{
    PANIC_REC(0) = 0xDEADC0DEU;
    g_mock_usart3.SR |= USART_SR_TXE | USART_SR_TC;

    (void) cpu_init();

    TEST_ASSERT_EQUAL_HEX32(0U, PANIC_REC(0));
    TEST_ASSERT_BITS_HIGH(PWR_CR_DBP, g_mock_pwr.CR); /* backup domain writable for the clear */
}

void test_TC_CPU_032_init_ignores_an_invalid_magic(void)
{
    PANIC_REC(0) = 0xCAFEBABEU;

    TEST_ASSERT_EQUAL(STATUS_OK, cpu_init());

    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_usart3.CR1);
    TEST_ASSERT_EQUAL_HEX32(0xCAFEBABEU, PANIC_REC(0)); /* left alone */
}

void test_TC_CPU_033_init_without_a_record_prints_nothing(void)
{
    TEST_ASSERT_EQUAL(STATUS_OK, cpu_init());

    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_usart3.CR1);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_usart3.DR);
}

/* ===================================================================== */
/* Placement, sources, init: TC-CPU-034 to 039                           */
/* ===================================================================== */

void test_TC_CPU_034_record_does_not_touch_backup_registers_0_and_1(void)
{
    g_mock_rtc.BKP0R = 0x5A5A5A5AU; /* TimeProvider's sync flag */
    g_mock_rtc.BKP1R_to_BKP19R[0] = 0x12345678U;

    cpu_panic(CPU_PANIC_USER, "placement");

    TEST_ASSERT_EQUAL_HEX32(0x5A5A5A5AU, g_mock_rtc.BKP0R);
    TEST_ASSERT_EQUAL_HEX32(0x12345678U, g_mock_rtc.BKP1R_to_BKP19R[0]);
    TEST_ASSERT_EQUAL_HEX32(0xDEADC0DEU, g_mock_rtc.BKP1R_to_BKP19R[1]); /* register 2 */
}


void test_TC_CPU_035_fault_entry_records_each_vector_source(void)
{
    uint32_t frame[8] = {0U, 0U, 0U, 0U, 0U, 0x08000100U, 0x08000200U, 0x01000000U};
    const cpu_panic_source_t sources[] = {CPU_PANIC_HARDFAULT, CPU_PANIC_BUSFAULT,
                                          CPU_PANIC_MEMMANAGE, CPU_PANIC_USAGEFAULT};

    for (uint32_t i = 0U; i < (sizeof(sources) / sizeof(sources[0])); ++i)
    {
        cpu_reset_for_test();
        stm32_cmsis_mock_reset();

        cpu_fault_entry(frame, sources[i]);

        TEST_ASSERT_EQUAL_UINT32((uint32_t) sources[i], PANIC_REC(13));
        TEST_ASSERT_EQUAL_HEX32(0x08000200U, PANIC_REC(4));
    }
}

void test_TC_CPU_036_init_enables_the_configurable_fault_handlers(void)
{
    TEST_ASSERT_EQUAL(STATUS_OK, cpu_init());

    TEST_ASSERT_BITS_HIGH(SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk |
                              SCB_SHCSR_USGFAULTENA_Msk,
                          g_mock_scb.SHCSR);
}

void test_TC_CPU_038_init_returns_ok_and_leaves_the_cycle_counter_alone(void)
{
    TEST_ASSERT_EQUAL(STATUS_OK, cpu_init());

    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_dwt.CTRL);
}

void test_TC_CPU_039_halt_or_reset_matches_the_build(void)
{
    cpu_panic(CPU_PANIC_USER, "mode");

#ifdef DEBUG
    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_breakpoint_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_system_reset_count);
#else
    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_breakpoint_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_system_reset_count);
#endif
}

void test_TC_CPU_042_record_holds_the_rtc_time_and_date(void)
{
    g_mock_rtc.TR = 0x00123456U;
    g_mock_rtc.DR = 0x00261107U;

    cpu_panic(CPU_PANIC_USER, "time");

    TEST_ASSERT_EQUAL_HEX32(0x00123456U, PANIC_REC(15));
    TEST_ASSERT_EQUAL_HEX32(0x00261107U, PANIC_REC(16));
}

/* ===================================================================== */
/* Reset: TC-CPU-041                                                     */
/* ===================================================================== */

void test_TC_CPU_041_cpu_reset_issues_a_barrier_then_the_reset(void)
{
    cpu_reset();

    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_system_reset_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_barriers_at_reset);
    TEST_ASSERT_EQUAL_HEX32(0U, PANIC_REC(0)); /* a clean reset leaves no record */
}
