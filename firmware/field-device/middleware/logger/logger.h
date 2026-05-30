#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>   /* snprintf — used inside the LOG_* macros */

/* ====================================================================== */
/* Compile-time configuration                                             */
/* ====================================================================== */

/* Numeric level constants — preprocessor-visible so they can be used in
 * #if expressions for compile-time level filtering. Lower value = higher
 * severity. */
#define LOG_LVL_ERROR  (0)
#define LOG_LVL_WARN   (1)
#define LOG_LVL_INFO   (2)
#define LOG_LVL_DEBUG  (3)

/* Compile-time minimum severity. Levels less severe than this are stripped
 * at compile time — no function call, no argument evaluation, no local
 * buffer. Override per build with -DLOG_LEVEL_MIN=LOG_LVL_xxx. */
#ifndef LOG_LEVEL_MIN
#define LOG_LEVEL_MIN  LOG_LVL_DEBUG
#endif

/* ANSI colour codes around the level tag. Set to 0 for plain text. */
#ifndef LOGGER_USE_ANSI_COLORS
#define LOGGER_USE_ANSI_COLORS  (1)
#endif

/* Caller-side scratch buffer for printf substitution in the macros. The
 * substituted message is copied (truncated) into the queue entry. */
#define LOGGER_MESSAGE_MAX   (64U)
#define LOGGER_MODULE_WIDTH  (16U)

/* ====================================================================== */
/* Types                                                                  */
/* ====================================================================== */

typedef enum {
    LOG_LEVEL_ERROR = LOG_LVL_ERROR,
    LOG_LEVEL_WARN  = LOG_LVL_WARN,
    LOG_LEVEL_INFO  = LOG_LVL_INFO,
    LOG_LEVEL_DEBUG = LOG_LVL_DEBUG
} log_level_t;

typedef enum {
    LOGGER_OK              = 0,
    LOGGER_ERR_NOT_INIT    = 1,
    LOGGER_ERR_INVALID_ARG = 2
} logger_err_t;

/* ====================================================================== */
/* Public API                                                             */
/* ====================================================================== */

/** @see logger.md §2.1 */
logger_err_t logger_init(log_level_t initial_level);

/** @see logger.md §2.1 */
logger_err_t logger_set_level(log_level_t level);

/** @see logger.md §2.1 — plain message string, no varargs. Use the macros. */
void logger_log(log_level_t level, const char *module, const char *msg);

/** @see logger.md §2.1 — ISR-safe entry; uptime timestamp; no formatting. */
void logger_log_from_isr(log_level_t level, const char *module, const char *msg);

/** @see logger.md §2.1 — number of lines dropped due to a full queue. */
uint32_t logger_get_dropped_count(void);

/* ====================================================================== */
/* Task-context macros — run snprintf locally, then call logger_log()      */
/* ====================================================================== */

#if (LOG_LVL_ERROR <= LOG_LEVEL_MIN)
#define LOG_ERROR(mod, fmt, ...) do {                                       \
    char _logger_msg[LOGGER_MESSAGE_MAX];                                   \
    (void)snprintf(_logger_msg, sizeof(_logger_msg), (fmt), ##__VA_ARGS__); \
    logger_log(LOG_LEVEL_ERROR, (mod), _logger_msg);                        \
} while (0)
#else
#define LOG_ERROR(mod, fmt, ...)  ((void)0)
#endif

#if (LOG_LVL_WARN <= LOG_LEVEL_MIN)
#define LOG_WARN(mod, fmt, ...) do {                                        \
    char _logger_msg[LOGGER_MESSAGE_MAX];                                   \
    (void)snprintf(_logger_msg, sizeof(_logger_msg), (fmt), ##__VA_ARGS__); \
    logger_log(LOG_LEVEL_WARN, (mod), _logger_msg);                         \
} while (0)
#else
#define LOG_WARN(mod, fmt, ...)   ((void)0)
#endif

#if (LOG_LVL_INFO <= LOG_LEVEL_MIN)
#define LOG_INFO(mod, fmt, ...) do {                                        \
    char _logger_msg[LOGGER_MESSAGE_MAX];                                   \
    (void)snprintf(_logger_msg, sizeof(_logger_msg), (fmt), ##__VA_ARGS__); \
    logger_log(LOG_LEVEL_INFO, (mod), _logger_msg);                         \
} while (0)
#else
#define LOG_INFO(mod, fmt, ...)   ((void)0)
#endif

#if (LOG_LVL_DEBUG <= LOG_LEVEL_MIN)
#define LOG_DEBUG(mod, fmt, ...) do {                                       \
    char _logger_msg[LOGGER_MESSAGE_MAX];                                   \
    (void)snprintf(_logger_msg, sizeof(_logger_msg), (fmt), ##__VA_ARGS__); \
    logger_log(LOG_LEVEL_DEBUG, (mod), _logger_msg);                        \
} while (0)
#else
#define LOG_DEBUG(mod, fmt, ...)  ((void)0)
#endif

/* ====================================================================== */
/* ISR-context macros — plain string, no printf substitution               */
/* ====================================================================== */

#if (LOG_LVL_ERROR <= LOG_LEVEL_MIN)
#define LOG_ERROR_ISR(mod, msg)  logger_log_from_isr(LOG_LEVEL_ERROR, (mod), (msg))
#else
#define LOG_ERROR_ISR(mod, msg)  ((void)0)
#endif

#if (LOG_LVL_WARN <= LOG_LEVEL_MIN)
#define LOG_WARN_ISR(mod, msg)   logger_log_from_isr(LOG_LEVEL_WARN, (mod), (msg))
#else
#define LOG_WARN_ISR(mod, msg)   ((void)0)
#endif

/* ====================================================================== */
/* Test-only exposures (#ifdef TEST)                                       */
/* ====================================================================== */

#ifdef TEST

/**
 * @brief White-box queue-entry layout, exposed so test code can inspect
 *        what producers put on the queue. Identical to the private
 *        definition in logger.c — keep in sync.
 */
typedef struct {
    log_level_t level;
    bool        use_uptime;
    union {
        struct {
            uint8_t hour;
            uint8_t minute;
            uint8_t second;
        }        wallclock;
        uint32_t uptime_ms;
    } ts;
    char module[LOGGER_MODULE_WIDTH + 1U];
    char message[LOGGER_MESSAGE_MAX];
} log_entry_t;

/** Reset internal state and zero the static FreeRTOS buffers. */
void logger_reset_for_test(void);

/** Run one iteration of the drain task body (dequeue → format → send). */
void logger_drain_once_for_test(void);

#endif /* TEST */

#endif /* LOGGER_H */
