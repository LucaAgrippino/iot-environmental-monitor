/**
 * @file test_lifecycle_controller_main.c
 * @brief On-target integration test scaffold — IT-LC-001..014.
 *
 * Executed manually on STM32F469I-DISCO (FD) with a debug-UART terminal.
 * Each test case corresponds to a physical setup step; the pass criterion
 * is checked via UART log messages and observable hardware state
 * (LED fault pattern, splash/dismiss on LCD).
 *
 * This file drives the lifecycle_controller module in a real FreeRTOS
 * environment and logs each state transition.  No automated assertion
 * framework is used; the technician reads the UART log.
 *
 * Build: include in the field-device firmware project, compile with
 *   -DBOARD_FIELD_DEVICE -DINTEGRATION_TEST
 *
 * @see docs/lld/application/lifecycle-controller.md §22
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lifecycle_controller/lifecycle_controller.h"
#include "logger/logger.h"

/* ======================================================================= */
/* Integration-test configuration                                          */
/* ======================================================================= */

#define IT_TASK_STACK_WORDS (2048U)
#define IT_MONITOR_PERIOD_MS (500U)

/* ======================================================================= */
/* Monitor task — logs lifecycle state transitions on UART                 */
/* ======================================================================= */

static StackType_t  s_monitor_stack[IT_TASK_STACK_WORDS];
static StaticTask_t s_monitor_tcb;

static void monitor_task(void *arg)
{
    (void)arg;
    lifecycle_state_t prev_state = LIFECYCLE_STATE_INIT;

    /* Wait for the start-gate before entering the polling loop */
    EventGroupHandle_t gate = lifecycle_get_start_gate();
    (void)xEventGroupWaitBits(gate, LIFECYCLE_START_GATE_BIT,
                              pdFALSE, pdTRUE, portMAX_DELAY);

    LOG_INFO("[IT-LC]","Start-gate released — Operational entered");

    for (;;)
    {
        lifecycle_state_t cur = lifecycle_controller->get_state();
        if (cur != prev_state)
        {
            static const char *const state_names[] = {
                "INIT", "OPERATIONAL", "EDITING_CONFIG",
                "RESTARTING", "UPDATING_FW", "FAULTED"
            };
            const char *name = (cur < 6U) ? state_names[cur] : "???";
            LOG_INFO("[IT-LC] State → %s  (reset_cause=%u)",
                     name,
                     (unsigned)lifecycle_controller->get_reset_cause());
            prev_state = cur;
        }
        vTaskDelay(pdMS_TO_TICKS(IT_MONITOR_PERIOD_MS));
    }
}

/* ======================================================================= */
/* it_lc_start — call from application main() after all service init       */
/* ======================================================================= */

/**
 * @brief Launch the lifecycle controller integration-test monitor task.
 *
 * Call this after lifecycle_controller_init() and before vTaskStartScheduler().
 * The lifecycle_task_body() must also be created by the application main().
 */
void it_lc_start(void)
{
    (void)xTaskCreateStatic(monitor_task,
                            "IT_LC_Monitor",
                            IT_TASK_STACK_WORDS,
                            NULL,
                            tskIDLE_PRIORITY + 1U,
                            s_monitor_stack,
                            &s_monitor_tcb);

    LOG_INFO("[IT-LC]","Integration test monitor started");
    LOG_INFO("[IT-LC]","IT-LC-001: Cold boot FD — expect 5 sub-states then Operational");
    LOG_INFO("[IT-LC]","IT-LC-003: Corrupt config — expect Faulted + LED fault pattern");
    LOG_INFO("[IT-LC]", "IT-LC-005: Console 'config commit' — expect EditingConfig then Operational");
    LOG_INFO("[IT-LC]", "IT-LC-007: No console input 5 min — expect Operational (auto-cancel)");
}
