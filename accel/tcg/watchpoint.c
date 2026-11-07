/*
 * CPU watchpoints
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "exec/breakpoint.h"
#include "exec/cpu-interrupt.h"
#include "exec/page-protection.h"
#include "exec/translation-block.h"
#include "system/tcg.h"
#include "system/replay.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/cpu-ops.h"
#include "hw/core/cpu.h"
#include "internal-common.h"

/*
 * Return true if this watchpoint address matches the specified
 * access (ie the address range covered by the watchpoint overlaps
 * partially or completely with the address range covered by the
 * access).
 */
static inline bool watchpoint_address_matches(CPUWatchpoint *wp,
                                              vaddr addr, vaddr len)
{
    /*
     * We know the lengths are non-zero, but a little caution is
     * required to avoid errors in the case where the range ends
     * exactly at the top of the address space and so addr + len
     * wraps round to zero.
     */
    vaddr wpend = wp->vaddr + wp->len - 1;
    vaddr addrend = addr + len - 1;

    return !(addr > wpend || wp->vaddr > addrend);
}

/* Return flags for watchpoints that match addr + prot.  */
int cpu_watchpoint_address_matches(CPUState *cpu, vaddr addr, vaddr len)
{
    CPUWatchpoint *wp;
    int ret = 0;

    QTAILQ_FOREACH(wp, &cpu->watchpoints, entry) {
        if (!(wp->flags & BP_PHYS) &&
            watchpoint_address_matches(wp, addr, len)) {
            ret |= wp->flags;
        }
    }
    return ret;
}

/* LEMU: the same for the watchpoints on physical addresses */
int cpu_watchpoint_phys_matches(CPUState *cpu, uint64_t paddr, uint64_t len)
{
    CPUWatchpoint *wp;
    int ret = 0;

    QTAILQ_FOREACH(wp, &cpu->watchpoints, entry) {
        if ((wp->flags & BP_PHYS) &&
            watchpoint_address_matches(wp, paddr, len)) {
            ret |= wp->flags;
        }
    }
    return ret;
}

/* Generate a debug exception if a watchpoint has been hit.  */
void cpu_check_watchpoint(CPUState *cpu, vaddr addr, vaddr len,
                          MemTxAttrs attrs, int flags, uintptr_t ra)
{
    /* the physical address is not known: only virtual watchpoints match */
    cpu_check_watchpoint_phys(cpu, addr, UINT64_MAX, len, attrs, flags, ra);
}

/*
 * LEMU: the same, with the physical address of @addr, which watchpoints
 * on physical addresses (BP_PHYS) compare; UINT64_MAX when not known
 */
void cpu_check_watchpoint_phys(CPUState *cpu, vaddr addr, uint64_t paddr,
                               vaddr len, MemTxAttrs attrs, int flags,
                               uintptr_t ra)
{
    CPUWatchpoint *wp;

    assert(tcg_enabled());
    if (cpu->watchpoint_hit) {
        /*
         * We re-entered the check after replacing the TB.
         * Now raise the debug interrupt so that it will
         * trigger after the current instruction.
         */
        bql_lock();
        cpu_interrupt(cpu, CPU_INTERRUPT_DEBUG);
        bql_unlock();
        return;
    }

    if (cpu->cc->tcg_ops->adjust_watchpoint_address) {
        /* this is currently used only by ARM BE32 */
        addr = cpu->cc->tcg_ops->adjust_watchpoint_address(cpu, addr, len);
    }

    assert((flags & ~BP_MEM_ACCESS) == 0);
    QTAILQ_FOREACH(wp, &cpu->watchpoints, entry) {
        int hit_flags = wp->flags & flags;
        bool phys = wp->flags & BP_PHYS;
        vaddr at = phys ? paddr : addr;

        if (hit_flags && (!phys || paddr != UINT64_MAX) &&
            watchpoint_address_matches(wp, at, len)) {
            if (replay_running_debug()) {
                /*
                 * replay_breakpoint reads icount.
                 * Force recompile to succeed, because icount may
                 * be read only at the end of the block.
                 */
                if (!cpu->neg.can_do_io) {
                    /* Force execution of one insn next time.  */
                    cpu->cflags_next_tb = 1 | CF_NOIRQ | curr_cflags(cpu);
                    cpu_loop_exit_restore(cpu, ra);
                }
                /*
                 * Don't process the watchpoints when we are
                 * in a reverse debugging operation.
                 */
                replay_breakpoint();
                return;
            }

            wp->flags |= hit_flags << BP_HIT_SHIFT;
            wp->hitaddr = MAX(at, wp->vaddr);
            wp->hitattrs = attrs;

            if (wp->flags & BP_CPU
                && cpu->cc->tcg_ops->debug_check_watchpoint
                && !cpu->cc->tcg_ops->debug_check_watchpoint(cpu, wp)) {
                wp->flags &= ~BP_WATCHPOINT_HIT;
                continue;
            }
            cpu->watchpoint_hit = wp;

            /* This call also restores vCPU state */
            tb_check_watchpoint(cpu, ra);
            wp->hitpc = cpu->cc->get_pc(cpu);
            if (wp->flags & BP_STOP_BEFORE_ACCESS) {
                cpu->exception_index = EXCP_DEBUG;
                cpu_loop_exit(cpu);
            } else {
                /* Force execution of one insn next time.  */
                cpu->cflags_next_tb = 1 | CF_NOIRQ | curr_cflags(cpu);
                cpu_loop_exit_noexc(cpu);
            }
        } else {
            wp->flags &= ~BP_WATCHPOINT_HIT;
        }
    }
}
