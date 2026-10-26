/**
 * @file ota_public_key.c
 * @brief Release public key for OTA image verification (raw P-256, X || Y).
 *
 * PLACEHOLDER. The key below is all zeros, which is not a point on the curve, so
 * ota_ecdsa_p256_verify() fails closed and no image can be installed until a real
 * key is provisioned. Generate a key pair and regenerate this file with:
 *
 *   tools/ota/make_image.py genkey --private <path outside the repo> --public-c
 * firmware/gateway/middleware/firmware_store/ota_public_key.c
 *
 * The private key never enters git (tools/ota/README.md).
 */

#include "ota_crypto.h"

const uint8_t ota_public_key[OTA_PUBLIC_KEY_LEN] = {0};
