/**
 * @file fake_transport.c
 * @brief Fake IOtaTransport and probes (see fake_transport.h).
 */

#include "fake_transport.h"

#include <string.h>

#include "fake_log.h"

static bool s_connected;
static uint32_t s_connected_calls;
static bool s_accept_requests;
static uint32_t s_refuse_reports;
static uint32_t s_report_calls;
static us_chunk_request_t s_requests[FT_LOG_MAX];
static uint32_t s_request_count;
static us_ota_report_t s_reports[FT_LOG_MAX];
static uint32_t s_report_count;
static bool s_sensors;
static bool s_modbus;

static bool t_is_connected(void *ctx)
{
    (void) ctx;
    fake_audit_mutex();
    s_connected_calls++;

    return s_connected;
}

static bool t_request_chunk(void *ctx, const us_chunk_request_t *req)
{
    (void) ctx;
    fake_audit_mutex();
    fake_trace_add("request");

    if (s_request_count < FT_LOG_MAX)
    {
        s_requests[s_request_count] = *req;
        s_request_count++;
    }

    return s_accept_requests;
}

static bool t_report(void *ctx, const us_ota_report_t *rep)
{
    (void) ctx;
    static const char *const k_tags[] = {"report_accepted", "report_rejected", "report_failed",
                                         "report_success", "report_rolled_back"};

    fake_audit_mutex();
    s_report_calls++;
    fake_trace_add(((uint32_t) rep->result < 5U) ? k_tags[rep->result] : "report");

    if (s_refuse_reports > 0U)
    {
        s_refuse_reports--;
        return false;
    }

    if (s_report_count < FT_LOG_MAX)
    {
        s_reports[s_report_count] = *rep;
        s_report_count++;
    }

    return true;
}

static bool p_sensors(void *ctx)
{
    (void) ctx;
    fake_audit_mutex();

    return s_sensors;
}

static bool p_modbus(void *ctx)
{
    (void) ctx;
    fake_audit_mutex();

    return s_modbus;
}

const iota_transport_t fake_transport = {
    .ctx = NULL,
    .is_connected = t_is_connected,
    .request_chunk = t_request_chunk,
    .report = t_report,
};

const us_probe_if_t fake_probes = {
    .ctx = NULL,
    .sensors_ready = p_sensors,
    .modbus_ready = p_modbus,
};

void fake_transport_reset(void)
{
    s_connected = true;
    s_connected_calls = 0U;
    s_accept_requests = true;
    s_refuse_reports = 0U;
    s_report_calls = 0U;
    s_request_count = 0U;
    s_report_count = 0U;
    s_sensors = true;
    s_modbus = true;
    (void) memset(s_requests, 0, sizeof(s_requests));
    (void) memset(s_reports, 0, sizeof(s_reports));
}

void fake_transport_set_connected(bool connected)
{
    s_connected = connected;
}

uint32_t fake_transport_connected_calls(void)
{
    return s_connected_calls;
}

void fake_transport_set_accept_requests(bool accept)
{
    s_accept_requests = accept;
}

void fake_transport_refuse_reports(uint32_t n)
{
    s_refuse_reports = n;
}

uint32_t fake_transport_request_count(void)
{
    return s_request_count;
}

const us_chunk_request_t *fake_transport_request_at(uint32_t index)
{
    return &s_requests[index];
}

uint32_t fake_transport_report_calls(void)
{
    return s_report_calls;
}

uint32_t fake_transport_report_count(void)
{
    return s_report_count;
}

const us_ota_report_t *fake_transport_report_at(uint32_t index)
{
    return &s_reports[index];
}

void fake_probes_set(bool sensors, bool modbus)
{
    s_sensors = sensors;
    s_modbus = modbus;
}
