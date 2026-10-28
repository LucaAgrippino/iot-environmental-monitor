/**
 * @file test_tls_arena.c
 * @brief Unity unit tests for the mbedTLS static arena — TLSA-T01 through TLSA-T15.
 *
 * Built with the Gateway mbedTLS configuration (MBEDTLS_CONFIG_FILE), so the types the arena
 * code uses (the FreeRTOS-backed mutex, the memory-buffer-allocator declarations) are the real
 * ones. The three mbedTLS entry points tls_arena.c calls are hand-written fakes defined here:
 * the real memory_buffer_alloc.c and threading.c are not linked (they are not on a Ceedling
 * source path), and what is under test is the order and the arguments of the calls, not mbedTLS.
 * The FreeRTOS semaphore calls are the shared mock (freertos_mock.h).
 */

#include "unity.h"

#include <stddef.h>
#include <stdint.h>

#include "freertos_mock.h"
#include "tls_arena.h"

#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/threading.h"

/* ---------------------------------------------------------------------- */
/* Fakes of the mbedTLS entry points tls_arena.c calls                      */
/* ---------------------------------------------------------------------- */

typedef void (*mutex_init_fn)(mbedtls_threading_mutex_t *);
typedef void (*mutex_free_fn)(mbedtls_threading_mutex_t *);
typedef int (*mutex_op_fn)(mbedtls_threading_mutex_t *);

static mutex_init_fn s_init;
static mutex_free_fn s_free;
static mutex_op_fn s_lock;
static mutex_op_fn s_unlock;

static uint32_t s_call_seq;
static uint32_t s_set_alt_at;
static uint32_t s_alloc_init_at;
static uint32_t s_alloc_init_calls;
static unsigned char *s_alloc_buf;
static size_t s_alloc_len;
static size_t s_fake_max_used;
static uint32_t s_reset_calls;

void mbedtls_threading_set_alt(void (*mutex_init)(mbedtls_threading_mutex_t *),
                               void (*mutex_free)(mbedtls_threading_mutex_t *),
                               int (*mutex_lock)(mbedtls_threading_mutex_t *),
                               int (*mutex_unlock)(mbedtls_threading_mutex_t *))
{
    s_init = mutex_init;
    s_free = mutex_free;
    s_lock = mutex_lock;
    s_unlock = mutex_unlock;
    s_set_alt_at = ++s_call_seq;
}

void mbedtls_memory_buffer_alloc_init(unsigned char *buf, size_t len)
{
    s_alloc_buf = buf;
    s_alloc_len = len;
    s_alloc_init_at = ++s_call_seq;
    s_alloc_init_calls++;
}

void mbedtls_memory_buffer_alloc_max_get(size_t *max_used, size_t *max_blocks)
{
    *max_used = s_fake_max_used;
    *max_blocks = 1U;
}

void mbedtls_memory_buffer_alloc_max_reset(void)
{
    s_reset_calls++;
}

/* ---------------------------------------------------------------------- */

void setUp(void)
{
    mock_freertos_reset();
    tls_arena_reset_for_test();

    s_init = NULL;
    s_free = NULL;
    s_lock = NULL;
    s_unlock = NULL;
    s_call_seq = 0U;
    s_set_alt_at = 0U;
    s_alloc_init_at = 0U;
    s_alloc_init_calls = 0U;
    s_alloc_buf = NULL;
    s_alloc_len = 0U;
    s_fake_max_used = 0U;
    s_reset_calls = 0U;
}

void tearDown(void)
{
}

/* TLSA-T01 */
void test_not_ready_before_init(void)
{
    TEST_ASSERT_FALSE(tls_arena_is_ready());
}

/* TLSA-T02 */
void test_init_hands_the_whole_arena_to_mbedtls(void)
{
    tls_arena_init();

    TEST_ASSERT_TRUE(tls_arena_is_ready());
    TEST_ASSERT_NOT_NULL(s_alloc_buf);
    TEST_ASSERT_EQUAL_UINT32(TLS_ARENA_SIZE_BYTES, (uint32_t) s_alloc_len);
}

/* TLSA-T03: the allocator creates its own mutex when it starts, so the layer comes first. */
void test_init_installs_the_mutex_layer_before_the_allocator(void)
{
    tls_arena_init();

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, s_set_alt_at);
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, s_alloc_init_at);
    TEST_ASSERT_LESS_THAN_UINT32(s_alloc_init_at, s_set_alt_at);
}

