/**
 * @file fake_ota_crypto.h
 * @brief Deterministic stand-in for the OTA crypto seam (FirmwareStore tests).
 *
 * Defines ota_sha256_* and ota_ecdsa_p256_verify() so that firmware_store.c links
 * against it instead of the mbedTLS-backed ota_crypto.c. The "hash" is a
 * 256-bit FNV-style mix that changes with every input byte; a "signature" is the
 * digest followed by its bitwise complement. Neither is secure: the real
 * primitives are exercised in test_fs_crypto_vectors.c.
 *
 * Including this header makes Ceedling auto-link fake_ota_crypto.c.
 */

#ifndef FAKE_OTA_CRYPTO_H
#define FAKE_OTA_CRYPTO_H

#include <stdint.h>

#include "ota_crypto_types.h"

/** @brief Behaviour of the next ota_ecdsa_p256_verify() calls. */
typedef enum
{
    FAKE_VERIFY_NORMAL = 0,    /**< Valid iff the signature matches the digest.   */
    FAKE_VERIFY_FORCE_INVALID, /**< Always OTA_CRYPTO_ERR_SIG_INVALID.            */
    FAKE_VERIFY_FORCE_FAILED   /**< Always OTA_CRYPTO_ERR_FAILED (library fault).  */
} fake_verify_mode_t;

/** @brief Back to NORMAL, counters to zero. */
void fake_crypto_reset(void);

void fake_crypto_set_verify_mode(fake_verify_mode_t mode);

/** @brief Compute the fake digest of a buffer. */
void fake_crypto_digest(const uint8_t *data, uint32_t len, uint8_t digest[OTA_SHA256_LEN]);

/** @brief Build the "signature" that the fake accepts for a digest. */
void fake_crypto_sign(const uint8_t digest[OTA_SHA256_LEN], uint8_t signature[OTA_SIGNATURE_LEN]);

uint32_t fake_crypto_sha_begin_calls(void);
uint32_t fake_crypto_verify_calls(void);

#endif /* FAKE_OTA_CRYPTO_H */
