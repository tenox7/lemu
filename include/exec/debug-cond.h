/*
 * A condition on the stops of the MCP debug tools
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_DEBUG_COND_H
#define EXEC_DEBUG_COND_H

#include "exec/breakpoint.h"
#include "hw/core/cpu.h"

/*
 * Whether a CPU that gets to a debug breakpoint, touches watched memory
 * or takes a watched exception stops there.  The MCP debug tools set it
 * with the CPU stopped.  The CPU's own thread calls @test at each hit,
 * before the VM stops, so a hit that does not meet it costs no stop.
 */
typedef struct CPUDebugCond CPUDebugCond;
struct CPUDebugCond {
    bool (*test)(CPUState *cpu, CPUDebugCond *cond);
};

static inline bool cpu_debug_cond_ok(CPUState *cpu)
{
    CPUDebugCond *c = cpu->debug_cond;

    return !c || c->test(cpu, c);
}

/*
 * As a CPU gets to a debug breakpoint: whether it stops.  When it hit a
 * watchpoint there, it runs the instruction again; the condition did not
 * hold as it got there, or it would not have run it.
 */
static inline bool cpu_debug_bp_ok(CPUState *cpu)
{
    return !cpu->debug_cond ||
           (!cpu->watchpoint_hit && cpu_debug_cond_ok(cpu));
}

/*
 * As a CPU that touched watched memory raises its debug interrupt,
 * after the access: whether it stops.  If the condition does not hold,
 * the hit is forgotten and the CPU goes on.
 */
static inline bool cpu_debug_watch_ok(CPUState *cpu)
{
    CPUWatchpoint *wp = cpu->watchpoint_hit;

    if (!cpu->debug_cond || !wp || !(wp->flags & BP_GDB) ||
        cpu_debug_cond_ok(cpu)) {
        return true;
    }
    wp->flags &= ~BP_WATCHPOINT_HIT;
    cpu->watchpoint_hit = NULL;
    return false;
}

#endif
