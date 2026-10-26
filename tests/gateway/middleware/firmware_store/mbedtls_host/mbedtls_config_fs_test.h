/**
 * @file mbedtls_config_fs_test.h
 * @brief Minimal mbedTLS configuration for the host crypto-vector test (FirmwareStore).
 *
 * Just what ota_crypto.c needs: SHA-256 and ECDSA-P256 verify. Deliberately not
 * the Gateway TLS configuration: that one lacks MBEDTLS_ECDSA_C (open item FS-O4).
 */

#ifndef MBEDTLS_CONFIG_FS_TEST_H
#define MBEDTLS_CONFIG_FS_TEST_H

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C

#endif /* MBEDTLS_CONFIG_FS_TEST_H */
