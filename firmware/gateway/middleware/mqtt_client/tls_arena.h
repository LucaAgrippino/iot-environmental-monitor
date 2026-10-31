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
 * Decision D-1 option F (WP4, 2026-10-30): 28 KB with the record buffers of
 * mbedtls_config_gateway.h (5 120 in / 2 048 out). Measured on the board against AWS IoT Core
 * (TLS 1.2 mutual auth, RSA-2048 client certificate, full verification): the peak is 20 920 bytes;
 * the handshake fails at 23 552 (`-0x4310`, the RSA private-key operation needs one contiguous
 * block, so the working size is above the peak) and works at 24 576. 28 672 leaves 4 096 bytes
 * above the working floor and 7 752 above the peak. Do not lower it without re-measuring.
 */
#define TLS_ARENA_SIZE_BYTES (28U * 1024U)

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
