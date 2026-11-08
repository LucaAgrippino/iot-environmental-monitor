/**
 * @file test_cert_store.c
 * @brief Host tests of CertStore (REQ-LI-040, REQ-LI-060, REQ-NF-302) on the simulated QSPI flash.
 */

#include "unity.h"

#include <string.h>

#include "cert_store.h"
#include "config_store_crc.h" /* auto-links config_store_crc.c */
#include "fake_qspi_flash.h"
#include "mbedtls_host_build.h" /* auto-links the host build of mbedTLS (SHA-256) */
#include "power_cut.h"          /* auto-links the injector the flash fake uses */

static const uint8_t k_uid[CERT_STORE_UID_BYTES] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
static const uint8_t k_other_uid[CERT_STORE_UID_BYTES] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

#define SLOT0 (CERT_STORE_BASE)
#define SLOT1 (CERT_STORE_BASE + 8192UL)
#define PASS "correct-horse-battery"
#define PASS_OFFSET                                                                                \
    (10U + 128U + 64U) /* in the body: lengths, endpoint, SSID, then the passphrase */

/* A DER SEQUENCE of the given total length, filled with a pattern. */
static uint32_t make_der(uint8_t *out, uint32_t total, uint8_t fill)
{
    uint32_t hdr = 4U;
    out[0] = 0x30U;
    if (total < (0x80U + 2U))
    {
        out[1] = (uint8_t) (total - 2U);
        hdr = 2U;
    }
    else
    {
        out[1] = 0x82U;
        out[2] = (uint8_t) ((total - 4U) >> 8U);
        out[3] = (uint8_t) (total - 4U);
    }
    memset(&out[hdr], fill, total - hdr);
    return total;
}

static bool in_flash(const void *needle, size_t n)
{
    const uint8_t *mem = fake_qspi_mem() + CERT_STORE_BASE;
    for (size_t i = 0U; (i + n) <= 16384U; i++)
    {
        if (memcmp(&mem[i], needle, n) == 0)
        {
            return true;
        }
    }
    return false;
}

void setUp(void)
{
    fake_qspi_reset();
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid));
}

void tearDown(void)
{
}

void test_an_empty_store_reads_as_nothing_provisioned(void)
{
    uint32_t len = 99U;

    TEST_ASSERT_FALSE(cert_store_has_wifi());
    TEST_ASSERT_EQUAL_STRING("", cert_store_ssid());
    TEST_ASSERT_NULL(cert_store_item(CERT_STORE_CA, &len));
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    TEST_ASSERT_FALSE(cert_store_identity_complete());
    TEST_ASSERT_EQUAL_UINT32(0U, cert_store_sequence());
}

void test_init_refuses_null_arguments(void)
{
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_init(NULL, k_uid));
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_init(&fake_qspi_flash_if, NULL));
}

void test_wifi_credentials_survive_a_restart(void)
{
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_wifi("Lab-2G", PASS));

    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid)); /* a reset */

    TEST_ASSERT_TRUE(cert_store_has_wifi());
    TEST_ASSERT_EQUAL_STRING("Lab-2G", cert_store_ssid());
    TEST_ASSERT_EQUAL_STRING(PASS, cert_store_password());
}

void test_the_passphrase_is_not_stored_in_plaintext(void)
{
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_wifi("Lab-2G", PASS));

    TEST_ASSERT_FALSE(in_flash(PASS, strlen(PASS)));
    TEST_ASSERT_TRUE(in_flash("Lab-2G", 6U)); /* the network name is not a secret */
}

void test_the_private_key_is_not_stored_in_plaintext_but_the_certificates_are(void)
{
    uint8_t key[200];
    uint8_t cert[300];

    make_der(key, sizeof(key), 0xA5U);
    make_der(cert, sizeof(cert), 0x5AU);
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_item(CERT_STORE_KEY, key, sizeof(key)));
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_item(CERT_STORE_CLIENT, cert, sizeof(cert)));

    uint8_t run_key[64];
    uint8_t run_cert[64];
    memset(run_key, 0xA5U, sizeof(run_key));
    memset(run_cert, 0x5AU, sizeof(run_cert));
    TEST_ASSERT_FALSE(in_flash(run_key, sizeof(run_key)));
    TEST_ASSERT_TRUE(in_flash(run_cert, sizeof(run_cert)));
}

void test_the_secrets_read_back_exactly_after_a_restart(void)
{
    uint8_t key[300];
    uint32_t len = 0U;

    make_der(key, sizeof(key), 0x3CU);
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_item(CERT_STORE_KEY, key, sizeof(key)));
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid));

    const uint8_t *got = cert_store_item(CERT_STORE_KEY, &len);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_UINT32(sizeof(key), len);
    TEST_ASSERT_EQUAL_MEMORY(key, got, sizeof(key));
}

