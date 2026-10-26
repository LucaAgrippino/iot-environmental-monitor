/**
 * @file mbedtls_host_build.h
 * @brief Pulls the mbedTLS sources needed by ota_crypto.c into one host translation unit.
 *
 * Ceedling links a .c file only when the test includes its header. mbedTLS spreads
 * SHA-256 and ECDSA over a dozen files with no single header, and adding the
 * vendor library directory to the global source path would put its generic
 * header names into every test's search path. This header has a matching .c that
 * includes the library files by relative path instead, so including this header
 * in test_fs_crypto_vectors.c is all it takes.
 */

#ifndef MBEDTLS_HOST_BUILD_H
#define MBEDTLS_HOST_BUILD_H

#endif /* MBEDTLS_HOST_BUILD_H */
