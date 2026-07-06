/**
 * @file test_exti_driver_fd.c
 * @brief Unit tests for ExtiDriver — Field Device (STM32F469) register layout.
 *
 * Covers EXTI-T01..T17 per docs/lld/drivers/exti-driver.md §7.
 *
 * ExtiDriver is a single shared source file (firmware/shared/drivers/exti/)
 * compiled once per board via STM32L475xx / STM32F469xx. This test exercises
 * the F469 single-bank register names (IMR/RTSR/FTSR/PR); see
 * test_exti_driver_gw.c for the L475 multi-bank names (IMR1/RTSR1/FTSR1/PR1).
 * Both files share the same test bodies since ExtiDriver's public behaviour
 * is platform-agnostic — only the internal register macro resolution
 * differs, and that's exactly what running the same assertions against both
 * mock layouts verifies.
 *
 * Build: STM32F469xx and TEST must be defined (project.yml :test_exti_driver_fd:).
 */

#include "unity.h"

#include "stm32_cmsis_mock.h"
#include "stm32f469xx.h"
#include "exti_driver.h"

void setUp(void)
{
    stm32_cmsis_mock_reset();
    exti_reset_for_test();
}

void tearDown(void)
{
}

/* --------------------------------------------------------------------- */
/* EXTI-T01: exti_configure(1, PORT_E, RISING)                            */
/* --------------------------------------------------------------------- */

void test_EXTI_T01_configure_line1_port_e_rising(void)
{
    exti_err_t err = exti_configure(1u, EXTI_PORT_E, EXTI_EDGE_RISING);

    TEST_ASSERT_EQUAL(EXTI_ERR_OK, err);
    TEST_ASSERT_EQUAL_HEX32(((uint32_t) EXTI_PORT_E) << 4, SYSCFG->EXTICR[0] & 0xF0u);
    TEST_ASSERT_BITS(1u << 1u, 1u << 1u, EXTI->RTSR);
    TEST_ASSERT_BITS(1u << 1u, 0u, EXTI->FTSR);
}

/* --------------------------------------------------------------------- */
/* EXTI-T02: exti_configure(8, PORT_C, RISING)                            */
/* --------------------------------------------------------------------- */

void test_EXTI_T02_configure_line8_port_c_rising(void)
{
    exti_err_t err = exti_configure(8u, EXTI_PORT_C, EXTI_EDGE_RISING);

    TEST_ASSERT_EQUAL(EXTI_ERR_OK, err);
    TEST_ASSERT_EQUAL_HEX32((uint32_t) EXTI_PORT_C, SYSCFG->EXTICR[2] & 0x0Fu);
    TEST_ASSERT_BITS(1u << 8u, 1u << 8u, EXTI->RTSR);
}

/* --------------------------------------------------------------------- */
/* EXTI-T03: exti_configure(11, PORT_C, RISING)                           */
/* --------------------------------------------------------------------- */

void test_EXTI_T03_configure_line11_port_c_rising(void)
{
    exti_err_t err = exti_configure(11u, EXTI_PORT_C, EXTI_EDGE_RISING);

    TEST_ASSERT_EQUAL(EXTI_ERR_OK, err);
    TEST_ASSERT_EQUAL_HEX32(((uint32_t) EXTI_PORT_C) << 12, SYSCFG->EXTICR[2] & 0xF000u);
    TEST_ASSERT_BITS(1u << 11u, 1u << 11u, EXTI->RTSR);
}

/* --------------------------------------------------------------------- */
/* EXTI-T04: exti_configure same line twice -> conflict                  */
/* --------------------------------------------------------------------- */

void test_EXTI_T04_configure_same_line_twice_returns_conflict(void)
{
    TEST_ASSERT_EQUAL(EXTI_ERR_OK, exti_configure(1u, EXTI_PORT_E, EXTI_EDGE_RISING));
    uint32_t exticr0_before = SYSCFG->EXTICR[0];

    exti_err_t err = exti_configure(1u, EXTI_PORT_A, EXTI_EDGE_FALLING);

    TEST_ASSERT_EQUAL(EXTI_ERR_CONFLICT, err);
    TEST_ASSERT_EQUAL_HEX32(exticr0_before, SYSCFG->EXTICR[0]);
}

