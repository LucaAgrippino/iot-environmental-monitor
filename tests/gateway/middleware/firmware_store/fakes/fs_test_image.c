/**
 * @file fs_test_image.c
 * @brief Signed-image builder for the FirmwareStore unit tests that use the fake crypto.
 */

#include "fs_test_image.h"

#include <string.h>

#include "fake_ota_crypto.h"
#include "firmware_image_format.h"

void fsu_resign_header(uint8_t *file)
{
    uint8_t digest[OTA_SHA256_LEN];
    firmware_image_header_t *h = (firmware_image_header_t *) (void *) file;

    fake_crypto_digest(file, FIRMWARE_SIGNED_HEADER_LEN, digest);
    fake_crypto_sign(digest, h->signature);
}

void fsu_refresh_header(uint8_t *file)
{
    firmware_image_header_t *h = (firmware_image_header_t *) (void *) file;

    fake_crypto_digest(&file[FIRMWARE_HEADER_SIZE], h->image_len, h->body_sha256);
    fsu_resign_header(file);
}

uint32_t fsu_build_image(uint8_t *out, uint32_t body_len, uint32_t load_addr, uint32_t version)
{
    firmware_image_header_t h;
    uint8_t *body = &out[FIRMWARE_HEADER_SIZE];

    for (uint32_t i = 0U; i < body_len; i++)
    {
        body[i] = fsu_pattern(i, version);
    }

    /* Vector table: initial stack pointer, then a Thumb reset vector inside the image. */
    const uint32_t sp = 0x20018000UL;

    /* Thumb entry 0x100 into the image, or the last even offset of a tiny body. */
    const uint32_t entry = ((body_len - 2U) < 0x100U) ? ((body_len - 2U) & ~1UL) : 0x100U;
    const uint32_t reset = load_addr + entry + 1U;

    (void) memcpy(&body[0], &sp, sizeof(sp));
    (void) memcpy(&body[4], &reset, sizeof(reset));

    (void) memset(&h, 0, sizeof(h));
    h.magic = FIRMWARE_IMAGE_MAGIC;
    h.header_version = FIRMWARE_IMAGE_HEADER_VERSION;
    h.header_size = FIRMWARE_HEADER_SIZE;
    h.image_len = body_len;
    h.version = version;
    h.load_addr = load_addr;
    fake_crypto_digest(body, body_len, h.body_sha256);

    (void) memcpy(out, &h, sizeof(h));
    fsu_resign_header(out);

    return FIRMWARE_HEADER_SIZE + body_len;
}
