/**
 * @file test_fs_crypto_vectors.c
 * @brief FirmwareStore against the REAL mbedTLS SHA-256 and ECDSA-P256, with generated vectors.
 *
 * Test IDs follow docs/lld/middleware/firmware-store.md §22 "Verification"
 * (TC-FS-041, -044, -045, -046, -047) plus known-answer tests of the crypto seam
 * itself (TC-FS-C01..). The signed images in vectors/test_vectors.h come from
 * tools/ota/make_image.py (Python `cryptography`), so signing and verifying are
 * done by independent implementations.
 *
 * The real mbedTLS sources are built through mbedtls_host_build.c with a minimal
 * test configuration; nothing here uses the fake crypto, so this file must not
 * include fs_test_image.h.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, MBEDTLS_CONFIG_FILE, TEST
 * (:test_fs_crypto_vectors:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "freertos_mock.h" /* causes Ceedling to auto-link freertos_mock.c */

#include "config_store_crc.h" /* auto-links config_store_crc.c */
#include "fake_internal_flash.h"
#include "fake_qspi_flash.h"
#include "firmware_store.h"
#include "fs_metadata.h"
#include "fs_staging.h"
#include "fs_test_util.h"
#include "mbedtls_host_build.h" /* auto-links the unity build of the mbedTLS sources */
#include "ota_crypto.h"         /* auto-links ota_crypto.c */
#include "power_cut.h"
#include "test_vectors.h"

#define IMG (&firmware_store_image_ops)
#define IMAGE_ID (0x77U)
#define CHUNK (1000U)

/* The release key is a generated file; the vectors use their own TEST key instead. */
const uint8_t ota_public_key[OTA_PUBLIC_KEY_LEN] = {TV_PUBLIC_KEY_BYTES};

static uint8_t s_work[sizeof(tv_image_slot_b)];

void setUp(void)
{
    fsu_reset_world();
}

void tearDown(void)
{
}

static void arrange_capable(void)
{
    fsu_seed_running_a();
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
}

/** @brief Stage a vector image (possibly altered in s_work) and return the verify() result. */
static firmware_store_err_t stage_and_verify(void)
{
    firmware_image_info_t info;

    TEST_ASSERT_EQUAL_INT(
        FIRMWARE_STORE_ERR_OK,
        fsu_download(s_work, (uint32_t) sizeof(tv_image_slot_b), IMAGE_ID, CHUNK));

    return IMG->verify(fsu_fs, &info);
}

/* ====================================================================== */
/* The crypto seam on its own                                              */
/* ====================================================================== */

static void sha256_of(const uint8_t *data, uint32_t len, uint8_t out[OTA_SHA256_LEN])
{
    ota_sha256_ctx_t ctx;

    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_OK, ota_sha256_begin(&ctx));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_OK, ota_sha256_update(&ctx, data, len));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_OK, ota_sha256_finish(&ctx, out));
}

