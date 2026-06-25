/**
 * @file health_monitor_stub.h
 * @brief Narrow stub for HealthMonitor in unit tests.
 *
 * Pulls in the three per-interface headers (ihealth_report.h,
 * ihealth_snapshot.h, ihealth_admin.h) which are pure-type, TEST-agnostic
 * headers. This prevents Ceedling from auto-linking health_monitor.c
 * (which cascades to led_driver → gpio → CMSIS).
 *
 * Singleton externs (health_report, health_snapshot, health_admin) are
 * declared in the interface headers. Test TUs that need them must define
 * their own spy instances and provide the definitions:
 *
 *   static const ihealth_report_t g_spy = { .push_event = spy_push };
 *   const ihealth_report_t *const health_report = &g_spy;
 *
 * Basename: health_monitor_stub — does NOT match health_monitor.c.
 */

#ifndef HEALTH_MONITOR_STUB_H
#define HEALTH_MONITOR_STUB_H

#include "health_monitor/ihealth_report.h"
#include "health_monitor/ihealth_snapshot.h"
#include "health_monitor/ihealth_admin.h"

#endif /* HEALTH_MONITOR_STUB_H */
