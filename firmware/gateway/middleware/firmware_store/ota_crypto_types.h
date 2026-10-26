/**
 * @file ota_crypto_types.h
 * @brief Types and prototypes of the OTA crypto seam (SHA-256 and ECDSA-P256 verify).
 *
 * Deliberately separate from ota_crypto.h: this header has no matching .c, so a
 * unit test can include it, define the functions itself and keep the real
 * mbedTLS-backed ota_crypto.c out of the link (the Ceedling auto-link rule).
 * No mbedTLS type appears here.
 *
 * @see docs/lld/middleware/firmware-store.md §11, §21
 */

#ifndef OTA_CRYPTO_TYPES_H
#define OTA_CRYPTO_TYPES_H

#include <stdint.h>

/** Length of a SHA-256 digest. */
#define OTA_SHA256_LEN (32U)

/** Length of the raw ECDSA-P256 signature, r || s. */
#define OTA_SIGNATURE_LEN (64U)

/** Length of the raw P-256 public key, X || Y. */
#define OTA_PUBLIC_KEY_LEN (64U)

/** Words reserved for a SHA-256 context; ota_crypto.c asserts it is large enough. */
#define OTA_SHA256_CTX_WORDS (32U)

/** @brief SHA-256 streaming context; opaque to callers. */
typedef struct
{
    uint32_t opaque[OTA_SHA256_CTX_WORDS];
} ota_sha256_ctx_t;

/** @brief Result of a crypto operation. */
typedef enum
{
    OTA_CRYPTO_OK = 0,
    OTA_CRYPTO_ERR_SIG_INVALID = 1, /**< The signature does not verify: tampering.      */
    OTA_CRYPTO_ERR_FAILED = 2 /**< Library or resource failure; says nothing about the image. */
} ota_crypto_err_t;

/** @brief Start a SHA-256 computation. */
ota_crypto_err_t ota_sha256_begin(ota_sha256_ctx_t *ctx);

/** @brief Feed bytes into a SHA-256 computation. */
ota_crypto_err_t ota_sha256_update(ota_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);

/** @brief Finish a SHA-256 computation and write the 32-byte digest. */
ota_crypto_err_t ota_sha256_finish(ota_sha256_ctx_t *ctx, uint8_t digest[OTA_SHA256_LEN]);

/**
 * @brief Verify an ECDSA-P256 signature over a SHA-256 digest with the compiled-in key.
 *
 * @param[in] digest     SHA-256 of the signed bytes.
 * @param[in] signature  Raw r || s, big-endian, 32 + 32 bytes.
 * @return OTA_CRYPTO_OK; OTA_CRYPTO_ERR_SIG_INVALID if the signature is wrong;
 *         OTA_CRYPTO_ERR_FAILED on a library failure or an unusable key.
 */
ota_crypto_err_t ota_ecdsa_p256_verify(const uint8_t digest[OTA_SHA256_LEN],
                                       const uint8_t signature[OTA_SIGNATURE_LEN]);

#endif /* OTA_CRYPTO_TYPES_H */