void test_TC_FS_C01_sha256_known_answers(void)
{
    static const uint8_t k_abc_digest[32] = {0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
                                             0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
                                             0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
                                             0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD};
    static const uint8_t k_two_block_digest[32] = {0x24, 0x8D, 0x6A, 0x61, 0xD2, 0x06, 0x38, 0xB8,
                                                   0xE5, 0xC0, 0x26, 0x93, 0x0C, 0x3E, 0x60, 0x39,
                                                   0xA3, 0x3C, 0xE4, 0x59, 0x64, 0xFF, 0x21, 0x67,
                                                   0xF6, 0xEC, 0xED, 0xD4, 0x19, 0xDB, 0x06, 0xC1};
    static const char k_two_block[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t digest[OTA_SHA256_LEN];

    sha256_of((const uint8_t *) "abc", 3U, digest);
    TEST_ASSERT_EQUAL_MEMORY(k_abc_digest, digest, sizeof(digest));

    sha256_of((const uint8_t *) k_two_block, (uint32_t) strlen(k_two_block), digest);
    TEST_ASSERT_EQUAL_MEMORY(k_two_block_digest, digest, sizeof(digest));
}

void test_TC_FS_C02_streaming_in_uneven_pieces_equals_one_shot(void)
{
    uint8_t one_shot[OTA_SHA256_LEN];
    uint8_t streamed[OTA_SHA256_LEN];
    ota_sha256_ctx_t ctx;

    sha256_of(tv_image_slot_b, (uint32_t) sizeof(tv_image_slot_b), one_shot);

    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_OK, ota_sha256_begin(&ctx));

    for (uint32_t pos = 0U, step = 1U; pos < sizeof(tv_image_slot_b);
         pos += step, step = (step * 3U) + 1U)
    {
        const uint32_t n =
            ((sizeof(tv_image_slot_b) - pos) < step) ? (sizeof(tv_image_slot_b) - pos) : step;

        TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_OK, ota_sha256_update(&ctx, &tv_image_slot_b[pos], n));
    }

    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_OK, ota_sha256_finish(&ctx, streamed));
    TEST_ASSERT_EQUAL_MEMORY(one_shot, streamed, sizeof(one_shot));
}

void test_TC_FS_C03_null_arguments_are_a_failure_not_a_crash(void)
{
    uint8_t digest[OTA_SHA256_LEN] = {0};
    ota_sha256_ctx_t ctx;

    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_FAILED, ota_sha256_begin(NULL));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_OK, ota_sha256_begin(&ctx));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_FAILED, ota_sha256_update(NULL, digest, 1U));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_FAILED, ota_sha256_update(&ctx, NULL, 1U));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_OK, ota_sha256_update(&ctx, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_FAILED, ota_sha256_finish(NULL, digest));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_FAILED, ota_sha256_finish(&ctx, NULL));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_FAILED, ota_ecdsa_p256_verify(NULL, digest));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_FAILED, ota_ecdsa_p256_verify(digest, NULL));
}

void test_TC_FS_C04_ecdsa_verifies_the_independently_made_signature(void)
{
    uint8_t digest[OTA_SHA256_LEN];
    uint8_t sig[OTA_SIGNATURE_LEN];

    sha256_of(tv_image_slot_b, FIRMWARE_SIGNED_HEADER_LEN, digest);
    (void) memcpy(sig, &tv_image_slot_b[FIRMWARE_SIGNED_HEADER_LEN], sizeof(sig));

    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_OK, ota_ecdsa_p256_verify(digest, sig));
}

void test_TC_FS_C05_ecdsa_rejects_a_changed_digest_or_signature(void)
{
    uint8_t digest[OTA_SHA256_LEN];
    uint8_t sig[OTA_SIGNATURE_LEN];

    sha256_of(tv_image_slot_b, FIRMWARE_SIGNED_HEADER_LEN, digest);
    (void) memcpy(sig, &tv_image_slot_b[FIRMWARE_SIGNED_HEADER_LEN], sizeof(sig));

    digest[5] ^= 0x10U;
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_SIG_INVALID, ota_ecdsa_p256_verify(digest, sig));
    digest[5] ^= 0x10U;

    for (uint32_t i = 0U; i < OTA_SIGNATURE_LEN; i += 9U)
    {
        sig[i] ^= 0x01U;
        TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_SIG_INVALID, ota_ecdsa_p256_verify(digest, sig));
        sig[i] ^= 0x01U;
    }

    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_OK, ota_ecdsa_p256_verify(digest, sig));
}

void test_TC_FS_C06_ecdsa_rejects_degenerate_signatures(void)
{
    uint8_t digest[OTA_SHA256_LEN];
    uint8_t sig[OTA_SIGNATURE_LEN];

    sha256_of(tv_image_slot_b, FIRMWARE_SIGNED_HEADER_LEN, digest);

    /* r = s = 0 */
    (void) memset(sig, 0, sizeof(sig));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_SIG_INVALID, ota_ecdsa_p256_verify(digest, sig));

    /* r = s = 2^256 - 1, far above the group order */
    (void) memset(sig, 0xFF, sizeof(sig));
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_SIG_INVALID, ota_ecdsa_p256_verify(digest, sig));
}

