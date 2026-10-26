/**
 * @file ota_crypto.c
 * @brief OTA crypto seam over mbedTLS: SHA-256 and ECDSA-P256 verify.
 *
 * @see docs/lld/middleware/firmware-store.md §11
 */

#include "ota_crypto.h"

#include <stddef.h>
#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

/** Bytes of one coordinate or one signature half. */
#define OTA_COORD_LEN (32U)

/** Uncompressed-point tag. */
#define OTA_POINT_UNCOMPRESSED (0x04U)

/** Length of an uncompressed P-256 point: tag + X + Y. */
#define OTA_POINT_LEN (1U + (2U * OTA_COORD_LEN))

_Static_assert(sizeof(mbedtls_sha256_context) <= sizeof(ota_sha256_ctx_t),
               "ota_sha256_ctx_t is too small for the mbedTLS SHA-256 context");
_Static_assert(OTA_PUBLIC_KEY_LEN == (2U * OTA_COORD_LEN), "public key is X || Y");
_Static_assert(OTA_SIGNATURE_LEN == (2U * OTA_COORD_LEN), "signature is r || s");

static mbedtls_sha256_context *sha_ctx(ota_sha256_ctx_t *ctx)
{
    void *raw = ctx;

    return (mbedtls_sha256_context *) raw;
}

ota_crypto_err_t ota_sha256_begin(ota_sha256_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return OTA_CRYPTO_ERR_FAILED;
    }

    (void) memset(ctx, 0, sizeof(*ctx));
    mbedtls_sha256_init(sha_ctx(ctx));

    return (mbedtls_sha256_starts(sha_ctx(ctx), 0) == 0) ? OTA_CRYPTO_OK : OTA_CRYPTO_ERR_FAILED;
}

ota_crypto_err_t ota_sha256_update(ota_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    if ((ctx == NULL) || ((data == NULL) && (len != 0U)))
    {
        return OTA_CRYPTO_ERR_FAILED;
    }

    return (mbedtls_sha256_update(sha_ctx(ctx), data, len) == 0) ? OTA_CRYPTO_OK
                                                                 : OTA_CRYPTO_ERR_FAILED;
}

ota_crypto_err_t ota_sha256_finish(ota_sha256_ctx_t *ctx, uint8_t digest[OTA_SHA256_LEN])
{
    if ((ctx == NULL) || (digest == NULL))
    {
        return OTA_CRYPTO_ERR_FAILED;
    }

    int ret = mbedtls_sha256_finish(sha_ctx(ctx), digest);

    mbedtls_sha256_free(sha_ctx(ctx));

    return (ret == 0) ? OTA_CRYPTO_OK : OTA_CRYPTO_ERR_FAILED;
}

ota_crypto_err_t ota_ecdsa_p256_verify(const uint8_t digest[OTA_SHA256_LEN],
                                       const uint8_t signature[OTA_SIGNATURE_LEN])
{
    if ((digest == NULL) || (signature == NULL))
    {
        return OTA_CRYPTO_ERR_FAILED;
    }

    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r;
    mbedtls_mpi s;
    uint8_t point[OTA_POINT_LEN];
    ota_crypto_err_t result = OTA_CRYPTO_ERR_FAILED;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    point[0] = (uint8_t) OTA_POINT_UNCOMPRESSED;
    (void) memcpy(&point[1], ota_public_key, OTA_PUBLIC_KEY_LEN);

    /* A key that is not a point on the curve (including the all-zero placeholder)
     * fails here: the module fails closed, as a resource fault, not as forgery. */
    if ((mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0) &&
        (mbedtls_ecp_point_read_binary(&grp, &q, point, sizeof(point)) == 0) &&
        (mbedtls_ecp_check_pubkey(&grp, &q) == 0) &&
        (mbedtls_mpi_read_binary(&r, signature, OTA_COORD_LEN) == 0) &&
        (mbedtls_mpi_read_binary(&s, &signature[OTA_COORD_LEN], OTA_COORD_LEN) == 0))
    {
        int ret = mbedtls_ecdsa_verify(&grp, digest, OTA_SHA256_LEN, &q, &r, &s);

        if (ret == 0)
        {
            result = OTA_CRYPTO_OK;
        }
        else if (ret == MBEDTLS_ERR_ECP_VERIFY_FAILED)
        {
            result = OTA_CRYPTO_ERR_SIG_INVALID;
        }
        else
        {
            result = OTA_CRYPTO_ERR_FAILED;
        }
    }

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);

    return result;
}
