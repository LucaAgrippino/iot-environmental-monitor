/**
 * @file config_store_stub.h
 * @brief Narrow stub for IConfigStore used in config_service unit tests.
 *
 * Replaces #include "config_store/config_store.h" in the test build so that
 * config_store.c (and its QspiFlashDriver cascade) is NOT auto-linked.
 *
 * The struct layout of iconfig_store_t MUST match the production definition
 * in config_store.h: same number of function-pointer fields, same order,
 * same binary size.  The init field uses void * for the health parameter
 * because ihealth_report_t is not needed here; all function pointers are the
 * same width regardless of parameter types.
 *
 * Basename: config_store_stub — does NOT match config_store.c.
 */

#ifndef CONFIG_STORE_STUB_H
#define CONFIG_STORE_STUB_H

#include <stdint.h>

/* ========================================================================= */
/* Constants (must match config_store.h)                                    */
/* ========================================================================= */

#define CONFIG_STORE_MAX_DATA_BYTES 32712U /**< Max config blob size. */

/* ========================================================================= */
/* Error codes (must match config_store.h enum values)                      */
/* ========================================================================= */

typedef enum
{
    CONFIG_STORE_OK               = 0,
    CONFIG_STORE_ERR_NOT_INIT     = 1,
    CONFIG_STORE_ERR_NULL_ARG     = 2,
    CONFIG_STORE_ERR_TOO_LARGE    = 3,
    CONFIG_STORE_ERR_NO_VALID_SLOT = 4,
    CONFIG_STORE_ERR_FLASH_ERASE  = 5,
    CONFIG_STORE_ERR_FLASH_WRITE  = 6,
    CONFIG_STORE_ERR_FLASH_READ   = 7,
} config_store_err_t;

/* ========================================================================= */
/* iconfig_store_t vtable (layout must match config_store.h)               */
/* ========================================================================= */

typedef struct
{
    config_store_err_t (*init)(const void *health);   /* void* avoids pulling in ihealth_report_t */
    config_store_err_t (*load)(void *data_out, uint32_t *len_out, uint32_t max_len);
    config_store_err_t (*save)(const void *data, uint32_t len);
    config_store_err_t (*check_integrity)(void);
    config_store_err_t (*erase)(void);
} iconfig_store_t;

#endif /* CONFIG_STORE_STUB_H */
