/**
 * @file test_fs_crypto_placeholder.c
 * @brief The committed placeholder public key must fail closed (FirmwareStore).
 *
 * ota_public_key.c ships as all zeros until a release key is provisioned
 * (tools/ota/README.md). All zeros is not a point on the curve, so no signature,
 * however good, may verify against it, and the failure must be reported as a
 * library/key fault (OTA_CRYPTO_ERR_FAILED, FirmwareStore CRYPTO) rather than as
 * a forged image. This links the REAL ota_public_key.c, which the vector test
 * replaces with its own TEST key.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, MBEDTLS_CONFIG_FILE, TEST
 * (:test_fs_crypto_placeholder:).
 */

#include "unity.h"

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
#include "ota_public_key.h"     /* auto-links the real ota_public_key.c */
#include "power_cut.h"
#include "test_vectors.h"

void setUp(void)
{
    fsu_reset_world();
}

void tearDown(void)
{
}

void test_TC_FS_C10_the_placeholder_key_is_all_zero(void)
{
    static const uint8_t k_zero[OTA_PUBLIC_KEY_LEN] = {0};

    TEST_ASSERT_EQUAL_MEMORY(k_zero, ota_public_key, sizeof(k_zero));
}

void test_TC_FS_C11_nothing_verifies_against_the_placeholder_key(void)
{
    uint8_t digest[OTA_SHA256_LEN] = {0};
    uint8_t sig[OTA_SIGNATURE_LEN];

    (void) memcpy(sig, &tv_image_slot_b[FIRMWARE_SIGNED_HEADER_LEN], sizeof(sig));

    /* A resource/key fault, not a verdict on the image. */
    TEST_ASSERT_EQUAL_INT(OTA_CRYPTO_ERR_FAILED, ota_ecdsa_p256_verify(digest, sig));
}

void test_TC_FS_C12_firmware_store_reports_crypto_not_forgery(void)
{
    static uint8_t file[sizeof(tv_image_slot_b)];
    firmware_image_info_t info;

    fsu_seed_running_a();
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    (void) memcpy(file, tv_image_slot_b, sizeof(file));

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          fsu_download(file, (uint32_t) sizeof(file), 5U, 1000U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_CRYPTO,
                          firmware_store_image_ops.verify(fsu_fs, &info));

    /* So no image can be installed before a real key is provisioned. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE,
                          firmware_store_image_ops.install_begin(fsu_fs));
}
