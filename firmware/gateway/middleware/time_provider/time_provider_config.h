/**
 * @file time_provider_config.h
 * @brief Compile-time configuration for TimeProvider (Gateway).
 *
 * TIME_PROVIDER_SANITY_DELTA_S: open item TP-O3 — value to be confirmed by the
 * TimeService LLD companion (GW). The 24 h placeholder is kept until then.
 */

#ifndef TIME_PROVIDER_CONFIG_H
#define TIME_PROVIDER_CONFIG_H

#include <stdint.h>

/** BKP0R allocated for sync-flag persistence (LLD-D16). */
#define TIME_PROVIDER_BKUP_REG (0U)

/** Magic value stored in BKP0R when synchronised (LLD-D16). */
#define TIME_PROVIDER_SYNC_MAGIC (0xA5A55A5AUL)

/** Value written to BKP0R when the provider is marked unsynchronised. */
#define TIME_PROVIDER_SYNC_CLEARED (0x00000000UL)

/**
 * @brief Maximum acceptable |new_epoch - rtc_current| for set_time() (seconds).
 *
 * Applied as a defence-in-depth check when the provider is already
 * SYNCHRONISED. Skipped on first sync, so the initial wall-clock set succeeds
 * even after a cold boot with the RTC at its default date.
 *
 * TP-O3: placeholder — confirm the threshold at the TimeService LLD companion.
 */
#define TIME_PROVIDER_SANITY_DELTA_S (86400UL)

/**
 * @brief Earliest epoch the RTC can hold: 2000-01-01 00:00:00 UTC.
 *
 * The RTC stores a two-digit year offset from 2000, and rtc_set_time() does not
 * range-check it, so TimeProvider rejects anything outside this window rather
 * than let the driver write a wrapped year.
 */
#define TIME_PROVIDER_EPOCH_MIN (946684800UL)

/** @brief Latest epoch the RTC can hold: 2099-12-31 23:59:59 UTC. */
#define TIME_PROVIDER_EPOCH_MAX (4102444799UL)

#endif /* TIME_PROVIDER_CONFIG_H */
