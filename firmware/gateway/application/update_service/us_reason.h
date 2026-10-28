/**
 * @file us_reason.h
 * @brief Wire strings of the OTA result reasons (update-service-lld.md §8.4).
 *
 * Kept in a file of its own, with no dependency beyond the types header, so that CloudPublisher
 * (which serialises the reports) can name a reason without linking UpdateService.
 */

#ifndef US_REASON_H
#define US_REASON_H

#include "update_service_types.h"

/** @brief Wire string of a reason (§8.4); "" for US_REASON_NONE or an unknown value. */
const char *update_service_reason_string(us_reason_t reason);

#endif /* US_REASON_H */
