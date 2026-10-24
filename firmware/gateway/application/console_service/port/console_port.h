/**
 * @file console_port.h
 * @brief Platform hooks for ConsoleService (the only code that touches the MCU).
 *
 * Implemented by port/serial_vcp.c on the Gateway and by spies in host tests, so core/ stays
 * board-independent.
 */

#ifndef CONSOLE_PORT_H
#define CONSOLE_PORT_H

#include <stdint.h>

/** @brief Read the 96-bit unique device ID. */
void console_port_uid(uint32_t uid[3]);

/** @brief Reset the MCU. Does not return on target. */
void console_port_reset(void);

/** @brief Milliseconds since boot (monotonic). */
uint32_t console_port_millis(void);

#endif /* CONSOLE_PORT_H */
