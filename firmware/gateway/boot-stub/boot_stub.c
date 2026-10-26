/**
 * @file boot_stub.c
 * @brief Minimal Slot-A boot stub for the Gateway (B-L475E-IOT01A).
 *
 * Stands in for the real bootloader (FS-O7) so that an image linked for Slot A
 * (STM32L475VGTX_SLOT_A.ld, 0x0800_6000) can run on the board: at reset it checks the first two
 * words of Slot A, points VTOR at the slot, loads the slot's stack pointer and jumps to its
 * reset handler. It reads no metadata and writes nothing; the application finds
 * SCB->VTOR = 0x0800_6000 and FirmwareStore therefore sees that it runs from Slot A.
 *
 * If Slot A does not hold a plausible vector table the stub halts. It is not part of the
 * firmware image and is flashed separately at 0x0800_0000 (scripts/build-slot-a.sh).
 */

#include <stdbool.h>
#include <stdint.h>

#define SLOT_A_BASE (0x08006000UL)
#define SLOT_A_SIZE (480UL * 1024UL)

/* SRAM1 and SRAM2 of the STM32L475VG. The initial stack pointer must point at the top of one. */
#define SRAM1_END (0x20018000UL)
#define SRAM2_END (0x10008000UL)

#define THUMB_BIT (1UL)

/* SCB->VTOR, written as a plain address so that the stub needs no CMSIS header. */
#define SCB_VTOR (*(volatile uint32_t *) 0xE000ED08UL)

extern uint32_t _estack;

/** @brief True if the vector-table pair looks like a Thumb image inside Slot A. */
static bool slot_a_is_plausible(uint32_t sp, uint32_t pc)
{
    const bool sp_ok = (sp == SRAM1_END) || (sp == SRAM2_END);
    const uint32_t entry = pc & ~THUMB_BIT;
    const bool pc_ok =
        ((pc & THUMB_BIT) != 0U) && (entry >= SLOT_A_BASE) && (entry < (SLOT_A_BASE + SLOT_A_SIZE));

    return sp_ok && pc_ok;
}

void boot_reset_handler(void)
{
    const uint32_t *const vectors = (const uint32_t *) SLOT_A_BASE;
    const uint32_t sp = vectors[0];
    const uint32_t pc = vectors[1];

    if (slot_a_is_plausible(sp, pc))
    {
        SCB_VTOR = SLOT_A_BASE;
        __asm volatile("dsb\n\tisb\n\t"
                       "msr msp, %0\n\t"
                       "bx %1\n\t"
                       :
                       : "r"(sp), "r"(pc)
                       : "memory");
    }

    for (;;)
    {
        /* No image in Slot A: nothing to run. */
    }
}

static void halt(void)
{
    for (;;)
    {
    }
}

/* Initial stack pointer, reset, NMI and HardFault; everything else stays at zero. */
__attribute__((section(".isr_vector"), used)) const uint32_t g_boot_vectors[] = {
    (uint32_t) &_estack,
    (uint32_t) boot_reset_handler,
    (uint32_t) halt,
    (uint32_t) halt,
};
