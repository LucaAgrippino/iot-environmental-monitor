/**
 * @file fake_fs_ops.h
 * @brief Scriptable fakes of the two FirmwareStore vtables (UpdateService tests).
 *
 * UpdateService is tested against the interfaces, not the real module: FirmwareStore's contract
 * is proven by its own suites. Each function returns its scripted codes in call order (OK once
 * the script is exhausted), counts its calls, and appends to a shared call log so that tests
 * can assert ordering.
 *
 * Including this header makes Ceedling auto-link fake_fs_ops.c.
 */

#ifndef FAKE_FS_OPS_H
#define FAKE_FS_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "firmware_store/ifirmware_boot_control.h"
#include "firmware_store/ifirmware_image_store.h"

/** @brief The scriptable functions, and the entries of the call log. */
typedef enum
{
    FFS_BEGIN = 0,
    FFS_RESUME,
    FFS_WRITE_CHUNK,
    FFS_VERIFY,
    FFS_INSTALL_BEGIN,
    FFS_INSTALL_STEP,
    FFS_ACTIVATE,
    FFS_DISCARD,
    FFS_GET_BOOT_INFO,
    FFS_CONFIRM,
    FFS_ACK_ROLLBACK,
    FFS_COUNT
} fake_fs_fn_t;

#define FFS_SCRIPT_MAX (8U)
#define FFS_LOG_MAX (64U)
#define FFS_WRITE_KEEP (4096U)

extern const ifirmware_image_store_t fake_fs_image_ops;
extern const ifirmware_boot_control_t fake_fs_boot_ops;

/** @brief Clear scripts, counters, the log, and set a default CONFIRMED Slot-A boot info. */
void fake_fs_reset(void);

/** @brief The next n calls of fn return codes[0..n-1]; later calls return OK. */
void fake_fs_script(fake_fs_fn_t fn, const firmware_store_err_t *codes, uint32_t n);

/** @brief Every call of fn returns code (after its script is exhausted). */
void fake_fs_always(fake_fs_fn_t fn, firmware_store_err_t code);

uint32_t fake_fs_calls(fake_fs_fn_t fn);
uint32_t fake_fs_log_count(void);
fake_fs_fn_t fake_fs_log_at(uint32_t index);

/** @brief Offset that resume() reports (default 0). */
void fake_fs_set_resume_offset(uint32_t offset);

/** @brief What verify() fills in. */
void fake_fs_set_verify_info(uint32_t version, uint32_t len, firmware_slot_t slot);

/** @brief install_step() sets *done after this many calls of an install (default 3). */
void fake_fs_set_install_steps(uint32_t steps);

/** @brief What get_boot_info() reports. */
void fake_fs_set_boot_info(const firmware_boot_info_t *info);

/** @brief Offset and length of the last write_chunk(), and a copy of its bytes (first 4 KB). */
uint32_t fake_fs_last_write_offset(void);
uint32_t fake_fs_last_write_len(void);
const uint8_t *fake_fs_last_write_data(void);

/** @brief Arguments of the last begin() / resume(). */
uint32_t fake_fs_last_job(void);
uint32_t fake_fs_last_file_len(void);

#endif /* FAKE_FS_OPS_H */
