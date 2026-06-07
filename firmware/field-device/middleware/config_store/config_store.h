/**
 * @file config_store.h
 * @brief ConfigStore — power-loss-safe A/B slot NOR flash configuration persistence.
 *
 * Provides IConfigStore. Persists one opaque config blob per write using an
 * A/B slot rotation with CRC32/ISO-HDLC integrity protection (D39).
 * Thread-safe (internal priority-inheritance mutex). Not ISR-safe.
 *
 * Boards: Field Device (STM32F469I-DISCO) and Gateway (B-L475E-IOT01A).
 *
 * @see docs/lld/middleware/config-store.md
 */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "config_store_config.h"

/* ========================================================================= */
/* IHealthReport forward declaration                                         */
/* ========================================================================= */

/* In production builds include health_monitor.h for the full ihealth_report_t
 * definition.  In test builds only a forward declaration is provided here;
 * the test TU includes health_monitor_stub.h to complete the struct body
 * before instantiating any ihealth_report_t.  This keeps health_monitor.c
 * and its LED/driver dependencies out of the test link unit. */
#ifndef TEST
#  include "health_monitor/health_monitor.h"
#else
#  ifndef IHEALTH_REPORT_T_DEFINED
#  define IHEALTH_REPORT_T_DEFINED
struct ihealth_report_s;
typedef struct ihealth_report_s ihealth_report_t;
#  endif /* IHEALTH_REPORT_T_DEFINED */
#endif /* TEST */

/* ========================================================================= */
/* Error codes                                                               */
/* ========================================================================= */

typedef enum
{
    CONFIG_STORE_ERR_OK            = 0, /**< Operation succeeded. */
    CONFIG_STORE_ERR_NOT_INIT      = 1, /**< Called before config_store_init(). */
    CONFIG_STORE_ERR_NULL_ARG      = 2, /**< Required pointer argument is NULL. */
    CONFIG_STORE_ERR_TOO_LARGE     = 3, /**< Blob exceeds CONFIG_STORE_MAX_DATA_BYTES. */
    CONFIG_STORE_ERR_NO_VALID_SLOT = 4, /**< Both flash slots have invalid CRC. */
    CONFIG_STORE_ERR_FLASH_ERASE   = 5, /**< Erase failure from QspiFlashDriver. */
    CONFIG_STORE_ERR_FLASH_WRITE   = 6, /**< Program failure from QspiFlashDriver. */
    CONFIG_STORE_ERR_FLASH_READ    = 7, /**< Read failure from QspiFlashDriver. */
} config_store_err_t;

/* ========================================================================= */
/* Constants                                                                 */
/* ========================================================================= */

/** Maximum config blob in bytes (32 KB slot − 16-byte header − 4-byte CRC − margin). */
#define CONFIG_STORE_MAX_DATA_BYTES  32712U

/** Magic number written at the start of every valid slot header. */
#define CONFIG_STORE_MAGIC           0xC0FFEE00UL

/* ========================================================================= */
/* IConfigStore vtable (P2 — Dependency Inversion)                          */
/* ========================================================================= */

/**
 * @brief IConfigStore vtable.
 *
 * ConfigService (and other callers) depend on this interface; the concrete
 * driver is injected as a const pointer to the static singleton below.
 * Layout must remain binary-compatible with config_store_stub.h.
 */
typedef struct
{
    config_store_err_t (*init)(ihealth_report_t *health);
    config_store_err_t (*load)(void *data_out, uint32_t *len_out, uint32_t max_len);
    config_store_err_t (*save)(const void *data, uint32_t len);
    config_store_err_t (*check_integrity)(void);
    config_store_err_t (*erase)(void);
} iconfig_store_t;

/** Singleton pointer to the ConfigStore vtable instance. */
extern const iconfig_store_t *const config_store;

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

/**
 * @brief Initialise ConfigStore.
 *
 * Must be called after qspi_flash_driver_init().  Verifies the QSPI partition
 * is accessible.  Does NOT load config — load is a separate call.
 *
 * @param  health  IHealthReport handle for failure event push.
 * @return CONFIG_STORE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking.  Call before scheduler starts.
 */
