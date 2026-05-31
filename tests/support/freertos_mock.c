/**
 * @file freertos_mock.c
 * @brief Implementation of FreeRTOS host stubs for unit tests.
 *
 * Holds the g_mock_* global state and the function-stub bodies declared in
 * tests/support/FreeRTOS.h.
 */

#include "FreeRTOS.h"

#include <string.h>

/* --------------------------------------------------------------------- */
/* Mock state — defaults reset by mock_freertos_reset()                   */
/* --------------------------------------------------------------------- */

BaseType_t_eTaskState g_mock_scheduler_state;
TickType_t            g_mock_tick_count;
TickType_t            g_mock_tick_count_from_isr;

QueueHandle_t  g_mock_xQueueCreateStatic_return;
uint32_t       g_mock_xQueueCreateStatic_call_count;

TaskHandle_t   g_mock_xTaskCreateStatic_return;
uint32_t       g_mock_xTaskCreateStatic_call_count;
TaskFunction_t g_mock_xTaskCreateStatic_last_fn;

BaseType_t  g_mock_xQueueSend_return;
uint32_t    g_mock_xQueueSend_call_count;
uint8_t     g_mock_xQueueSend_last_item[256];
size_t      g_mock_xQueueSend_last_item_size;

BaseType_t  g_mock_xQueueSendFromISR_return;
uint32_t    g_mock_xQueueSendFromISR_call_count;
uint8_t     g_mock_xQueueSendFromISR_last_item[256];

BaseType_t  g_mock_xQueueReceive_return;
uint32_t    g_mock_xQueueReceive_call_count;
uint8_t     g_mock_xQueueReceive_next_item[256];
size_t      g_mock_xQueueReceive_next_item_size;

/* A canned non-NULL handle used as the default return value of the
 * static-create functions. Tests don't dereference it. */
static int g_dummy_handle_sentinel;
#define DUMMY_HANDLE  ((void *)&g_dummy_handle_sentinel)

/* --------------------------------------------------------------------- */
/* Reset                                                                  */
/* --------------------------------------------------------------------- */

void mock_freertos_reset(void)
{
    g_mock_scheduler_state              = taskSCHEDULER_NOT_STARTED;
    g_mock_tick_count                   = 0U;
    g_mock_tick_count_from_isr          = 0U;

    g_mock_xQueueCreateStatic_return    = DUMMY_HANDLE;
    g_mock_xQueueCreateStatic_call_count = 0U;

    g_mock_xTaskCreateStatic_return     = DUMMY_HANDLE;
    g_mock_xTaskCreateStatic_call_count = 0U;
    g_mock_xTaskCreateStatic_last_fn    = NULL;

    g_mock_xQueueSend_return            = pdTRUE;
    g_mock_xQueueSend_call_count        = 0U;
    (void)memset(g_mock_xQueueSend_last_item, 0,
                 sizeof(g_mock_xQueueSend_last_item));
    g_mock_xQueueSend_last_item_size    = 0U;

    g_mock_xQueueSendFromISR_return     = pdTRUE;
    g_mock_xQueueSendFromISR_call_count = 0U;
    (void)memset(g_mock_xQueueSendFromISR_last_item, 0,
                 sizeof(g_mock_xQueueSendFromISR_last_item));

    g_mock_xQueueReceive_return         = pdTRUE;
    g_mock_xQueueReceive_call_count     = 0U;
    (void)memset(g_mock_xQueueReceive_next_item, 0,
                 sizeof(g_mock_xQueueReceive_next_item));
    g_mock_xQueueReceive_next_item_size = 0U;
}

/* --------------------------------------------------------------------- */
/* API stubs                                                              */
/* --------------------------------------------------------------------- */

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *ctrl)
{
    (void)length;
    (void)item_size;
    (void)storage;
    (void)ctrl;
    g_mock_xQueueCreateStatic_call_count++;
    return g_mock_xQueueCreateStatic_return;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)q;
    (void)wait;
    g_mock_xQueueSend_call_count++;
    if ((item != NULL) && (g_mock_xQueueSend_last_item_size > 0U))
    {
        (void)memcpy(g_mock_xQueueSend_last_item, item,
                     g_mock_xQueueSend_last_item_size);
    }
    return g_mock_xQueueSend_return;
}

BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item,
                             BaseType_t *woken)
{
    (void)q;
    if (woken != NULL) { *woken = pdFALSE; }
    g_mock_xQueueSendFromISR_call_count++;
    if (item != NULL)
    {
        (void)memcpy(g_mock_xQueueSendFromISR_last_item, item,
                     sizeof(g_mock_xQueueSendFromISR_last_item) < 256U
                         ? sizeof(g_mock_xQueueSendFromISR_last_item) : 256U);
    }
    return g_mock_xQueueSendFromISR_return;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait)
{
    (void)q;
    (void)wait;
    g_mock_xQueueReceive_call_count++;
    if ((g_mock_xQueueReceive_return == pdTRUE) &&
        (out != NULL) &&
        (g_mock_xQueueReceive_next_item_size > 0U))
    {
        (void)memcpy(out, g_mock_xQueueReceive_next_item,
                     g_mock_xQueueReceive_next_item_size);
    }
    return g_mock_xQueueReceive_return;
}

TaskHandle_t xTaskCreateStatic(TaskFunction_t fn, const char *name,
                               uint32_t stack_words, void *arg,
                               UBaseType_t priority,
                               StackType_t *stack, StaticTask_t *tcb)
{
    (void)name;
    (void)stack_words;
    (void)arg;
    (void)priority;
    (void)stack;
    (void)tcb;
    g_mock_xTaskCreateStatic_call_count++;
    g_mock_xTaskCreateStatic_last_fn = fn;
    return g_mock_xTaskCreateStatic_return;
}

BaseType_t_eTaskState xTaskGetSchedulerState(void)
{
    return g_mock_scheduler_state;
}

TickType_t xTaskGetTickCount(void)
{
    return g_mock_tick_count;
}

TickType_t xTaskGetTickCountFromISR(void)
{
    return g_mock_tick_count_from_isr;
}
