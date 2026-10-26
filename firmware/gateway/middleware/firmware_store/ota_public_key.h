/**
 * @file ota_public_key.h
 * @brief Marker header for ota_public_key.c.
 *
 * The key itself is declared in ota_crypto.h. This header exists only so that a
 * unit test can include it and have Ceedling link ota_public_key.c (the real,
 * generated key file) in place of a test-local key.
 */

#ifndef OTA_PUBLIC_KEY_H
#define OTA_PUBLIC_KEY_H

#include "ota_crypto.h"

#endif /* OTA_PUBLIC_KEY_H */