config_store_err_t config_store_init(ihealth_report_t *health);

/**
 * @brief Load the most recent valid config blob from flash.
 *
 * Reads both slots.  Selects the slot with the highest seq_number whose
 * CRC32 is valid.  Copies the blob into the caller's buffer.
 *
 * @param[out] data_out  Caller-supplied buffer; receives config blob.
 * @param[out] len_out   Set to the blob byte count on success.
 * @param[in]  max_len   Size of data_out in bytes.
 * @return CONFIG_STORE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking.  Not ISR-safe.
 */
config_store_err_t config_store_load(void     *data_out,
                                     uint32_t *len_out,
                                     uint32_t  max_len);

/**
 * @brief Persist a new config blob.
 *
 * Executes the power-loss-safe A/B write protocol.  On any flash failure,
 * pushes HEALTH_EVENT_CONFIG_WRITE_FAIL and returns the appropriate error.
 * The previous slot remains valid on failure.
 *
 * Thread-safe (acquires internal mutex).
 *
 * @param[in]  data  Opaque config blob.
 * @param[in]  len   Byte count; must be <= CONFIG_STORE_MAX_DATA_BYTES.
 * @return CONFIG_STORE_ERR_OK on success; non-zero error code on failure.
 */
config_store_err_t config_store_save(const void *data, uint32_t len);

/**
 * @brief Verify flash integrity without loading.
 *
 * Returns OK if at least one slot has a valid CRC32.  Does not modify state.
 * Pushes HEALTH_EVENT_CONFIG_NO_VALID_SLOT if both slots are invalid.
 *
 * @return CONFIG_STORE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking.  Not ISR-safe.
 */
config_store_err_t config_store_check_integrity(void);

/**
 * @brief Erase both slots (factory reset).
 *
 * Erases all 16 sectors.  After this call the next config_store_load() returns
 * CONFIG_STORE_ERR_NO_VALID_SLOT — the caller must supply defaults.
 *
 * @return CONFIG_STORE_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, may block.  Not ISR-safe.
 */
config_store_err_t config_store_erase(void);

/* ========================================================================= */
/* UNIT_TEST: RAM-backed flash simulation                                    */
/* ========================================================================= */

#ifdef UNIT_TEST

/* Minimal QSPI error type — avoids including qspi_flash_driver.h in test builds. */
typedef int qspi_flash_err_t;
#define QSPI_FLASH_OK 0

/** 64 KB RAM flash simulation buffer (defined in config_store.c under UNIT_TEST). */
extern uint8_t g_config_store_flash_sim[];

/* Stub function declarations — definitions provided by the test TU. */
qspi_flash_err_t stub_cs_flash_erase_range(uint8_t *flash, uint32_t base, uint32_t sectors);
qspi_flash_err_t stub_cs_flash_write(uint8_t *flash, uint32_t addr,
                                     const uint8_t *buf, uint32_t len);
qspi_flash_err_t stub_cs_flash_read(uint8_t *flash, uint32_t addr,
                                    uint8_t *buf, uint32_t len);

/* Redirect internal wrapper names to RAM-backed stubs. */
#define cs_flash_erase_range(base, sectors)  \
    stub_cs_flash_erase_range(g_config_store_flash_sim, (base), (sectors))
#define cs_flash_write_bytes(addr, buf, len) \
    stub_cs_flash_write(g_config_store_flash_sim, (addr), (buf), (len))
#define cs_flash_read_bytes(addr, buf, len)  \
    stub_cs_flash_read(g_config_store_flash_sim, (addr), (buf), (len))

#endif /* UNIT_TEST */

/* ========================================================================= */
/* Test-only hooks                                                           */
/* ========================================================================= */

#ifdef TEST
#define CONFIG_STORE_TEST_VISIBLE
/** Reset internal state to post-BSS defaults.  Call from setUp(). */
void config_store_reset_for_test(void);
#else
#define CONFIG_STORE_TEST_VISIBLE static
#endif /* TEST */

#endif /* CONFIG_STORE_H */
