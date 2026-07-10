/**
 * @file mbedtls_pk_mockable.h
 * @brief CMock-generatable proxy for mbedtls/pk.h.
 *
 * The real header declares ~30 functions, several with parameters of
 * types that are deliberately incomplete in the public API (e.g.
 * mbedtls_pk_setup()'s `const mbedtls_pk_info_t *info` — mbedtls_pk_info_t
 * has no public definition at all). CMock's generated mock body does
 * sizeof(<param type>) for parameter comparison, which fails to compile
 * against an incomplete type. This proxy declares, with identical
 * signatures, only the three pk functions mqtt_client.c calls — see
 * mbedtls_x509_crt_mockable.h for the same technique and full rationale.
 *
 * Keep signatures in sync with vendor/mbedtls/include/mbedtls/pk.h if
 * the pinned mbedTLS version changes.
 */

#ifndef MBEDTLS_PK_MOCKABLE_H
#define MBEDTLS_PK_MOCKABLE_H

#include <stddef.h>

#include "mbedtls/pk.h"

void mbedtls_pk_init(mbedtls_pk_context *ctx);

int mbedtls_pk_parse_key(mbedtls_pk_context *ctx, const unsigned char *key, size_t keylen,
                         const unsigned char *pwd, size_t pwdlen, mbedtls_f_rng_t *f_rng,
                         void *p_rng);

void mbedtls_pk_free(mbedtls_pk_context *ctx);

#endif /* MBEDTLS_PK_MOCKABLE_H */