/* --------------------------------------------------------------------- */
/* EXTI-T05: exti_configure(16, ...) -> invalid arg                      */
/* --------------------------------------------------------------------- */

void test_EXTI_T05_configure_line16_returns_invalid_arg(void)
{
    exti_err_t err = exti_configure(16u, EXTI_PORT_A, EXTI_EDGE_RISING);

    TEST_ASSERT_EQUAL(EXTI_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_HEX32(0u, SYSCFG->EXTICR[0]);
    TEST_ASSERT_EQUAL_HEX32(0u, SYSCFG->EXTICR[1]);
    TEST_ASSERT_EQUAL_HEX32(0u, SYSCFG->EXTICR[2]);
    TEST_ASSERT_EQUAL_HEX32(0u, SYSCFG->EXTICR[3]);
}

/* --------------------------------------------------------------------- */
/* EXTI-T06: exti_configure(5, PORT_A, FALLING)                           */
/* --------------------------------------------------------------------- */

void test_EXTI_T06_configure_line5_falling_edge(void)
{
    exti_err_t err = exti_configure(5u, EXTI_PORT_A, EXTI_EDGE_FALLING);

    TEST_ASSERT_EQUAL(EXTI_ERR_OK, err);
    TEST_ASSERT_BITS(1u << 5u, 1u << 5u, EXTI->FTSR);
    TEST_ASSERT_BITS(1u << 5u, 0u, EXTI->RTSR);
}

/* --------------------------------------------------------------------- */
/* EXTI-T07: exti_configure(3, PORT_B, BOTH)                              */
/* --------------------------------------------------------------------- */

void test_EXTI_T07_configure_line3_both_edges(void)
{
    exti_err_t err = exti_configure(3u, EXTI_PORT_B, EXTI_EDGE_BOTH);

    TEST_ASSERT_EQUAL(EXTI_ERR_OK, err);
    TEST_ASSERT_BITS(1u << 3u, 1u << 3u, EXTI->RTSR);
    TEST_ASSERT_BITS(1u << 3u, 1u << 3u, EXTI->FTSR);
}

/* --------------------------------------------------------------------- */
/* EXTI-T08: exti_enable(1, 6) after configure                           */
/* --------------------------------------------------------------------- */

void test_EXTI_T08_enable_after_configure(void)
{
    TEST_ASSERT_EQUAL(EXTI_ERR_OK, exti_configure(1u, EXTI_PORT_E, EXTI_EDGE_RISING));

    exti_err_t err = exti_enable(1u, 6u);

    TEST_ASSERT_EQUAL(EXTI_ERR_OK, err);
    TEST_ASSERT_BITS(1u << 1u, 1u << 1u, EXTI->IMR);
    TEST_ASSERT_EQUAL_UINT32(6u, g_mock_nvic_priority[EXTI1_IRQn]);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_nvic_enable_count[EXTI1_IRQn]);
}

/* --------------------------------------------------------------------- */
/* EXTI-T09: exti_enable without prior configure                         */
/* --------------------------------------------------------------------- */

void test_EXTI_T09_enable_without_configure_returns_not_configured(void)
{
    exti_err_t err = exti_enable(1u, 6u);

    TEST_ASSERT_EQUAL(EXTI_ERR_NOT_CONFIGURED, err);
    TEST_ASSERT_EQUAL_HEX32(0u, EXTI->IMR);
}

/* --------------------------------------------------------------------- */
/* EXTI-T10: exti_disable after configure + enable                       */
/* --------------------------------------------------------------------- */

void test_EXTI_T10_disable_after_configure_and_enable(void)
{
    TEST_ASSERT_EQUAL(EXTI_ERR_OK, exti_configure(1u, EXTI_PORT_E, EXTI_EDGE_RISING));
    TEST_ASSERT_EQUAL(EXTI_ERR_OK, exti_enable(1u, 6u));

    exti_err_t err = exti_disable(1u);

    TEST_ASSERT_EQUAL(EXTI_ERR_OK, err);
    TEST_ASSERT_BITS(1u << 1u, 0u, EXTI->IMR);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_nvic_disable_count[EXTI1_IRQn]);
}

