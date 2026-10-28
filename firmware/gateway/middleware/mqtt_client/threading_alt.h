/**
 * @file threading_alt.h
 * @brief mbedTLS threading layer over FreeRTOS static mutexes (MBEDTLS_THREADING_ALT).
 *
 * mbedTLS's threading.h includes this header by name when MBEDTLS_THREADING_ALT is defined
 * (mbedtls_config_gateway.h). The mutex is a statically allocated FreeRTOS mutex embedded in the
 * mbedTLS object that owns it, so no heap is involved (REQ-NF-408). The functions are installed
 * by tls_arena_init(); nothing else includes this file.
 */

#ifndef THREADING_ALT_H
#define THREADING_ALT_H

#include "FreeRTOS.h"
#include "semphr.h"

/** @brief The mutex type mbedTLS embeds in its contexts. */
typedef struct mbedtls_threading_mutex_t
{
    SemaphoreHandle_t handle;  /**< Handle returned by xSemaphoreCreateMutexStatic().  */
    StaticSemaphore_t storage; /**< Kernel control block, owned by this structure.     */
} mbedtls_threading_mutex_t;

#endif /* THREADING_ALT_H */
