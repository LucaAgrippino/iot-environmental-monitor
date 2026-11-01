/**
 * @file console_mem.h
 * @brief Placement of the console's larger buffers.
 *
 * The screen scratch buffer, the line editor and the model snapshot (about 1.3 KB) live in the
 * Gateway's SRAM2 bank (zero-initialised, cleared by the startup code). The 96 KB main bank is
 * nearly full (the mbedTLS arena and task stacks), and SRAM2 has room since the cell grids went.
 */

#ifndef CONSOLE_MEM_H
#define CONSOLE_MEM_H

#ifdef TEST
#define CONSOLE_BIG_BUFFER /* host builds: ordinary static storage */
#else
#define CONSOLE_BIG_BUFFER __attribute__((section(".bss.sram2")))
#endif

#endif /* CONSOLE_MEM_H */
