/**
 * @file header.h
 * @brief Text of the two header lines (north panel).
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §8
 */

#ifndef HEADER_H
#define HEADER_H

#include <stddef.h>

#include "model.h"

/**
 * @brief Compose both header lines from the live model, with SGR colour sequences.
 *
 * Line 1: product, firmware, uptime, alarm count. Line 2: Wi-Fi, MQTT, Modbus, sensors, time.
 * A service whose data could not be read shows "?" in muted colour.
 */
void header_compose(const console_model_t *m, char *line1, size_t size1, char *line2, size_t size2);

#endif /* HEADER_H */
