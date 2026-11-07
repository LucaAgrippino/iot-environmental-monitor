/**
 * @file test_cpu_debug_fd.c
 * @brief CpuDriver end-of-panic policy in a DEBUG build (Field Device).
 *
 * Built with DEBUG defined (see the project.yml defines): with a debugger attached the panic stops at
 * a breakpoint; without one it resets exactly like a release build, because a BKPT with no
 * debugger is itself a fault (cpu-driver.md CPU-D3).
 */

#include "unity.h"

#include <stdint.h>

#include "cpu.h"
#include "cpu_hw.h"
#include "stm32_cmsis_mock.h"
#include "stm32f469xx.h"

void setUp(void)
{
    stm32_cmsis_mock_reset();
    cpu_reset_for_test();
}

void tearDown(void)
{
}

void test_TC_CPU_039a_debug_build_with_a_debugger_stops_at_the_breakpoint(void)
{
    g_cpu_hw_debugger_attached = true;

    cpu_panic(CPU_PANIC_USER, "halt");

    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_breakpoint_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_system_reset_count);
}

void test_TC_CPU_039b_debug_build_without_a_debugger_resets(void)
{
    g_cpu_hw_debugger_attached = false;

    cpu_panic(CPU_PANIC_USER, "no debugger");

    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_breakpoint_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_system_reset_count);
}

void test_TC_CPU_039c_nested_panic_follows_the_same_rule(void)
{
    g_cpu_hw_debugger_attached = false;
    cpu_panic(CPU_PANIC_USER, "first");
    stm32_cmsis_mock_reset();
    g_cpu_hw_debugger_attached = true;

    cpu_panic(CPU_PANIC_USER, "nested"); /* module state kept: this is the recursive entry */

    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_breakpoint_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_system_reset_count);
}

void test_TC_CPU_039d_assert_macro_panics_in_a_debug_build(void)
{
    CPU_ASSERT(1 == 2);

    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_disable_irq_count);
}

void test_TC_CPU_039e_assert_macro_passes_when_the_expression_holds(void)
{
    CPU_ASSERT(1 == 1);

    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_disable_irq_count);
}
