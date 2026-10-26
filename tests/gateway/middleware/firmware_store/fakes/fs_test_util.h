/**
 * @file fs_test_util.h
 * @brief Shared helpers for the FirmwareStore unit tests.
 *
 * Seeds the flash fakes straight from firmware_image_format.h, independently of
 * the code under test, so a symmetric bug in FirmwareStore's reader and writer
 * cannot hide. Also provides the Logger spy, a world reset, and drivers over the
 * public API. It has no cryptography in it, so it links with either the fake or the
 * real crypto; image building with the fake lives in fs_test_image.h.
 *
 * A test that includes this header must also include, directly, every header
 * whose .c it needs linked: firmware_store.h, fs_metadata.h, fs_staging.h,
 * config_store_crc.h, and the fake_* headers it uses (Ceedling links by the
 * test file's own includes only).
 */

#ifndef FS_TEST_UTIL_H
#define FS_TEST_UTIL_H

#include <stdbool.h>
#include <stdint.h>

#include "firmware_image_format.h"
#include "firmware_store.h"

/** Largest staged file the helpers build. */
#define FSU_MAX_FILE (FIRMWARE_HEADER_SIZE + 12288U)

/** Body length of the image a "capable" world has installed in the running slot. */
#define FSU_RUNNING_BODY_LEN (3000U)

/** Version of that running image. */
#define FSU_RUNNING_VERSION (0x01000000UL)

/** The handle produced by fsu_create(). */
extern firmware_store_t *fsu_fs;

/* ---------------------------------------------------------------------- */
/* Logger spy                                                              */
/* ---------------------------------------------------------------------- */

/** @brief Messages logged at a level (0 = ERROR, 1 = WARN, 2 = INFO, 3 = DEBUG). */
uint32_t fsu_log_count(uint32_t level);

/** @brief True if any logged message contains the text. */
bool fsu_log_contains(const char *text);

/* ---------------------------------------------------------------------- */
/* World                                                                   */
/* ---------------------------------------------------------------------- */

/** @brief Reset the FreeRTOS mock, both flash fakes, the power cutter, the log and the module. */
void fsu_reset_world(void);

/** @brief Write a valid metadata record into copy 0 (A) or 1 (B), directly into the fake. */
void fsu_seed_meta(uint8_t copy, uint8_t active, uint8_t state, uint8_t previous,
                   uint32_t rollback_count, uint32_t generation);

/** @brief Write body bytes and a valid descriptor for a slot, directly into the fake. */
void fsu_seed_slot(firmware_slot_t slot, uint32_t body_len, uint32_t version);

/**
 * @brief A device that booted a confirmed image from Slot A.
 *
 * Metadata copy A: generation 1, CONFIRMED, active A, previous B; Slot A holds a
 * valid image. Slot B is blank.
 */
void fsu_seed_running_a(void);

/** @brief Create the module with the fakes and the given vector-table base. Asserts success. */
firmware_store_t *fsu_create(uint32_t running_image_base);

/** @brief Pattern byte used for seeded and built bodies. */
uint8_t fsu_pattern(uint32_t index, uint32_t seed);

/**
 * @brief Drive begin + write_chunk through the public API.
 *
 * @param chunk  Bytes per write_chunk call.
 * @return The first non-OK result, or OK.
 */
firmware_store_err_t fsu_download(const uint8_t *file, uint32_t len, uint32_t image_id,
                                  uint32_t chunk);

/** @brief Run install_begin and the install_step loop to completion. @return first non-OK result.
 */
firmware_store_err_t fsu_install_all(uint32_t *steps_out);

/** @brief Read the staged file back out of the QSPI fake. */
void fsu_read_staged(uint8_t *out, uint32_t len);

#endif /* FS_TEST_UTIL_H */
