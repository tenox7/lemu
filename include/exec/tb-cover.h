/*
 * The blocks of translated code a CPU ran, and how often, for the MCP
 * debug tools
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_TB_COVER_H
#define EXEC_TB_COVER_H

#include "exec/vaddr.h"

/* A key for code that is not in RAM or ROM: its address, marked */
#define TB_COVER_NOT_RAM        (1ULL << 63)

#define TB_COVER_SLOTS          (1u << 14)  /* to begin with */
#define TB_COVER_SLOTS_MAX      (1u << 20)

typedef struct CPUTBCoverEntry {
    uint64_t key;               /* where its code is in RAM (ram_addr_t),
                                   or its pc | TB_COVER_NOT_RAM */
    vaddr pc;                   /* where the CPU entered it */
    uint32_t size;              /* bytes of guest code */
    uint32_t runs;              /* times the CPU entered it; 0: free */
} CPUTBCoverEntry;

/*
 * A table of the blocks a CPU entered, by where their code is, written by
 * the CPU's own thread and read with the CPU stopped.  While a CPU has
 * one, its blocks don't chain, so each block it enters passes where it
 * is noted.
 */
typedef struct CPUTBCover {
    uint32_t mask;              /* slots, less 1; slots are a power of 2 */
    uint32_t used;
    uint64_t lost;              /* entries it had no room for */
    CPUTBCoverEntry *e;
} CPUTBCover;

static inline CPUTBCover *cpu_tb_cover_new(void)
{
    CPUTBCover *c = g_new0(CPUTBCover, 1);

    c->mask = TB_COVER_SLOTS - 1;
    c->e = g_new0(CPUTBCoverEntry, TB_COVER_SLOTS);
    return c;
}

static inline void cpu_tb_cover_free(CPUTBCover *c)
{
    if (c) {
        g_free(c->e);
        g_free(c);
    }
}

/* The slot that holds @key, or the free one where it would go */
static inline CPUTBCoverEntry *cpu_tb_cover_slot(CPUTBCover *c, uint64_t key)
{
    uint32_t i = ((key * 0x9e3779b97f4a7c15ull) >> 32) & c->mask;

    while (c->e[i].runs && c->e[i].key != key) {
        i = (i + 1) & c->mask;
    }
    return &c->e[i];
}

/* Twice the slots; false when it has the most it may have */
static inline bool cpu_tb_cover_grow(CPUTBCover *c)
{
    CPUTBCoverEntry *old = c->e;
    uint32_t n = c->mask + 1, i;

    if (n >= TB_COVER_SLOTS_MAX) {
        return false;
    }
    c->e = g_new0(CPUTBCoverEntry, n * 2);
    c->mask = n * 2 - 1;
    for (i = 0; i < n; i++) {
        if (old[i].runs) {
            *cpu_tb_cover_slot(c, old[i].key) = old[i];
        }
    }
    g_free(old);
    return true;
}

/*
 * Note that the CPU enters the block at @pc, @size bytes long, whose
 * code is at @ram in RAM or ROM (-1: elsewhere)
 */
static inline void cpu_tb_cover_add(CPUTBCover *c, vaddr pc, uint64_t ram,
                                    uint32_t size)
{
    uint64_t key = ram != (uint64_t)-1 ? ram : pc | TB_COVER_NOT_RAM;
    CPUTBCoverEntry *e = cpu_tb_cover_slot(c, key);

    if (e->runs) {
        if (e->runs < UINT32_MAX) {
            e->runs++;
        }
        e->size = MAX(e->size, size);
        return;
    }
    /* at most half full, so that lookups stay short */
    if (c->used >= (c->mask + 1) / 2) {
        if (!cpu_tb_cover_grow(c)) {
            c->lost++;
            return;
        }
        e = cpu_tb_cover_slot(c, key);
    }
    e->key = key;
    e->pc = pc;
    e->size = size;
    e->runs = 1;
    c->used++;
}

#endif
