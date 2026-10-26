/**
 * @file mbedtls_host_build.c
 * @brief One host translation unit holding the mbedTLS sources ota_crypto.c needs.
 *
 * Built only with the MBEDTLS_CONFIG_FILE define of :test_fs_crypto_vectors:.
 * The library files are included by path relative to this file; their own quoted
 * includes (common.h, bignum_core.h ...) then resolve beside them.
 */

#include "mbedtls_host_build.h"

#include "../../../../../vendor/mbedtls/library/platform_util.c"
#include "../../../../../vendor/mbedtls/library/platform.c"
#include "../../../../../vendor/mbedtls/library/constant_time.c"
#include "../../../../../vendor/mbedtls/library/sha256.c"
#include "../../../../../vendor/mbedtls/library/md.c"
#include "../../../../../vendor/mbedtls/library/hmac_drbg.c"
#include "../../../../../vendor/mbedtls/library/asn1parse.c"
#include "../../../../../vendor/mbedtls/library/asn1write.c"
#include "../../../../../vendor/mbedtls/library/bignum_core.c"
#include "../../../../../vendor/mbedtls/library/bignum_mod_raw.c"
#include "../../../../../vendor/mbedtls/library/bignum_mod.c"
#include "../../../../../vendor/mbedtls/library/bignum.c"
#include "../../../../../vendor/mbedtls/library/ecp_curves.c"
#include "../../../../../vendor/mbedtls/library/ecp.c"
#include "../../../../../vendor/mbedtls/library/ecdsa.c"
