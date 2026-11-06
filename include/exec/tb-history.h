/*
 * The last blocks of translated code a CPU ran, for the MCP debug tools
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_TB_HISTORY_H
#define EXEC_TB_HISTORY_H

#include "exec/vaddr.h"

typedef struct CPUTBHistoryEntry {
    vaddr pc;
    uint32_t size;              /* bytes of guest code */
    uint32_t repeat;            /* times it ran in a row */
} CPUTBHistoryEntry;

/*
 * A ring of blocks, written by the CPU's own thread and read with the
 * CPU stopped.  While a CPU has one, its blocks don't chain, so each
 * block it enters passes where it is noted.
 */
typedef struct CPUTBHistory {
    uint32_t size;              /* entries, a power of 2 */
    uint32_t next;              /* the entry to write next */
    uint64_t count;             /* entries written */
    CPUTBHistoryEntry e[];
} CPUTBHistory;

/* Note that the CPU enters the block at @pc, @size bytes long */
static inline void cpu_tb_history_add(CPUTBHistory *h, vaddr pc,
                                      uint32_t size)
{
    CPUTBHistoryEntry *e = &h->e[(h->next - 1) & (h->size - 1)];

    if (h->count && e->pc == pc && e->size == size) {
        if (e->repeat < UINT32_MAX) {
            e->repeat++;
        }
        return;
    }
    e = &h->e[h->next];
    e->pc = pc;
    e->size = size;
    e->repeat = 1;
    h->next = (h->next + 1) & (h->size - 1);
    h->count++;
}

#endif
