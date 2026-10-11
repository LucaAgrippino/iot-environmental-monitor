/**
 * @file config_store.h
 * @brief ConfigStore — power-loss-safe persistence of one config blob (Gateway).
 *
 * Stores the system configuration as a single opaque byte array over two 32 KB
 * A/B slots in QSPI flash, protected by a CRC32 that is written last (D39). The
 * key-value structure is ConfigService's concern; ConfigStore only moves bytes.
 *
 * Failures are reported upward to HealthMonitor: a failed erase or program as
 * HEALTH_EVENT_CONFIG_WRITE_FAIL, and a missing or unreadable configuration as
 * HEALTH_EVENT_FAULT with one of the FAULT_CONFIG_* codes below.
 *
 * Called from task context only; a priority-inheritance mutex serialises
 * callers. The module is passive and has no task of its own.
 *
 * @see docs/lld/middleware/config-store-lld.md
 */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "health_monitor/health_monitor.h"
#include "qspi_flash/qspi_flash.h"

/** @brief ConfigStore error codes. Codes 0..3 follow the project-wide ADT convention. */
typedef enum
{
    CONFIG_STORE_ERR_OK = 0,            /**< Success.                        */
    CONFIG_STORE_ERR_NULL_PTR = 1,      /**< Null pointer argument.          */
    CONFIG_STORE_ERR_NO_RESOURCE = 2,   /**< Static pool exhausted.          */
    CONFIG_STORE_ERR_NOT_INIT = 3,      /**< Handle not created or invalid.  */
    CONFIG_STORE_ERR_TOO_LARGE = 4,     /**< Blob exceeds the capacity given. */
    CONFIG_STORE_ERR_NO_VALID_SLOT = 5, /**< Neither slot holds a valid blob. */
    CONFIG_STORE_ERR_FLASH_ERASE = 6,   /**< Erase failure from the driver.  */
    CONFIG_STORE_ERR_FLASH_WRITE = 7,   /**< Program failure from the driver. */
    CONFIG_STORE_ERR_FLASH_READ = 8     /**< Read failure from the driver.   */
} config_store_err_t;

/** Largest config blob accepted: the 32 744-byte data area less a 32-byte margin. */
#define CONFIG_STORE_MAX_DATA_BYTES (32712U)

/** Magic word identifying a written slot. */
#define CONFIG_STORE_MAGIC (0xC0FFEE00UL)

/** HEALTH_EVENT_FAULT param: neither slot holds a valid configuration. */
#define FAULT_CONFIG_NO_VALID_SLOT (0x0100U)

/** HEALTH_EVENT_FAULT param: a flash read failed during load or integrity check. */
#define FAULT_CONFIG_READ_FAIL (0x0101U)

/** @brief Opaque handle — every public function takes it first. */
typedef struct config_store_inst *config_store_handle_t;

/** @brief Dependencies injected at creation. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember */
    const iqspi_flash_t *flash; /**< QspiFlashDriver vtable.                          */
    /* cppcheck-suppress unusedStructMember */
    health_monitor_handle_t health; /**< HealthMonitor handle; NULL disables events.  */
} config_store_config_t;

/**
 * @brief Create a ConfigStore instance.
 *
 * Takes the single static instance, stores the dependency handles and creates
 * the mutex. Does not touch flash: call config_store_check_integrity() and/or
 * config_store_load() afterwards.
 *
 * @param[in]  config      Dependencies; config->flash must not be NULL.
 * @param[out] handle_out  Receives the handle on success.
 * @return CONFIG_STORE_ERR_OK; CONFIG_STORE_ERR_NULL_PTR if config, handle_out
 *         or config->flash is NULL; CONFIG_STORE_ERR_NO_RESOURCE if already
 *         created or the mutex cannot be created.
 * @note Threading: call once before the scheduler starts.
 */
config_store_err_t config_store_create(const config_store_config_t *config,
                                       config_store_handle_t *handle_out);

