/**
 * @file fake_transport.h
 * @brief Fake IOtaTransport and self-check probes (UpdateService tests).
 *
 * Including this header makes Ceedling auto-link fake_transport.c.
 */

#ifndef FAKE_TRANSPORT_H
#define FAKE_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "iota_transport.h"
#include "update_service_types.h"

#define FT_LOG_MAX (64U)

extern const iota_transport_t fake_transport;
extern const us_probe_if_t fake_probes;

/** @brief Link up, queues accept, probes ready; logs cleared. */
void fake_transport_reset(void);

void fake_transport_set_connected(bool connected);
uint32_t fake_transport_connected_calls(void);

/** @brief request_chunk() returns false while this is false. */
void fake_transport_set_accept_requests(bool accept);

/** @brief The next n report() calls return false (queue full); later ones succeed. */
void fake_transport_refuse_reports(uint32_t n);

uint32_t fake_transport_request_count(void);
const us_chunk_request_t *fake_transport_request_at(uint32_t index);

/** @brief Report() calls made, including refused ones; and the accepted reports. */
uint32_t fake_transport_report_calls(void);
uint32_t fake_transport_report_count(void);
const us_ota_report_t *fake_transport_report_at(uint32_t index);

void fake_probes_set(bool sensors, bool modbus);

#endif /* FAKE_TRANSPORT_H */
