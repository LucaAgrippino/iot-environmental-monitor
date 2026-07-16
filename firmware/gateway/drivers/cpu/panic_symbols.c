/**
 * @file panic_symbols.c
 * @brief Binary-search implementation for panic_symbols.h.
 *
 * Filename matches the header deliberately (Ceedling auto-links a test's
 * #include "panic_symbols.h" to a same-named .c) — the generated data
 * table this searches lives in a separately-named file,
 * panic_symbols_data.c (gitignored), so the two never collide.
 */

#include "panic_symbols.h"

const panic_symbol_t *panic_symbol_lookup(const panic_symbol_t *table, uint32_t count,
                                          uint32_t addr)
{
    if ((table == NULL) || (count == 0u))
    {
        return NULL;
    }

    /* Binary search for the rightmost entry with addr_field <= addr (the
     * "predecessor" search) — table must be sorted by addr ascending. */
    uint32_t lo = 0u;
    uint32_t hi = count;
    while (lo < hi)
    {
        uint32_t mid = lo + ((hi - lo) / 2u);
        if (table[mid].addr <= addr)
        {
            lo = mid + 1u;
        }
        else
        {
            hi = mid;
        }
    }

    if (lo == 0u)
    {
        return NULL; /* addr precedes every entry */
    }

    const panic_symbol_t *candidate = &table[lo - 1u];
    if ((addr >= candidate->addr) && (addr < (candidate->addr + candidate->size)))
    {
        return candidate;
    }
    return NULL; /* addr falls in a gap between functions, or past the last one */
}
