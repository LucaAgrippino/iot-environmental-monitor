/**
 * @file mbedtls_ssl_mockable.h
 * @brief CMock-generatable proxy for mbedtls/ssl.h.
 *
 * The real header declares ~150 functions across many optional TLS
 * features (DTLS, session tickets, early data, PSK, ALPN...), several
 * with parameters of types that are incomplete or otherwise awkward for
 * CMock's parser/codegen. This proxy declares, with identical
 * signatures, only the ssl functions mqtt_client.c calls — see
 * mbedtls_x509_crt_mockable.h for the same technique and full
 * rationale.
 *
 * Keep signatures in sync with vendor/mbedtls/include/mbedtls/ssl.h if
 * the pinned mbedTLS version changes.
 */

#ifndef MBEDTLS_SSL_MOCKABLE_H
#define MBEDTLS_SSL_MOCKABLE_H

#include <stddef.h>

#include "mbedtls/ssl.h"

void mbedtls_ssl_init(mbedtls_ssl_context *ssl);
void mbedtls_ssl_free(mbedtls_ssl_context *ssl);

void mbedtls_ssl_config_init(mbedtls_ssl_config *conf);
void mbedtls_ssl_config_free(mbedtls_ssl_config *conf);
int mbedtls_ssl_config_defaults(mbedtls_ssl_config *conf, int endpoint, int transport, int preset);

void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *conf, int authmode);
void mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config *conf, mbedtls_x509_crt *ca_chain,
                               mbedtls_x509_crl *ca_crl);
int mbedtls_ssl_conf_own_cert(mbedtls_ssl_config *conf, mbedtls_x509_crt *own_cert,
                              mbedtls_pk_context *pk_key);
void mbedtls_ssl_conf_rng(mbedtls_ssl_config *conf, mbedtls_f_rng_t *f_rng, void *p_rng);

int mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf);
int mbedtls_ssl_set_hostname(mbedtls_ssl_context *ssl, const char *hostname);
void mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *p_bio, mbedtls_ssl_send_t *f_send,
                         mbedtls_ssl_recv_t *f_recv, mbedtls_ssl_recv_timeout_t *f_recv_timeout);

int mbedtls_ssl_handshake(mbedtls_ssl_context *ssl);
int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len);
int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len);
int mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl);

#endif /* MBEDTLS_SSL_MOCKABLE_H */
