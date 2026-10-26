/**
 * @file fake_ota_crypto.c
 * @brief Deterministic stand-in for the OTA crypto seam (FirmwareStore tests).
 */

#include "fake_ota_crypto.h"

#include <string.h>

#define LANES (4U)
#define FNV_OFFSET (0xCBF29CE484222325ULL)
#define FNV_PRIME (0x00000100000001B3ULL)
#define LANE_SALT (0x9E3779B97F4A7C15ULL)

typedef struct
{
    uint64_t lane[LANES];
    uint64_t total;
} fake_sha_state_t;

_Static_assert(sizeof(fake_sha_state_t) <= sizeof(ota_sha256_ctx_t), "fake state fits the context");

static fake_verify_mode_t s_mode;
static uint32_t s_sha_begins;
static uint32_t s_verifies;

static fake_sha_state_t *state_of(ota_sha256_ctx_t *ctx)
{
    void *raw = ctx;

    return (fake_sha_state_t *) raw;
}

static void mix(fake_sha_state_t *st, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        for (uint32_t l = 0U; l < LANES; l++)
        {
            st->lane[l] ^= (uint64_t) data[i] + (st->total * (l + 1U));
            st->lane[l] *= FNV_PRIME;
        }

        st->total++;
    }
}

static void begin(fake_sha_state_t *st)
{
    for (uint32_t l = 0U; l < LANES; l++)
    {
        st->lane[l] = FNV_OFFSET + (LANE_SALT * l);
    }

    st->total = 0U;
}

static void finish(const fake_sha_state_t *st, uint8_t digest[OTA_SHA256_LEN])
{
    for (uint32_t l = 0U; l < LANES; l++)
    {
        const uint64_t v = st->lane[l] ^ st->total;

        (void) memcpy(&digest[l * sizeof(v)], &v, sizeof(v));
    }
}

ota_crypto_err_t ota_sha256_begin(ota_sha256_ctx_t *ctx)
{
    s_sha_begins++;
    (void) memset(ctx, 0, sizeof(*ctx));
    begin(state_of(ctx));

    return OTA_CRYPTO_OK;
}

ota_crypto_err_t ota_sha256_update(ota_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    mix(state_of(ctx), data, len);

    return OTA_CRYPTO_OK;
}

ota_crypto_err_t ota_sha256_finish(ota_sha256_ctx_t *ctx, uint8_t digest[OTA_SHA256_LEN])
{
    finish(state_of(ctx), digest);

    return OTA_CRYPTO_OK;
}

void fake_crypto_sign(const uint8_t digest[OTA_SHA256_LEN], uint8_t signature[OTA_SIGNATURE_LEN])
{
    for (uint32_t i = 0U; i < OTA_SHA256_LEN; i++)
    {
        signature[i] = digest[i];
        signature[OTA_SHA256_LEN + i] = (uint8_t) ~digest[i];
    }
}

ota_crypto_err_t ota_ecdsa_p256_verify(const uint8_t digest[OTA_SHA256_LEN],
                                       const uint8_t signature[OTA_SIGNATURE_LEN])
{
    uint8_t expected[OTA_SIGNATURE_LEN];

    s_verifies++;

    if (s_mode == FAKE_VERIFY_FORCE_FAILED)
    {
        return OTA_CRYPTO_ERR_FAILED;
    }

    if (s_mode == FAKE_VERIFY_FORCE_INVALID)
    {
        return OTA_CRYPTO_ERR_SIG_INVALID;
    }

    fake_crypto_sign(digest, expected);

    return (memcmp(expected, signature, sizeof(expected)) == 0) ? OTA_CRYPTO_OK
                                                                : OTA_CRYPTO_ERR_SIG_INVALID;
}

void fake_crypto_reset(void)
{
    s_mode = FAKE_VERIFY_NORMAL;
    s_sha_begins = 0U;
    s_verifies = 0U;
}

void fake_crypto_set_verify_mode(fake_verify_mode_t mode)
{
    s_mode = mode;
}

void fake_crypto_digest(const uint8_t *data, uint32_t len, uint8_t digest[OTA_SHA256_LEN])
{
    fake_sha_state_t st;

    begin(&st);
    mix(&st, data, len);
    finish(&st, digest);
}

uint32_t fake_crypto_sha_begin_calls(void)
{
    return s_sha_begins;
}

uint32_t fake_crypto_verify_calls(void)
{
    return s_verifies;
}
