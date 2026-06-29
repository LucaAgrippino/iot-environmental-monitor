/**
 * @file cpu_fault.c
 * @brief Cortex-M4 fault handler assembly trampolines for the Gateway.
 *
 * Each handler determines whether the faulting code was using the Main Stack
 * Pointer (MSP) or the Process Stack Pointer (PSP) by inspecting bit 2 of
 * EXC_RETURN in LR, then branches to cpu_fault_entry() with the stacked
 * exception frame as the argument.
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

/* --------------------------------------------------------------------- */
/* HardFault handler                                                       */
/* --------------------------------------------------------------------- */

void HardFault_Handler(void)
{
    __asm volatile("tst   lr, #4      \n" /* test EXC_RETURN bit 2: 0 = MSP, 1 = PSP */
                   "ite   eq          \n"
                   "mrseq r0, msp     \n" /* MSP path: pass MSP as frame pointer */
                   "mrsne r0, psp     \n" /* PSP path: pass PSP as frame pointer */
                   "b     cpu_fault_entry \n");
}

/* --------------------------------------------------------------------- */
/* BusFault handler                                                        */
/* --------------------------------------------------------------------- */

void BusFault_Handler(void)
{
    __asm volatile("tst   lr, #4      \n"
                   "ite   eq          \n"
                   "mrseq r0, msp     \n"
                   "mrsne r0, psp     \n"
                   "b     cpu_fault_entry \n");
}

/* --------------------------------------------------------------------- */
/* MemManage handler                                                       */
/* --------------------------------------------------------------------- */

void MemManage_Handler(void)
{
    __asm volatile("tst   lr, #4      \n"
                   "ite   eq          \n"
                   "mrseq r0, msp     \n"
                   "mrsne r0, psp     \n"
                   "b     cpu_fault_entry \n");
}

/* --------------------------------------------------------------------- */
/* UsageFault handler                                                      */
/* --------------------------------------------------------------------- */

void UsageFault_Handler(void)
{
    __asm volatile("tst   lr, #4      \n"
                   "ite   eq          \n"
                   "mrseq r0, msp     \n"
                   "mrsne r0, psp     \n"
                   "b     cpu_fault_entry \n");
}