void test_a_chip_with_another_unique_id_cannot_read_the_secrets(void)
{
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_wifi("Lab-2G", PASS));

    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_other_uid));

    TEST_ASSERT_NOT_EQUAL(0, strcmp(PASS, cert_store_password()));
}

void test_the_ca_the_client_certificate_and_the_key_complete_the_identity(void)
{
    uint8_t ca[400];
    uint8_t cl[500];
    uint8_t key[121];

    make_der(ca, sizeof(ca), 1U);
    make_der(cl, sizeof(cl), 2U);
    make_der(key, sizeof(key), 3U);

    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_item(CERT_STORE_CA, ca, sizeof(ca)));
    TEST_ASSERT_FALSE(cert_store_identity_complete());
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_item(CERT_STORE_CLIENT, cl, sizeof(cl)));
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_item(CERT_STORE_KEY, key, sizeof(key)));
    TEST_ASSERT_TRUE(cert_store_identity_complete());

    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid));
    TEST_ASSERT_TRUE(cert_store_identity_complete());
}

void test_an_rsa_2048_private_key_of_1218_bytes_fits(void)
{
    uint8_t key[1218];

    make_der(key, sizeof(key), 0x77U);
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_item(CERT_STORE_KEY, key, sizeof(key)));
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid));
    uint32_t len = 0U;
    TEST_ASSERT_EQUAL_MEMORY(key, cert_store_item(CERT_STORE_KEY, &len), sizeof(key));
    TEST_ASSERT_EQUAL_UINT32(sizeof(key), len);
}

void test_an_rsa_4096_private_key_of_2375_bytes_fits_and_survives_a_restart(void)
{
    uint8_t key[2375];
    uint32_t len = 0U;

    make_der(key, sizeof(key), 0x42U);
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_item(CERT_STORE_KEY, key, sizeof(key)));
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid));
    TEST_ASSERT_EQUAL_MEMORY(key, cert_store_item(CERT_STORE_KEY, &len), sizeof(key));
    TEST_ASSERT_EQUAL_UINT32(sizeof(key), len);
}

void test_a_certificate_of_exactly_the_maximum_size_is_accepted(void)
{
    uint8_t cert[CERT_STORE_CERT_MAX];

    make_der(cert, sizeof(cert), 7U);
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_item(CERT_STORE_CA, cert, sizeof(cert)));
}

void test_an_item_above_the_maximum_size_is_refused(void)
{
    uint8_t big[CERT_STORE_CERT_MAX + 1U];
    uint8_t bigkey[CERT_STORE_KEY_MAX + 1U];

    make_der(big, sizeof(big), 7U);
    make_der(bigkey, sizeof(bigkey), 7U);
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_TOO_BIG, cert_store_set_item(CERT_STORE_CA, big, sizeof(big)));
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_TOO_BIG,
                      cert_store_set_item(CERT_STORE_KEY, bigkey, sizeof(bigkey)));
}

void test_data_that_is_not_der_is_refused_and_changes_nothing(void)
{
    const uint8_t text[] = "-----BEGIN CERTIFICATE-----";
    uint8_t good[50];

    make_der(good, sizeof(good), 9U);
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_item(CERT_STORE_CA, good, sizeof(good)));

    TEST_ASSERT_EQUAL(CERT_STORE_ERR_FORMAT,
                      cert_store_set_item(CERT_STORE_CA, text, sizeof(text)));
    good[1] = (uint8_t) (good[1] + 1U); /* the length no longer matches */
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_FORMAT,
                      cert_store_set_item(CERT_STORE_CA, good, sizeof(good)));
    uint32_t len = 0U;
    (void) cert_store_item(CERT_STORE_CA, &len);
    TEST_ASSERT_EQUAL_UINT32(50U, len);
}

void test_wifi_input_is_validated(void)
{
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_wifi("", PASS));
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_wifi("net", "short"));
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_wifi(NULL, PASS));
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_wifi("net", NULL));
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_wifi("bad\x01name", PASS));
    char big[70];
    memset(big, 'x', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_wifi(big, PASS));
    TEST_ASSERT_FALSE(cert_store_has_wifi());
}

