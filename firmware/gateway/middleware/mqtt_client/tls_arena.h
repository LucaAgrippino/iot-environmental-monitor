/**
 * @file tls_arena.h
 * @brief Static memory arena for mbedTLS (REQ-NF-408; MQTT-O1, FS-O4).
 *
 * mbedTLS allocates bignum, key and handshake state at run time. Without this module those
 * allocations come from the C heap. With it every allocation made by the TLS session
 * (MqttClient, CloudPublisherTask) and by the OTA signature check (FirmwareStore verify,
 * UpdateServiceTask) comes from one static buffer, so the linker heap is never touched by mbedTLS.
 *
 * The arena is guarded by a FreeRTOS mutex (threading_alt.h) because two tasks use it.
 *
 * @note See docs/lld/middleware/mqtt-client.md (MQTT-D, MQTT-O1).
 */

#ifndef TLS_ARENA_H
#define TLS_ARENA_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Arena size in bytes.
 *
 * Provisional (WP2): MQTT-O1 estimates 35 to 50 KB for a TLS session, plus the ECDSA verify.
 * Lowered from 48 to 44 KB to make the CloudPublisher image fit the 96 KB RAM region once
 * StoreAndForward, ConfigService and HealthMonitor are linked for real (2 088 bytes over at 48 KB).
 * A session needs about 33 KB for its two 16 KB record buffers alone, so measure before trusting
 * it. To be fixed with a 25 % margin once the peak is measured with a TLS session open and a verify
 * running (tls_arena_peak_bytes()).
 *
 * MEASURED 2026-10-29 (TC-HW-MQTT-014, board, 60 KB arena): the peak with a TLS 1.2 mutual-auth
 * session open against the bring-up broker (RSA-2048 chain) is 46 280 bytes, and the ECDSA verify
 * added 0. 44 KB (45 056) is therefore 1 224 bytes too small: the handshake cannot allocate,
 * mbedTLS reports it as -0x2700 and the board sends an "unknown ca" alert. The value stays here
 * until WP4 decision D-1.
 */
#ifndef TLS_ARENA_SIZE_BYTES /* a bench build may override it (scripts/build-mqtt-client-bench.sh) \
                              */
#define TLS_ARENA_SIZE_BYTES (44U * 1024U)
#endif

/**
 * @brief Install the mutex layer and hand the static arena to mbedTLS.
 *
 * Call once, before the scheduler starts and before any mbedTLS call, and before
 * mqtt_client_create() and firmware_store_create(). A second call does nothing: it must not reset
 * an allocator that is in use.
 *
 * @note Threading: call before the scheduler starts.
 */
void tls_arena_init(void);

/**
 * @brief Report whether tls_arena_init() has run.
 * @return true after tls_arena_init().
 */
bool tls_arena_is_ready(void);

/**
 * @brief Highest number of arena bytes in use at once since boot or the last reset.
 * @return Peak bytes, or 0 before tls_arena_init().
 * @note Threading: any task.
 */
uint32_t tls_arena_peak_bytes(void);

/**
 * @brief Restart the peak measurement from the current use (for a bench measurement).
 * @note Threading: any task.
 */
void tls_arena_reset_peak(void);

#ifdef TEST
/** @brief Test hook: forget that the arena was initialised. */
void tls_arena_reset_for_test(void);
#endif

#endif /* TLS_ARENA_H */
