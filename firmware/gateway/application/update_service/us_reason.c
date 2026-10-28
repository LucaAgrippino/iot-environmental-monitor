/**
 * @file us_reason.c
 * @brief Wire strings of the OTA result reasons (see us_reason.h).
 */

#include "us_reason.h"

#include <stdint.h>

const char *update_service_reason_string(us_reason_t reason)
{
    static const char *const k_names[] = {
        [US_REASON_NONE] = "",
        [US_REASON_BUSY] = "busy",
        [US_REASON_NOT_OTA_CAPABLE] = "not_ota_capable",
        [US_REASON_NOT_READY] = "not_ready",
        [US_REASON_BAD_COMMAND] = "bad_command",
        [US_REASON_START_TIMEOUT] = "start_timeout",
        [US_REASON_DOWNLOAD_FAILED] = "download_failed",
        [US_REASON_STALLED] = "stalled",
        [US_REASON_STAGING_ERROR] = "staging_error",
        [US_REASON_TOO_LARGE] = "too_large",
        [US_REASON_BAD_HEADER] = "bad_header",
        [US_REASON_WRONG_SLOT] = "wrong_slot",
        [US_REASON_SIG_INVALID] = "sig_invalid",
        [US_REASON_HASH_MISMATCH] = "hash_mismatch",
        [US_REASON_BAD_IMAGE] = "bad_image",
        [US_REASON_CRYPTO] = "crypto",
        [US_REASON_VERIFY_IO] = "verify_io",
        [US_REASON_INSTALL_FAILED] = "install_failed",
        [US_REASON_ACTIVATE_FAILED] = "activate_failed",
        [US_REASON_ROLLBACK] = "rollback",
    };

    if ((uint32_t) reason >= (sizeof(k_names) / sizeof(k_names[0])))
    {
        return "";
    }

    return k_names[reason];
}