/* ====================================================================== */
/* FirmwareStore with the real crypto                                      */
/* ====================================================================== */

void test_TC_FS_041_a_validly_signed_image_verifies_installs_and_activates(void)
{
    firmware_image_info_t info = {0};
    firmware_slot_descriptor_t d;

    arrange_capable();
    (void) memcpy(s_work, tv_image_slot_b, sizeof(tv_image_slot_b));

    TEST_ASSERT_EQUAL_INT(
        FIRMWARE_STORE_ERR_OK,
        fsu_download(s_work, (uint32_t) sizeof(tv_image_slot_b), IMAGE_ID, CHUNK));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_EQUAL_HEX32(TV_VERSION, info.version);
    TEST_ASSERT_EQUAL_UINT32(TV_BODY_LEN, info.image_len);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, info.target_slot);

    /* The install re-hash with the real SHA-256 agrees with the signed digest. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_install_all(NULL));
    TEST_ASSERT_EQUAL_MEMORY(&tv_image_slot_b[FIRMWARE_HEADER_SIZE],
                             fake_iflash_ptr(FIRMWARE_SLOT_B_BASE), TV_BODY_LEN);

    (void) memcpy(&d, fake_iflash_ptr(FIRMWARE_DESC_B_ADDR), sizeof(d));
    TEST_ASSERT_EQUAL_HEX32(FIRMWARE_DESC_MAGIC, d.magic);
    TEST_ASSERT_EQUAL_HEX32(TV_VERSION, d.version);
    TEST_ASSERT_EQUAL_HEX32(config_store_crc32(&tv_image_slot_b[FIRMWARE_HEADER_SIZE], TV_BODY_LEN),
                            d.body_crc32);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));
}

void test_TC_FS_044_an_image_linked_for_the_running_slot_is_refused(void)
{
    arrange_capable();
    (void) memcpy(s_work, tv_image_slot_a, sizeof(tv_image_slot_a));

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_WRONG_SLOT, stage_and_verify());
}

void test_TC_FS_045_any_changed_signed_header_field_is_sig_invalid(void)
{
    /* version, a reserved byte, and body_sha256 (first and last byte): all covered by the
     * signature. */
    static const uint32_t k_offsets[] = {0x0CU, 0x14U, 0x1FU, 0x20U, 0x3FU};

    for (uint32_t i = 0U; i < (sizeof(k_offsets) / sizeof(k_offsets[0])); i++)
    {
        fsu_reset_world();
        arrange_capable();
        (void) memcpy(s_work, tv_image_slot_b, sizeof(tv_image_slot_b));
        s_work[k_offsets[i]] ^= 0x01U;

        TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_SIG_INVALID, stage_and_verify());
    }
}

void test_TC_FS_045b_a_changed_signature_byte_is_sig_invalid(void)
{
    arrange_capable();
    (void) memcpy(s_work, tv_image_slot_b, sizeof(tv_image_slot_b));
    s_work[FIRMWARE_SIGNED_HEADER_LEN + 17U] ^= 0x80U;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_SIG_INVALID, stage_and_verify());
}

void test_TC_FS_046_a_changed_body_byte_is_a_hash_mismatch(void)
{
    arrange_capable();
    (void) memcpy(s_work, tv_image_slot_b, sizeof(tv_image_slot_b));
    s_work[FIRMWARE_HEADER_SIZE + 2000U] ^= 0x01U; /* the signature over the header is still good */

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_HASH_MISMATCH, stage_and_verify());
}

void test_TC_FS_047_an_image_signed_with_another_key_is_sig_invalid(void)
{
    arrange_capable();
    (void) memcpy(s_work, tv_image_other_key, sizeof(tv_image_other_key));

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_SIG_INVALID, stage_and_verify());
}