/**
 * @brief Load the newest valid config blob.
 *
 * Checks both slots and copies the blob from the valid one with the higher
 * seq_number. If neither slot is valid it pushes HEALTH_EVENT_FAULT with
 * FAULT_CONFIG_NO_VALID_SLOT, or FAULT_CONFIG_READ_FAIL when a flash read error
 * is why neither could be checked.
 *
 * @param[in]  self      ConfigStore handle.
 * @param[out] data_out  Buffer that receives the blob.
 * @param[out] len_out   Receives the blob length on success.
 * @param[in]  max_len   Size of data_out in bytes.
 * @return CONFIG_STORE_ERR_OK; CONFIG_STORE_ERR_NULL_PTR; CONFIG_STORE_ERR_NOT_INIT;
 *         CONFIG_STORE_ERR_NO_VALID_SLOT; CONFIG_STORE_ERR_TOO_LARGE if the
 *         blob is longer than max_len; CONFIG_STORE_ERR_FLASH_READ.
 * @note Threading: task-context only; takes the internal mutex.
 */
config_store_err_t config_store_load(config_store_handle_t self, void *data_out, uint32_t *len_out,
                                     uint32_t max_len);

/**
 * @brief Check that at least one slot holds a valid blob, without loading it.
 *
 * Called during Init.CheckingIntegrity (REQ-NF-214). Raises the same health
 * faults as config_store_load() when it finds nothing valid. Does not change
 * the cached state.
 *
 * @param[in] self  ConfigStore handle.
 * @return CONFIG_STORE_ERR_OK; CONFIG_STORE_ERR_NULL_PTR; CONFIG_STORE_ERR_NOT_INIT;
 *         CONFIG_STORE_ERR_NO_VALID_SLOT; CONFIG_STORE_ERR_FLASH_READ.
 * @note Threading: task-context only; takes the internal mutex.
 */
config_store_err_t config_store_check_integrity(config_store_handle_t self);

/**
 * @brief Persist a new config blob through the A/B write protocol.
 *
 * Erases the slot that is not active, writes header, data and finally the CRC32
 * (the commit point). On any flash failure it pushes
 * HEALTH_EVENT_CONFIG_WRITE_FAIL and the previous slot stays valid.
 *
 * @param[in] self  ConfigStore handle.
 * @param[in] data  Opaque blob.
 * @param[in] len   Byte count, at most CONFIG_STORE_MAX_DATA_BYTES.
 * @return CONFIG_STORE_ERR_OK; CONFIG_STORE_ERR_NULL_PTR; CONFIG_STORE_ERR_NOT_INIT;
 *         CONFIG_STORE_ERR_TOO_LARGE; CONFIG_STORE_ERR_FLASH_ERASE;
 *         CONFIG_STORE_ERR_FLASH_WRITE.
 * @note Threading: task-context only; takes the internal mutex and blocks for
 *       the erase and program time (tens of milliseconds per sector).
 */
config_store_err_t config_store_save(config_store_handle_t self, const void *data, uint32_t len);

/**
 * @brief Erase both slots (factory reset).
 *
 * After this call config_store_load() returns CONFIG_STORE_ERR_NO_VALID_SLOT and
 * the caller must supply defaults. A failure pushes
 * HEALTH_EVENT_CONFIG_WRITE_FAIL.
 *
 * @param[in] self  ConfigStore handle.
 * @return CONFIG_STORE_ERR_OK; CONFIG_STORE_ERR_NULL_PTR; CONFIG_STORE_ERR_NOT_INIT;
 *         CONFIG_STORE_ERR_FLASH_ERASE.
 * @note Threading: task-context only; blocks for sixteen sector erases.
 */
config_store_err_t config_store_erase(config_store_handle_t self);

#ifdef TEST
/** @brief Test hook — release the instance and return the pool to its post-bss state. */
void config_store_reset_for_test(void);
#endif

#endif /* CONFIG_STORE_H */
