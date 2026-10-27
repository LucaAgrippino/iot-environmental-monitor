/**
 * @file iupdate_service.h
 * @brief IUpdateService — command and chunk intake (CloudPublisherTask).
 *
 * Vtable only.
 *
 * @see docs/lld/application/update-service-lld.md §7.2
 */

#ifndef IUPDATE_SERVICE_H
#define IUPDATE_SERVICE_H

#include <stdint.h>

#include "update_service_types.h"

typedef struct
{
    /** Copy the command into the 1-deep inbox and notify the task. Never blocks. */
    update_service_err_t (*handle_command)(update_service_t *self, const us_ota_command_t *cmd);

    /** Copy one chunk into the slot if it is the one requested; else drop. Never blocks. */
    update_service_err_t (*on_chunk)(update_service_t *self, uint32_t job, uint32_t offset,
                                     const uint8_t *data, uint32_t len);
} iupdate_service_t;

#endif /* IUPDATE_SERVICE_H */
