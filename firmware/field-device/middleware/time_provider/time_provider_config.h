/**
 * @file time_provider_config.h
 * @brief Compile-time configuration for TimeProvider.
 *
 * TIME_PROVIDER_SANITY_DELTA_S: open item TP-O3 — value TBD pending
 * TimeService LLD companion (GW). Placeholder of 86400 s (24 h) is used
 * until integration testing determines the correct threshold.
 */

#ifndef TIME_PROVIDER_CONFIG_H
#define TIME_PROVIDER_CONFIG_H

#include <stdint.h>

/** BKP0R allocated for sync-flag persistence (LLD-D16). */
#define TIME_PROVIDER_BKUP_REG   (0U)

/** Magic value stored in BKP0R when synchronised (LLD-D16). */
#define TIME_PROVIDER_SYNC_MAGIC (0xA5A55A5AUL)

/**
 * @brief Maximum acceptable |new_epoch - rtc_current| for set_time() (seconds).
 *
 * Applied as a defence-in-depth sanity check when the provider is already
 * SYNCHRONISED. Skipped on first sync (when UNSYNCHRONISED) so that the
 * initial wall-clock set succeeds even after a cold boot with a default RTC.
 *
 * TP-O3: placeholder — confirm threshold at TimeService LLD companion (GW).
 */
#define TIME_PROVIDER_SANITY_DELTA_S (86400UL)

#endif /* TIME_PROVIDER_CONFIG_H */
