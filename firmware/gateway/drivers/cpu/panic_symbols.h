/**
 * @file panic_symbols.h
 * @brief On-device fault-address symbol resolution (Track B, panic decoder).
 *
 * The table itself (g_panic_symbols/g_panic_symbol_count/g_panic_files) is
 * generated at build time by scripts/gen-panic-symbols.py from
 * firmware/gateway/Debug/gateway.map — see
 * panic_symbols_data.c (gitignored, not this file). panic_symbol_lookup()
 * is hand-written and generic over any sorted table, specifically so it
 * stays unit-testable against a small fixture without needing the real,
 * ~658-entry generated one.
 */

#ifndef PANIC_SYMBOLS_H
#define PANIC_SYMBOLS_H

#include <stdint.h>

typedef struct
{
    uint32_t addr;
    uint32_t size;
    const char *name;
    uint8_t file_index; /**< Index into a caller-supplied file-name table. */
} panic_symbol_t;

/**
 * @brief Find the function symbol containing addr, if any.
 *
 * Binary search — table must be sorted by addr ascending (the generator
 * guarantees this; a hand-built test fixture must too). Deliberately
 * tolerant of addr values outside every known range (including addresses
 * that don't correspond to real code at all, e.g. a corrupted/garbage
 * pointer) — this runs inside a fault handler, where a lookup that itself
 * misbehaves on unexpected input is worse than one that just returns NULL.
 *
 * @param[in] table  Sorted-by-addr symbol table (may be the real generated
 *                    one, or a small test fixture).
 * @param[in] count  Number of entries in table.
 * @param[in] addr   Address to resolve (e.g. a faulting PC or LR).
 * @return Pointer into table for the containing symbol (addr falls within
 *         [entry->addr, entry->addr + entry->size)), or NULL if none
 *         contains it (including table == NULL or count == 0).
 */
const panic_symbol_t *panic_symbol_lookup(const panic_symbol_t *table, uint32_t count,
                                          uint32_t addr);

#endif /* PANIC_SYMBOLS_H */
