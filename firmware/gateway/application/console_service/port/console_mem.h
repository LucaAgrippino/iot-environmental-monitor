/**
 * @file console_mem.h
 * @brief Placement of the console's large buffers.
 *
 * The two cell grids and the REPL scrollback take about 25 KB. They live in the Gateway's SRAM2
 * bank (zero-initialised, cleared by the startup code) so the 96 KB main bank keeps its room for
 * the heap that mbedTLS allocates from: with the console resident in the main bank only about
 * 33 KB were left and the TLS handshake could not allocate its record buffers.
 */

#ifndef CONSOLE_MEM_H
#define CONSOLE_MEM_H

#ifdef TEST
#define CONSOLE_BIG_BUFFER /* host builds: ordinary static storage */
#else
#define CONSOLE_BIG_BUFFER __attribute__((section(".bss.sram2")))
#endif

#endif /* CONSOLE_MEM_H */
