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

/* Platform layer: mbedTLS's allocator and the check_config.h prerequisite of the arena below.
 * mbedTLS never calls the C library's calloc/free (REQ-NF-408): MBEDTLS_PLATFORM_MEMORY routes
 * every allocation through mbedtls_platform_set_calloc_free(), and tls_arena_init() points that at
 * a static arena. Until then the default allocator is the pair below, which refuses every request
 * (NULL, an mbedTLS ALLOC_FAILED error) instead of silently using the heap. */
#include <stddef.h>

void *tls_arena_uninit_calloc(size_t count, size_t size);
void tls_arena_uninit_free(void *ptr);

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_STD_CALLOC tls_arena_uninit_calloc
#define MBEDTLS_PLATFORM_STD_FREE tls_arena_uninit_free

/* The arena's debug dump prints to stderr, which does not exist: print nothing. Its corruption
 * checks call exit(1): trap, so a damaged arena faults at once instead of running on. */
#define MBEDTLS_PLATFORM_FPRINTF_MACRO(...) ((void) 0)
#define MBEDTLS_PLATFORM_EXIT_MACRO(status) __builtin_trap()

/* --- Static arena (tls_arena.c, WP2, MQTT-O1, FS-O4) ----------------
 * TLS (CloudPublisherTask) and OTA image verification (UpdateServiceTask) both allocate, so the
 * arena is guarded by a FreeRTOS mutex through mbedTLS's threading layer (threading_alt.h).
 * MBEDTLS_MEMORY_DEBUG is on in every build, not only in a bring-up one: it costs a few counters
 * and gives the arena peak (tls_arena_peak_bytes()) for the health report and the harnesses. */
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C
#define MBEDTLS_MEMORY_DEBUG
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT

/* --- TLS record buffers (WP4 D-1 option F) -------------------------
 * The defaults are 16 384 bytes each, about 33 KB of the 46 KB arena peak. The input buffer must
 * hold the largest single handshake message the server sends, because mbedTLS reassembles a
 * fragmented handshake message inside it: AWS IoT Core's Certificate message is 3 847 bytes
 * (three RSA certificates), so 5 120 leaves 1 273 bytes for a longer chain after a rotation. It
 * also bounds the largest record a peer may send; MQTT_PKT_BUF_SIZE (4 096) bounds the packets, so
 * a record above 5 120 would need two large packets in one record. A record larger than the
 * buffer ends the connection. The output buffer only splits our own writes (coreMQTT retries a
 * short write), so 2 048 is enough. Measured peak against AWS IoT: 20 920 bytes (46 280 before). */
#define MBEDTLS_SSL_IN_CONTENT_LEN 5120
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048

/* --- TLS protocol -------------------------------------------------- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2

/* --- Key exchange: ECDHE-RSA --------------------------------------- */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

/* --- ECDSA-P256 verification of OTA images (FirmwareStore, FS-O4) ---
 * Not used by the TLS handshake: the key exchange above stays ECDHE-RSA and no ECDSA
 * cipher suite is enabled. Only ota_crypto.c calls mbedtls_ecdsa_verify(). Needs ECP, BIGNUM
 * and the ASN.1 modules, all of which are on. */
#define MBEDTLS_ECDSA_C

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
