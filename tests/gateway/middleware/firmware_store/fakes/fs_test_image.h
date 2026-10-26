/**
 * @file fs_test_image.h
 * @brief Signed-image builder for the FirmwareStore unit tests that use the fake crypto.
 *
 * Kept apart from fs_test_util.h because it calls fake_ota_crypto.c, which defines
 * the same symbols as the real ota_crypto.c: a test that links the real crypto
 * (test_fs_crypto_vectors.c) must not include this header.
 *
 * Including this header makes Ceedling auto-link fs_test_image.c.
 */

#ifndef FS_TEST_IMAGE_H
#define FS_TEST_IMAGE_H

#include <stdint.h>

#include "fs_test_util.h"

/**
 * @brief Build a signed staged file (header + body) with the fake crypto.
 *
 * The body's reset vector is load_addr + 0x101 (Thumb bit set, inside the image);
 * for a body shorter than 0x102 bytes it is the last even offset, plus the Thumb bit.
 *
 * @return File length in bytes (128 + body_len).
 */
uint32_t fsu_build_image(uint8_t *out, uint32_t body_len, uint32_t load_addr, uint32_t version);

/** @brief Re-sign the header of a built image after a test altered a header field. */
void fsu_resign_header(uint8_t *file);

/** @brief Recompute body_sha256 from the body, then re-sign: use after altering the body. */
void fsu_refresh_header(uint8_t *file);

#endif /* FS_TEST_IMAGE_H */