/* --------------------------------------------------------------------- */
/* EXTI-T11: exti_disable without prior configure                        */
/* --------------------------------------------------------------------- */

void test_EXTI_T11_disable_without_configure_returns_not_configured(void)
{
    exti_err_t err = exti_disable(3u);

    TEST_ASSERT_EQUAL(EXTI_ERR_NOT_CONFIGURED, err);
}

/* --------------------------------------------------------------------- */
/* EXTI-T12: exti_clear_pending(8)                                        */
/* --------------------------------------------------------------------- */

void test_EXTI_T12_clear_pending_line8(void)
{
    exti_clear_pending(8u);

    TEST_ASSERT_EQUAL_HEX32(1u << 8u, EXTI->PR);
}

/* --------------------------------------------------------------------- */
/* EXTI-T13: EXTICR[0] field isolation across independent lines          */
/* --------------------------------------------------------------------- */

void test_EXTI_T13_exticr_field_isolation(void)
{
    TEST_ASSERT_EQUAL(EXTI_ERR_OK, exti_configure(0u, EXTI_PORT_A, EXTI_EDGE_RISING));
    TEST_ASSERT_EQUAL(EXTI_ERR_OK, exti_configure(1u, EXTI_PORT_E, EXTI_EDGE_RISING));

    TEST_ASSERT_EQUAL_HEX32((uint32_t) EXTI_PORT_A, SYSCFG->EXTICR[0] & 0x0Fu);
    TEST_ASSERT_EQUAL_HEX32(((uint32_t) EXTI_PORT_E) << 4, SYSCFG->EXTICR[0] & 0xF0u);
}

/* --------------------------------------------------------------------- */
/* EXTI-T14: invalid port value                                          */
/* --------------------------------------------------------------------- */

void test_EXTI_T14_configure_invalid_port_returns_invalid_arg(void)
{
    exti_err_t err = exti_configure(2u, (exti_port_t) 0xFFu, EXTI_EDGE_RISING);

    TEST_ASSERT_EQUAL(EXTI_ERR_INVALID_ARG, err);
}

/* --------------------------------------------------------------------- */
/* EXTI-T15: invalid edge value                                          */
/* --------------------------------------------------------------------- */

void test_EXTI_T15_configure_invalid_edge_returns_invalid_arg(void)
{
    exti_err_t err = exti_configure(2u, EXTI_PORT_A, (exti_edge_t) 0xFFu);

    TEST_ASSERT_EQUAL(EXTI_ERR_INVALID_ARG, err);
}

/* --------------------------------------------------------------------- */
/* EXTI-T16: exti_reset_for_test clears the conflict bitmap              */
/* --------------------------------------------------------------------- */

void test_EXTI_T16_reset_for_test_clears_configured_bitmap(void)
{
    TEST_ASSERT_EQUAL(EXTI_ERR_OK, exti_configure(1u, EXTI_PORT_E, EXTI_EDGE_RISING));

    exti_reset_for_test();

    exti_err_t err = exti_configure(1u, EXTI_PORT_A, EXTI_EDGE_FALLING);
    TEST_ASSERT_EQUAL(EXTI_ERR_OK, err);
}

/* --------------------------------------------------------------------- */
/* EXTI-T17: SYSCFG clock enabled on first exti_configure()               */
/* --------------------------------------------------------------------- */

void test_EXTI_T17_syscfg_clock_enabled_on_first_configure(void)
{
    TEST_ASSERT_BITS(RCC_APB2ENR_SYSCFGEN, 0u, RCC->APB2ENR);

    TEST_ASSERT_EQUAL(EXTI_ERR_OK, exti_configure(1u, EXTI_PORT_E, EXTI_EDGE_RISING));

    TEST_ASSERT_BITS(RCC_APB2ENR_SYSCFGEN, RCC_APB2ENR_SYSCFGEN, RCC->APB2ENR);
}
