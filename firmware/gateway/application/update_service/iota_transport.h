/**
 * @file iota_transport.h
 * @brief IOtaTransport — chunk requests and result reports towards the cloud.
 *
 * Owned by UpdateService, implemented by CloudPublisher (an adapter in wiring code). The
 * interface lives in UpdateService's folder so that CloudPublisher includes only vtable
 * headers and there is no include cycle.
 *
 * @see docs/lld/application/update-service-lld.md §7.4
 */

#ifndef IOTA_TRANSPORT_H
#define IOTA_TRANSPORT_H

#include <stdbool.h>

#include "update_service_types.h"

typedef struct
{
    void *ctx;

    /** Volatile mirror of the MQTT connection state. Any task. */
    bool (*is_connected)(void *ctx);

    /**
     * Queue a chunk request. false = not queued (queue full). Dropped by CloudPublisher if the
     * link is down when it is dequeued. Never buffered.
     */
    bool (*request_chunk)(void *ctx, const us_chunk_request_t *req);

    /** Queue a result report. false = not queued. QoS 1; StoreAndForward when offline. */
    bool (*report)(void *ctx, const us_ota_report_t *rep);
} iota_transport_t;

#endif /* IOTA_TRANSPORT_H */
