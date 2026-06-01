/**
 * @file FreeRTOS.h (mock)
 * @brief Hand-written FreeRTOS stub for host-side unit tests.
 *
 * Shadows the real FreeRTOS.h on the test include path. Provides just
 * enough surface for Logger (and future Middleware) tests: type stubs,
 * scheduler-state query, queue API, static task creation, tick counters,
 * critical-section macros.
 *
 * Mock state and configurable behaviour are exposed as globals prefixed
 * g_mock_*. Tests set these before exercising the SUT and inspect them
 * afterwards. Call mock_freertos_reset() in setUp().
 */

#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --------------------------------------------------------------------- */
/* Core types                                                             */
/* --------------------------------------------------------------------- */

typedef int32_t   BaseType_t;
typedef uint32_t  UBaseType_t;
typedef uint32_t  StackType_t;
typedef uint32_t  TickType_t;

typedef struct { uint32_t _dummy[4]; } StaticQueue_t;
typedef struct { uint32_t _dummy[4]; } StaticTask_t;

typedef void  *QueueHandle_t;
typedef void  *TaskHandle_t;
typedef void  *TimerHandle_t;
typedef struct { uint32_t _dummy[8]; } StaticTimer_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t xTimer);

/* In FreeRTOS, semaphores are implemented as queues. */
typedef QueueHandle_t  SemaphoreHandle_t;
typedef StaticQueue_t  StaticSemaphore_t;
typedef void (*TaskFunction_t)(void *);

/* --------------------------------------------------------------------- */
/* Macros and constants                                                   */
/* --------------------------------------------------------------------- */

#define pdTRUE              (1)
#define pdFALSE             (0)
#define pdPASS              pdTRUE
#define pdFAIL              pdFALSE

#define portMAX_DELAY       (0xFFFFFFFFUL)
#define portTICK_PERIOD_MS  (1U)
#define configTICK_RATE_HZ  (1000U)
#define tskIDLE_PRIORITY    (0U)

#define pdMS_TO_TICKS(x)    ((TickType_t)(x))

/* Scheduler-state enum — matches FreeRTOS naming. */
typedef enum
{
    taskSCHEDULER_NOT_STARTED = 0,
    taskSCHEDULER_RUNNING     = 1,
    taskSCHEDULER_SUSPENDED   = 2
} BaseType_t_eTaskState;

/* Critical-section macros: no-ops in the host build. */
#define taskENTER_CRITICAL()              ((void)0)
#define taskEXIT_CRITICAL()               ((void)0)
#define taskENTER_CRITICAL_FROM_ISR()     ((UBaseType_t)0)
#define taskEXIT_CRITICAL_FROM_ISR(mask)  ((void)(mask))
#define portYIELD_FROM_ISR(woken)         ((void)(woken))

/* --------------------------------------------------------------------- */
/* Mock state — set by tests before calling SUT; inspected after          */
/* --------------------------------------------------------------------- */

/* Scheduler state returned by xTaskGetSchedulerState() */
extern BaseType_t_eTaskState g_mock_scheduler_state;

/* Tick counters returned by xTaskGetTickCount / FromISR */
extern TickType_t g_mock_tick_count;
extern TickType_t g_mock_tick_count_from_isr;

/* xQueueCreateStatic / xTaskCreateStatic returns + call counts */
extern QueueHandle_t g_mock_xQueueCreateStatic_return;
extern uint32_t      g_mock_xQueueCreateStatic_call_count;

extern TaskHandle_t  g_mock_xTaskCreateStatic_return;
extern uint32_t      g_mock_xTaskCreateStatic_call_count;
extern TaskFunction_t g_mock_xTaskCreateStatic_last_fn;

/* xQueueSend mock: configurable return + last-item capture */
extern BaseType_t g_mock_xQueueSend_return;
extern uint32_t   g_mock_xQueueSend_call_count;
extern uint8_t    g_mock_xQueueSend_last_item[256];
extern size_t     g_mock_xQueueSend_last_item_size;

/* xQueueSendFromISR mock */
extern BaseType_t g_mock_xQueueSendFromISR_return;
extern uint32_t   g_mock_xQueueSendFromISR_call_count;
extern uint8_t    g_mock_xQueueSendFromISR_last_item[256];

/* xQueueReceive mock: pre-loaded item to deliver, configurable return */
extern BaseType_t g_mock_xQueueReceive_return;
extern uint32_t   g_mock_xQueueReceive_call_count;
extern uint8_t    g_mock_xQueueReceive_next_item[256];
extern size_t     g_mock_xQueueReceive_next_item_size;

/* xSemaphoreCreateMutexStatic mock */
extern SemaphoreHandle_t g_mock_xSemaphoreCreateMutexStatic_return;

/* xSemaphoreTake / xSemaphoreGive mocks */
extern BaseType_t g_mock_xSemaphoreTake_return;
extern uint32_t   g_mock_xSemaphoreTake_call_count;
extern BaseType_t g_mock_xSemaphoreGive_return;
extern uint32_t   g_mock_xSemaphoreGive_call_count;

/* uxTaskGetStackHighWaterMark mock */
extern UBaseType_t g_mock_uxTaskGetStackHighWaterMark_return;

/* xTimerCreateStatic / xTimerStart mock */
extern TimerHandle_t g_mock_xTimerCreateStatic_return;
extern uint32_t      g_mock_xTimerCreateStatic_call_count;
extern BaseType_t    g_mock_xTimerStart_return;
extern uint32_t      g_mock_xTimerStart_call_count;

/* xTaskGetCurrentTaskHandle mock */
extern TaskHandle_t  g_mock_xTaskGetCurrentTaskHandle_return;

/* xTaskNotifyGive / ulTaskNotifyTake mock */
extern uint32_t      g_mock_xTaskNotifyGive_call_count;
extern uint32_t      g_mock_ulTaskNotifyTake_return;

/* Reset all g_mock_* state to defaults. Call from setUp(). */
void mock_freertos_reset(void);

/* --------------------------------------------------------------------- */
/* FreeRTOS API stubs                                                     */
/* --------------------------------------------------------------------- */

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *ctrl);

BaseType_t    xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t    xQueueSendFromISR(QueueHandle_t q, const void *item,
                                BaseType_t *woken);
BaseType_t    xQueueReceive(QueueHandle_t q, void *out, TickType_t wait);

TaskHandle_t  xTaskCreateStatic(TaskFunction_t fn, const char *name,
                                uint32_t stack_words, void *arg,
                                UBaseType_t priority,
                                StackType_t *stack, StaticTask_t *tcb);

BaseType_t_eTaskState xTaskGetSchedulerState(void);
TickType_t            xTaskGetTickCount(void);
TickType_t            xTaskGetTickCountFromISR(void);

SemaphoreHandle_t     xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf);
BaseType_t            xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t            xSemaphoreGive(SemaphoreHandle_t sem);
UBaseType_t           uxTaskGetStackHighWaterMark(TaskHandle_t task);

TimerHandle_t xTimerCreateStatic(const char *name, TickType_t period,
                                  UBaseType_t reload, void *id,
                                  TimerCallbackFunction_t cb,
                                  StaticTimer_t *buf);
BaseType_t    xTimerStart(TimerHandle_t timer, TickType_t wait);
TaskHandle_t  xTaskGetCurrentTaskHandle(void);
void          xTaskNotifyGive(TaskHandle_t task);
uint32_t      ulTaskNotifyTake(BaseType_t clear, TickType_t wait);

#endif /* FREERTOS_H */
