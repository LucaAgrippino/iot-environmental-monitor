/**
 * @file boot_hw.h
 * @brief Bootloader hardware seam: clocks, console, LED, hand-over (target only).
 *
 * Everything that touches a register other than the flash controller (which belongs to
 * InternalFlashDriver) is behind these functions, in boot_hw_l4.c. The decision logic in
 * boot_core.c never sees them.
 *
 * @see docs/lld/drivers/bootloader.md §2.3, §3.5, §4
 */

#ifndef BOOT_HW_H
#define BOOT_HW_H

#include <stdint.h>

/** Frequency of the system clock while the bootloader runs. */
#define BOOT_HW_FAST_HZ (48000000UL)

/** @brief Raise the system clock from MSI 4 MHz to 48 MHz (2 flash wait states). Saves the state.
 */
void boot_hw_clocks_fast(void);

/** @brief USART1 115200 8N1, TX only, on PB6 (ST-LINK virtual COM port). Call after the clocks. */
void boot_hw_console_init(void);

/** @brief Write a zero-terminated string, polled. */
void boot_hw_console_puts(const char *s);

/** @brief Write a value in decimal. */
void boot_hw_console_dec(uint32_t v);

/** @brief Write "0x" and eight hexadecimal digits. */
void boot_hw_console_hex(uint32_t v);

/** @brief Free-running CPU cycle counter (DWT), enabled on first use. */
uint32_t boot_hw_cycles(void);

/** @brief Number of uncorrectable flash ECC errors the NMI handler has absorbed. */
uint32_t boot_hw_ecc_events(void);

/** @brief Blink LD2 (PB14) forever. A safe fault state: nothing is jumped to. */
void boot_hw_halt(void) __attribute__((noreturn));

/**
 * @brief Hand over to the image at @p slot_base.
 *
 * Drains the console, returns USART1, GPIOB, the DWT and the system clock to their reset state,
 * points SCB->VTOR at the slot, loads MSP from its first word and branches to its second.
 */
void boot_hw_jump(uint32_t slot_base) __attribute__((noreturn));

#endif /* BOOT_HW_H */