/* TLSA-T04 */
void test_init_installs_all_four_mutex_functions(void)
{
    tls_arena_init();

    TEST_ASSERT_NOT_NULL(s_init);
    TEST_ASSERT_NOT_NULL(s_free);
    TEST_ASSERT_NOT_NULL(s_lock);
    TEST_ASSERT_NOT_NULL(s_unlock);
}

/* TLSA-T05: a second call must not reset an allocator that is in use. */
void test_second_init_does_nothing(void)
{
    tls_arena_init();
    tls_arena_init();

    TEST_ASSERT_EQUAL_UINT32(1U, s_alloc_init_calls);
}

/* TLSA-T06: the buffer start is 8-byte aligned. */
void test_arena_is_eight_byte_aligned(void)
{
    tls_arena_init();

    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t) ((uintptr_t) s_alloc_buf % 8U));
}

/* TLSA-T07 */
void test_mutex_init_creates_a_static_mutex(void)
{
    mbedtls_threading_mutex_t m = {0};

    tls_arena_init();
    s_init(&m);

    TEST_ASSERT_NOT_NULL(m.handle);
}

/* TLSA-T08 */
void test_mutex_lock_takes_the_semaphore(void)
{
    mbedtls_threading_mutex_t m = {0};

    tls_arena_init();
    s_init(&m);

    TEST_ASSERT_EQUAL_INT(0, s_lock(&m));
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xSemaphoreTake_call_count);
}

/* TLSA-T09 */
void test_mutex_lock_failure_is_reported(void)
{
    mbedtls_threading_mutex_t m = {0};

    tls_arena_init();
    s_init(&m);
    g_mock_xSemaphoreTake_return = pdFALSE;

    TEST_ASSERT_EQUAL_INT(MBEDTLS_ERR_THREADING_MUTEX_ERROR, s_lock(&m));
}

/* TLSA-T10 */
void test_mutex_unlock_gives_the_semaphore(void)
{
    mbedtls_threading_mutex_t m = {0};

    tls_arena_init();
    s_init(&m);

    TEST_ASSERT_EQUAL_INT(0, s_unlock(&m));
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xSemaphoreGive_call_count);
}

/* TLSA-T11 */
void test_mutex_unlock_failure_is_reported(void)
{
    mbedtls_threading_mutex_t m = {0};

    tls_arena_init();
    s_init(&m);
    g_mock_xSemaphoreGive_return = pdFALSE;

    TEST_ASSERT_EQUAL_INT(MBEDTLS_ERR_THREADING_MUTEX_ERROR, s_unlock(&m));
}

/* TLSA-T12: a mutex that was freed, or never initialised, refuses the lock. */
void test_freed_mutex_refuses_lock_and_unlock(void)
{
    mbedtls_threading_mutex_t m = {0};

    tls_arena_init();
    s_init(&m);
    s_free(&m);

    TEST_ASSERT_EQUAL_INT(MBEDTLS_ERR_THREADING_BAD_INPUT_DATA, s_lock(&m));
    TEST_ASSERT_EQUAL_INT(MBEDTLS_ERR_THREADING_BAD_INPUT_DATA, s_unlock(&m));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xSemaphoreTake_call_count);
}

/* TLSA-T13: NULL arguments are refused, not dereferenced. */
void test_null_mutex_is_refused(void)
{
    tls_arena_init();

    s_init(NULL);
    s_free(NULL);
    TEST_ASSERT_EQUAL_INT(MBEDTLS_ERR_THREADING_BAD_INPUT_DATA, s_lock(NULL));
    TEST_ASSERT_EQUAL_INT(MBEDTLS_ERR_THREADING_BAD_INPUT_DATA, s_unlock(NULL));
}

/* TLSA-T14: the peak is 0 before init, then whatever the allocator reports; reset forwards. */
void test_peak_is_zero_before_init_and_follows_the_allocator_after(void)
{
    s_fake_max_used = 4096U;
    TEST_ASSERT_EQUAL_UINT32(0U, tls_arena_peak_bytes());

    tls_arena_reset_peak();
    TEST_ASSERT_EQUAL_UINT32(0U, s_reset_calls);

    tls_arena_init();
    TEST_ASSERT_EQUAL_UINT32(4096U, tls_arena_peak_bytes());

    tls_arena_reset_peak();
    TEST_ASSERT_EQUAL_UINT32(1U, s_reset_calls);
}

/* TLSA-T15: before the arena exists mbedTLS's default allocator refuses, it never uses the heap. */
void test_default_allocator_refuses_every_request(void)
{
    TEST_ASSERT_NULL(tls_arena_uninit_calloc(1U, 16U));
    TEST_ASSERT_NULL(tls_arena_uninit_calloc(0U, 0U));
    tls_arena_uninit_free(NULL); /* must be harmless */
}
