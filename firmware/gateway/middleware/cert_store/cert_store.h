#ifndef CERT_STORE_H
#define CERT_STORE_H

/**
 * @file cert_store.h
 * @brief CertStore (Gateway): the WiFi credentials and the X.509 identity, kept in QSPI flash.
 *
 * Provisioned from the console (REQ-LI-040, REQ-LI-060). The WiFi passphrase and the private key
 * are stored encrypted (REQ-NF-302); the certificates are public data and stored as they are. Two 4
 * KB slots in the CertStore partition (`flash-partition-layout.md`: device offset 0x1_0000) are
 * written alternately, each with a sequence number and a CRC, so a power cut during a write leaves
 * the previous copy valid. Design: docs/lld/middleware/cert-store.md.
 *
 * The decrypted copy lives in RAM (the caller places it in SRAM2): WifiTask and MqttClient read
 * their credentials from it, so nothing is copied again.
 */

#include <stdbool.h>
#include <stdint.h>

#include "qspi_flash/qspi_flash.h"

#define CERT_STORE_SSID_MAX (63U) /**< Characters, without the terminator. */
#define CERT_STORE_PASS_MAX (63U)
#define CERT_STORE_CERT_MAX (1536U) /**< DER bytes of a certificate. */
#define CERT_STORE_KEY_MAX (512U)   /**< DER bytes of a private key. */
#define CERT_STORE_UID_BYTES (12U)  /**< The 96-bit unique ID of the MCU, the root of the key. */

/** @brief Device offset of the CertStore partition in the QSPI flash. */
#define CERT_STORE_BASE (0x00010000UL)

typedef enum
{
    CERT_STORE_OK = 0,
    CERT_STORE_ERR_ARG = 1,      /**< NULL, empty or out-of-range argument. */
    CERT_STORE_ERR_NOT_INIT = 2, /**< cert_store_init() has not been called. */
    CERT_STORE_ERR_TOO_BIG = 3,  /**< The item is larger than its slot in the layout. */
    CERT_STORE_ERR_FORMAT = 4,   /**< The item is not a DER structure. */
    CERT_STORE_ERR_FLASH = 5     /**< Erase or program failed; the previous copy is intact. */
} cert_store_err_t;

typedef enum
{
    CERT_STORE_CA = 0,     /**< Root CA certificate (DER). */
    CERT_STORE_CLIENT = 1, /**< Client certificate (DER). */
    CERT_STORE_KEY = 2     /**< Client private key (DER). */
} cert_store_item_t;

/**
 * @brief Load the newest valid slot. An empty or damaged store is not an error: it reads as
 * "nothing provisioned".
 *
 * @param[in] flash  QSPI flash interface (the real driver, or a fake in tests).
 * @param[in] uid    CERT_STORE_UID_BYTES bytes of the MCU unique ID.
 * @return CERT_STORE_OK, or CERT_STORE_ERR_ARG for a NULL argument.
 */
cert_store_err_t cert_store_init(const iqspi_flash_t *flash,
                                 const uint8_t uid[CERT_STORE_UID_BYTES]);

/** @return true when a WiFi SSID is stored. */
bool cert_store_has_wifi(void);
/** @return The stored SSID, NUL-terminated ("" when none). */
const char *cert_store_ssid(void);
/** @return The stored passphrase, decrypted, NUL-terminated ("" when none). */
const char *cert_store_password(void);

/**
 * @brief One stored item.
 *
 * @param[in]  item  Which item.
 * @param[out] len   Length in bytes; 0 when the item is absent.
 * @return Pointer to the DER bytes, or NULL when the item is absent.
 */
const uint8_t *cert_store_item(cert_store_item_t item, uint32_t *len);

/** @return true when the CA, the client certificate and the private key are all stored. */
bool cert_store_identity_complete(void);

/**
 * @brief Store the WiFi credentials (REQ-LI-040).
 *
 * @param[in] ssid  1 to CERT_STORE_SSID_MAX printable characters.
 * @param[in] pass  8 to CERT_STORE_PASS_MAX printable characters.
 * @return CERT_STORE_OK, CERT_STORE_ERR_ARG, CERT_STORE_ERR_NOT_INIT or CERT_STORE_ERR_FLASH.
 */
cert_store_err_t cert_store_set_wifi(const char *ssid, const char *pass);

/**
 * @brief Store one certificate or key (REQ-LI-060).
 *
 * @param[in] item  Which item.
 * @param[in] der   DER bytes; the first byte must be 0x30 and the encoded length must match.
 * @param[in] len   Number of bytes.
 * @return CERT_STORE_OK, CERT_STORE_ERR_ARG, _TOO_BIG, _FORMAT, _NOT_INIT or _FLASH.
 */
cert_store_err_t cert_store_set_item(cert_store_item_t item, const uint8_t *der, uint32_t len);

/** @brief Remove everything (WiFi credentials, certificates, key). */
cert_store_err_t cert_store_clear(void);

/** @return Sequence number of the copy in use (0 when the store is empty); for diagnostics. */
uint32_t cert_store_sequence(void);

#endif /* CERT_STORE_H */
