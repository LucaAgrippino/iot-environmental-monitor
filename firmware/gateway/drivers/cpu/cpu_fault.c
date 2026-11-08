/**
 * @file cpu_fault.c
 * @brief Cortex-M4 fault handler assembly trampolines for the Gateway.
 *
 * Each handler determines whether the faulting code was using the Main Stack
 * Pointer (MSP) or the Process Stack Pointer (PSP) by inspecting bit 2 of
 * EXC_RETURN in LR, then branches to cpu_fault_entry() with the stacked
 * exception frame in R0 and the vector's panic source in R1.
 *
 * The handlers are naked: with a compiler prologue (at -O0 a "push {r7}") MSP would already be
 * 4 bytes below the frame the hardware stacked and the report would show the stacked LR as the PC
 * (cpu-driver.md CPU-D5).
 *
 * cpu_fault_entry() is defined in cpu.c and saves the frame before calling
 * cpu_panic().  These assembly stubs are intentionally kept in a separate
 * translation unit so the test build can link cpu.c (including
 * cpu_fault_entry and cpu_panic) without pulling in assembly that conflicts
 * with the host tool-chain.
 */

/* --------------------------------------------------------------------- */
/* Cortex-M4 stacked exception frame (hardware pushes in this order):     */
/*   [SP+0]  R0                                                           */
/*   [SP+4]  R1                                                           */
/*   [SP+8]  R2                                                           */
/*   [SP+12] R3                                                           */
/*   [SP+16] R12                                                          */
/*   [SP+20] LR  (return address)                                         */
/*   [SP+24] PC  (faulting instruction)                                   */
/*   [SP+28] xPSR                                                         */
/* --------------------------------------------------------------------- */

/** Source ids equal cpu_panic_source_t: HardFault 1, BusFault 2, MemManage 3, UsageFault 4. */
#define CPU_FAULT_STUB(name, source)                                                               \
    __attribute__((naked)) void name(void)                                                         \
    {                                                                                              \
        __asm volatile("tst   lr, #4      \n" /* EXC_RETURN bit 2: 0 = MSP, 1 = PSP */             \
                       "ite   eq          \n"                                                      \
                       "mrseq r0, msp     \n"                                                      \
                       "mrsne r0, psp     \n"                                                      \
                       "movs  r1, #" #source " \n"                                                 \
                       "b     cpu_fault_entry \n");                                                \
    }

CPU_FAULT_STUB(HardFault_Handler, 1)
CPU_FAULT_STUB(BusFault_Handler, 2)
CPU_FAULT_STUB(MemManage_Handler, 3)
CPU_FAULT_STUB(UsageFault_Handler, 4)
