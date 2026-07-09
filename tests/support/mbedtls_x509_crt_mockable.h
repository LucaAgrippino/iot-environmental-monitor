/**
 * @file mbedtls_x509_crt_mockable.h
 * @brief CMock-generatable proxy for mbedtls/x509_crt.h.
 *
 * The real vendor/mbedtls/include/mbedtls/x509_crt.h cannot be fed to
 * CMock directly: it hides struct fields behind an
 * `MBEDTLS_PRIVATE(name)` macro, which CMock's header parser
 * misreads as a function-like declaration ("Failed Parsing
 * Declaration Prototype!"). This proxy declares, with identical
 * signatures, only the three x509_crt functions mqtt_client.c calls.
 * CMock generates mock_mbedtls_x509_crt_mockable.{c,h} from THIS file
 * (test_mqtt_client.c includes the generated mock); mqtt_client.c
 * itself still includes the real mbedtls/x509_crt.h for the
 * mbedtls_x509_crt struct layout. Both resolve to the same linker
 * symbols (mbedtls_x509_crt_init/parse_der/free), so the mock
 * intercepts the real calls at link time.
 *
 * Keep signatures in sync with vendor/mbedtls/include/mbedtls/x509_crt.h
 * if the pinned mbedTLS version changes.
 */

#ifndef MBEDTLS_X509_CRT_MOCKABLE_H
#define MBEDTLS_X509_CRT_MOCKABLE_H

#include <stddef.h>

/* CMock only parses THIS file's own text (never follows #includes), so
 * the real header's struct definition is safely visible here — needed
 * because CMock's generated mock body does sizeof(mbedtls_x509_crt) for
 * parameter comparison, which requires a complete (non-opaque) type. */
#include "mbedtls/x509_crt.h"

void mbedtls_x509_crt_init(mbedtls_x509_crt *crt);

int mbedtls_x509_crt_parse_der(mbedtls_x509_crt *chain, const unsigned char *buf, size_t buflen);

void mbedtls_x509_crt_free(mbedtls_x509_crt *crt);

#endif /* MBEDTLS_X509_CRT_MOCKABLE_H */
