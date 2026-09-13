/**
 * @file mbedtls_config_gateway.h
 * @brief Minimal mbedTLS build configuration for MqttClient (Gateway).
 *
 * Selected via -DMBEDTLS_CONFIG_FILE="<mbedtls_config_gateway.h>" in the
 * CubeIDE project (see .cproject). Enables only the modules required
 * for TLS 1.2 with cipher suite TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
 * and X.509 mutual authentication against AWS IoT Core (companion §3,
 * REQ-NF-300/301/305). Not a general-purpose mbedTLS configuration.
 *
 * RAM/flash footprint of this reduced module set is validated against
 * the 128 KB SRAM budget at integration — see MQTT-O1 in the
 * companion.
 */

#ifndef MBEDTLS_CONFIG_GATEWAY_H
#define MBEDTLS_CONFIG_GATEWAY_H

/* Platform: bare-metal, no libc entropy source beyond the STM32L4
 * on-chip RNG (see prv_entropy_poll() in mqtt_client.c). */
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* mbedTLS's own check_config.h hard-requires this (unconditionally on
 * Windows, so host unit tests need it too). It brings in mbedTLS's
 * internal calloc/free usage for RSA/ECC bignum scratch space — a
 * heap dependency inside the vendored crypto library itself, distinct
 * from and not a violation of this project's "no dynamic allocation
 * after init" rule for MqttClient's own code. Variable-size bignum
 * arithmetic for RSA-2048/ECDHE has no practical static-allocation
 * alternative; this is standard practice for embedded mbedTLS
 * deployments. Bounding this heap (e.g. via
 * mbedtls_memory_buffer_alloc_init() over a fixed static buffer, so
 * mbedTLS itself never touches the linker heap) is part of the
 * RAM-budget validation already tracked as MQTT-O1. */
#define MBEDTLS_PLATFORM_C

/* --- TLS protocol -------------------------------------------------- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2

/* --- Key exchange: ECDHE-RSA --------------------------------------- */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

/* --- Certificates: X.509 mutual auth, RSA, DER at load time -------- */
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

/* --- Cipher: AES-128-GCM -------------------------------------------- */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C

/* --- Hash: SHA-256 --------------------------------------------------- */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C

/* --- Big number support (RSA/ECC) ------------------------------------ */
#define MBEDTLS_BIGNUM_C
/* Performance (Phase 4 hardware finding, TC-HW-CP-004): at -O0 on the
 * 80 MHz M4 the client's RSA-2048 CertificateVerify signature plus the
 * P-256 ECDHE alone exceeded the 30 s handshake budget (MqttClient's
 * own deadline and Mosquitto's pre-CONNECT timeout are both 30 s). Both
 * options below are on in mbedTLS's default config and cost no RAM:
 * HAVE_ASM selects bn_mul.h's Thumb-2 multiply-accumulate inner loop
 * (the RSA/ECC hot path); ECP_NIST_OPTIM uses the NIST curves' special-
 * form modular reduction instead of generic Montgomery. */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_ECP_NIST_OPTIM

/* --- RNG: CTR_DRBG seeded from the on-chip hardware entropy source --- */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

#endif /* MBEDTLS_CONFIG_GATEWAY_H */
