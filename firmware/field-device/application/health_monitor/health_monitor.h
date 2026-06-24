/**
 * @file health_monitor.h
 * @brief HealthMonitor — device health aggregator and LED status indicator.
 *
 * Provides three vtable interfaces (exported via per-interface headers):
 *   - IHealthReport  (write side — producers push metric events)
 *   - IHealthSnapshot (read side — consumers copy the consolidated snapshot)
 *   - IHealthAdmin   (control side — LifecycleController resets counters)
 *
 * HealthMonitor has no thread. All operations execute in the calling
 * task's context, serialised by an internal priority-inheritance mutex.
 *
 * @see docs/lld/application/health-monitor.md
 */

#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

/* Per-interface headers — consumers that only need one vtable type may
 * include the relevant i*.h directly without pulling in this header. */
#include "health_monitor/ihealth_report.h"
#include "health_monitor/ihealth_snapshot.h"
#include "health_monitor/ihealth_admin.h"

/* ======================================================================= */
/* Free-function API (implementations in health_monitor.c)                 */
/* ======================================================================= */

/**
 * @brief Initialise HealthMonitor.
 *
 * Creates the internal mutex and zeroes the snapshot. Must be called once
 * before any producer calls IHealthReport.
 *
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_init(void);

/**
 * @brief Push a named event into the snapshot.
 *
 * Thread-safe. Acquires mutex, updates the corresponding counter or flag,
 * releases mutex, then drives update_led_state() if the event affects
 * LED indication. Safe from any task context; not ISR-safe.
 *
 * @param  event  Event identifier.
 * @param  param  Event-specific parameter (sensor_id, fault code, etc.).
 *                Pass 0 if not applicable.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_push_event(health_event_t event, uint32_t param);

/**
 * @brief Update Modbus slave statistics in the snapshot (FD).
 *
 * Called by ModbusRegisterMap each cycle. Acquires mutex; copies stats
 * fields atomically.
 *
 * @param  stats  Pointer to the latest slave statistics. Must not be NULL.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_update_modbus_slave_stats(const modbus_slave_stats_t *stats);

#if defined(BOARD_GATEWAY)
/**
 * @brief Update Modbus master statistics in the snapshot (GW).
 *
 * @param  stats  Pointer to the latest master statistics. Must not be NULL.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_update_modbus_master_stats(const modbus_master_stats_t *stats);

/**
 * @brief Update MQTT statistics in the snapshot (GW).
 *
 * @param  stats  Pointer to the latest MQTT statistics. Must not be NULL.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_update_mqtt_stats(const mqtt_stats_t *stats);

/**
 * @brief Update store-and-forward buffer occupancy (GW).
 *
 * @param  entry_count  Current number of buffered entries.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_update_buffer_occupancy(uint32_t entry_count);
#endif /* BOARD_GATEWAY */

/**
 * @brief Update RTOS task stack watermarks in the snapshot.
 *
 * Called by health_monitor_poll() on each health-report interval.
 *
 * @param  watermarks  Array of HEALTH_TASK_COUNT minimum free stack words.
 *                     Must not be NULL.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t
health_monitor_update_stack_watermarks(const uint16_t watermarks[HEALTH_TASK_COUNT]);

/**
 * @brief Get a copy of the current health snapshot.
 *
 * Thread-safe. Acquires mutex; copies the entire snapshot; releases mutex.
 * The caller owns the returned copy — subsequent push_event() calls do not
 * affect it.
 *
 * @param[out] snap_out  Destination to fill. Must not be NULL.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_get_snapshot(device_health_snapshot_t *snap_out);

/**
 * @brief Override all LEDs to the Faulted pattern immediately.
 *
 * Called by LifecycleController on entering the Faulted state. Bypasses
 * the normal update_led_state() priority logic.
 */
void health_monitor_set_led_fault(void);

/**
 * @brief Reset all counter-type metrics to zero (LLD-D15).
 *
 * Zeroes cumulative counters (Modbus, sensor fail, alarm raise, MQTT).
 * Preserves event-flags (sync state, persistence-fail), lifecycle state,
 * and uptime.
 *
 * Thread-safe — acquires the internal mutex.
 *
 * @return HEALTH_ADMIN_ERR_OK on success; non-zero on failure.
 */
health_admin_err_t health_monitor_reset_metrics(void);

/**
 * @brief Poll stack watermarks and uptime for all registered tasks.
 *
 * Called by the component that owns the health-report interval
 * (ModbusRegisterMap or LifecycleTask on FD; CloudPublisher on GW).
 * Not ISR-safe.
 */
void health_monitor_poll(void);

/**
 * @brief Register a task handle for stack-watermark polling.
 *
 * Must be called once per task, from the task itself or from main(),
 * before health_monitor_poll() is first called.
 *
 * @param  name         Short task name (up to 15 chars + NUL).
 * @param  task_handle  FreeRTOS TaskHandle_t (passed as void * to avoid
 *                      exposing task.h in this header).
 * @return HEALTH_MONITOR_ERR_OK on success; HEALTH_MONITOR_ERR_NULL_ARG if
 *         either pointer is NULL; HEALTH_MONITOR_ERR_NOT_INIT if called
 *         before health_monitor_init().
 */
health_monitor_err_t health_monitor_register_task(const char *name, void *task_handle);

/* ======================================================================= */
/* Test-only exposure                                                      */
/* ======================================================================= */

#ifdef HEALTH_MONITOR_TEST_VISIBLE
#undef HEALTH_MONITOR_TEST_VISIBLE
#endif

#ifdef TEST
#define HEALTH_MONITOR_TEST_VISIBLE
#else
#define HEALTH_MONITOR_TEST_VISIBLE static
#endif

#ifdef TEST
/**
 * @brief Reset module state between unit tests.
 *
 * Clears s_hm to its post-BSS value. Must be called from setUp().
 */
void health_monitor_reset_for_test(void);
#endif /* TEST */

/* In UNIT_TEST builds, led_driver_set() is macro-replaced with a spy stub
 * so the test TU can inspect which LED calls were made. */
#ifdef UNIT_TEST
/* Forward declaration — definition provided by the test TU. */
void stub_led_set(uint32_t id, uint32_t state);
#define led_driver_set(id, state) stub_led_set((uint32_t) (id), (uint32_t) (state))
#endif /* UNIT_TEST */

#endif /* HEALTH_MONITOR_H */
