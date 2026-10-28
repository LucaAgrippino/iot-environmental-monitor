/**
 * @file tls_arena.c
 * @brief Static memory arena for mbedTLS — implementation. See tls_arena.h.
 */

#include "tls_arena.h"

#include <stddef.h>

#include "FreeRTOS.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/threading.h"
#include "semphr.h"

/** @brief mbedTLS aligns blocks inside the buffer itself; the buffer start must be 8-aligned. */
#define ARENA_ALIGNMENT_BYTES (8U)

static uint8_t s_arena[TLS_ARENA_SIZE_BYTES] __attribute__((aligned(ARENA_ALIGNMENT_BYTES)));
static bool s_ready;

/* ---------------------------------------------------------------------- */
/* mbedTLS threading layer over FreeRTOS                                    */
/* ---------------------------------------------------------------------- */

static void prv_mutex_init(mbedtls_threading_mutex_t *mutex)
{
    if (mutex != NULL)
    {
        mutex->handle = xSemaphoreCreateMutexStatic(&mutex->storage);
    }
}

static void prv_mutex_free(mbedtls_threading_mutex_t *mutex)
{
    if (mutex != NULL)
    {
        /* Static storage: there is nothing to give back. The handle is cleared so a use after
         * free fails the lock below instead of touching a dead control block. */
        mutex->handle = NULL;
    }
}

static int prv_mutex_lock(mbedtls_threading_mutex_t *mutex)
{
    if ((mutex == NULL) || (mutex->handle == NULL))
    {
        return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    }

    return (xSemaphoreTake(mutex->handle, portMAX_DELAY) == pdTRUE)
               ? 0
               : MBEDTLS_ERR_THREADING_MUTEX_ERROR;
}

static int prv_mutex_unlock(mbedtls_threading_mutex_t *mutex)
{
    if ((mutex == NULL) || (mutex->handle == NULL))
    {
        return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    }

    return (xSemaphoreGive(mutex->handle) == pdTRUE) ? 0 : MBEDTLS_ERR_THREADING_MUTEX_ERROR;
}

/* ---------------------------------------------------------------------- */
/* Default allocator until tls_arena_init(): refuses everything             */
/* ---------------------------------------------------------------------- */

void *tls_arena_uninit_calloc(size_t count, size_t size)
{
    (void) count;
    (void) size;
    return NULL;
}

void tls_arena_uninit_free(void *ptr)
{
    (void) ptr;
}

/* ---------------------------------------------------------------------- */
/* Public API                                                               */
/* ---------------------------------------------------------------------- */

void tls_arena_init(void)
{
    if (s_ready)
    {
        return;
    }

    /* The mutex layer first: the allocator creates its own mutex when it is initialised. */
    mbedtls_threading_set_alt(prv_mutex_init, prv_mutex_free, prv_mutex_lock, prv_mutex_unlock);
    mbedtls_memory_buffer_alloc_init(s_arena, sizeof(s_arena));

    s_ready = true;
}

bool tls_arena_is_ready(void)
{
    return s_ready;
}

uint32_t tls_arena_peak_bytes(void)
{
    size_t max_used = 0U;
    size_t max_blocks = 0U;

    if (!s_ready)
    {
        return 0U;
    }

    mbedtls_memory_buffer_alloc_max_get(&max_used, &max_blocks);
    return (uint32_t) max_used;
}

void tls_arena_reset_peak(void)
{
    if (s_ready)
    {
        mbedtls_memory_buffer_alloc_max_reset();
    }
}

#ifdef TEST
void tls_arena_reset_for_test(void)
{
    s_ready = false;
}
#endif