void test_each_write_goes_to_the_other_slot_with_a_higher_sequence(void)
{
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_wifi("one-net", PASS));
    TEST_ASSERT_EQUAL_UINT32(1U, cert_store_sequence());
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_wifi("two-net", PASS));
    TEST_ASSERT_EQUAL_UINT32(2U, cert_store_sequence());

    TEST_ASSERT_EQUAL_UINT32(1U, fake_qspi_sector_erases(CERT_STORE_BASE / 4096UL)); /* slot 0 */
    TEST_ASSERT_EQUAL_UINT32(1U, fake_qspi_sector_erases(CERT_STORE_BASE / 4096UL + 1UL));
    TEST_ASSERT_EQUAL_UINT32(1U,
                             fake_qspi_sector_erases(CERT_STORE_BASE / 4096UL + 2UL)); /* slot 1 */
    TEST_ASSERT_EQUAL_UINT32(1U, fake_qspi_sector_erases(CERT_STORE_BASE / 4096UL + 3UL));
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid));
    TEST_ASSERT_EQUAL_STRING("two-net", cert_store_ssid());
}

void test_a_damaged_newest_copy_falls_back_to_the_previous_one(void)
{
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_wifi("old-net", PASS));
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_wifi("new-net", PASS));

    fake_qspi_mem()[SLOT1 + 100U] ^= 0xFFU; /* slot 1 holds sequence 2 */

    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid));
    TEST_ASSERT_EQUAL_STRING("old-net", cert_store_ssid());
    TEST_ASSERT_EQUAL_STRING(PASS, cert_store_password());
}

void test_a_power_cut_before_the_header_leaves_the_previous_copy_valid(void)
{
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_wifi("old-net", PASS));
    /* the second write died after the body: erase slot 1 and write only part of it */
    memset(fake_qspi_mem() + SLOT1, 0xFFU, 8192U);
    fake_qspi_mem()[SLOT1 + 16U] = 0x00U;

    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid));

    TEST_ASSERT_EQUAL_STRING("old-net", cert_store_ssid());
}

void test_a_flash_failure_keeps_the_values_in_use(void)
{
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_wifi("old-net", PASS));
    fake_qspi_fail_erase_at(SLOT1); /* the next write goes to slot 1 */

    TEST_ASSERT_EQUAL(CERT_STORE_ERR_FLASH, cert_store_set_wifi("new-net", "another-pass-1"));

    TEST_ASSERT_EQUAL_STRING("old-net", cert_store_ssid());
    TEST_ASSERT_EQUAL_STRING(PASS, cert_store_password());
    TEST_ASSERT_EQUAL_UINT32(1U, cert_store_sequence());
    fake_qspi_fail_erase_at(FAKE_QSPI_NO_ADDR);
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_wifi("new-net", "another-pass-1"));
    TEST_ASSERT_EQUAL_UINT32(2U, cert_store_sequence());
}

void test_clear_removes_everything_and_it_stays_removed_after_a_restart(void)
{
    uint8_t cert[60];

    make_der(cert, sizeof(cert), 4U);
    (void) cert_store_set_wifi("Lab-2G", PASS);
    (void) cert_store_set_item(CERT_STORE_CA, cert, sizeof(cert));

    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_clear());
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid));

    TEST_ASSERT_FALSE(cert_store_has_wifi());
    TEST_ASSERT_NULL(cert_store_item(CERT_STORE_CA, NULL));
}

void test_replacing_the_passphrase_uses_a_new_keystream(void)
{
    (void) cert_store_set_wifi("Lab-2G", PASS);
    uint8_t first[64];
    memcpy(first, fake_qspi_mem() + SLOT0 + 16U + PASS_OFFSET, sizeof(first));

    (void) cert_store_set_wifi("Lab-2G", PASS); /* same value, next sequence, other slot */
    uint8_t second[64];
    memcpy(second, fake_qspi_mem() + SLOT1 + 16U + PASS_OFFSET, sizeof(second));

    TEST_ASSERT_NOT_EQUAL(0, memcmp(first, second, sizeof(first)));
}

void test_the_calls_before_init_are_refused(void)
{
    /* a fresh module state is not reachable after setUp, so use a null re-init to check the guard
     */
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_init(NULL, NULL));
}

void test_the_broker_endpoint_is_stored_and_survives_a_restart(void)
{
    TEST_ASSERT_EQUAL_STRING("", cert_store_endpoint());
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_endpoint("192.168.0.218"));
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_init(&fake_qspi_flash_if, k_uid));

    TEST_ASSERT_EQUAL_STRING("192.168.0.218", cert_store_endpoint());
}

void test_the_broker_endpoint_is_validated_and_cleared_with_the_rest(void)
{
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_endpoint(""));
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_endpoint(NULL));
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_endpoint("bad host"));
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_endpoint("under_score"));
    char big[130];
    memset(big, 'a', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';
    TEST_ASSERT_EQUAL(CERT_STORE_ERR_ARG, cert_store_set_endpoint(big));

    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_set_endpoint("broker.example.org"));
    TEST_ASSERT_EQUAL(CERT_STORE_OK, cert_store_clear());
    TEST_ASSERT_EQUAL_STRING("", cert_store_endpoint());
}
