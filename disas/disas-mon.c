/*
 * Functions related to disassembly from the monitor
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "disas-internal.h"
#include "disas/disas.h"
#include "system/memory.h"
#include "hw/core/cpu.h"
#include "monitor/monitor.h"
#include "monitor/hmp.h"

/*
 * Get LENGTH bytes from info's buffer, at target address memaddr.
 * Transfer them to myaddr.
 */
static int
virtual_read_memory(bfd_vma memaddr, bfd_byte *myaddr, int length,
                    struct disassemble_info *info)
{
    CPUDebug *s = container_of(info, CPUDebug, info);
    int r = cpu_memory_rw_debug(s->cpu, memaddr, myaddr, length, 0);
    return r ? EIO : 0;
}

static int
physical_read_memory(bfd_vma memaddr, bfd_byte *myaddr, int length,
                     struct disassemble_info *info)
{
    CPUDebug *s = container_of(info, CPUDebug, info);
    MemTxResult res;

    res = address_space_read(s->cpu->as, memaddr, MEMTXATTRS_UNSPECIFIED,
                             myaddr, length);
    return res == MEMTX_OK ? 0 : EIO;
}

char *(*monitor_disas_symbol)(uint64_t addr);

/* An address in an instruction, such as where it jumps, and its name */
static void monitor_print_address(bfd_vma addr, struct disassemble_info *info)
{
    g_autofree char *name = monitor_disas_symbol(addr);

    info->fprintf_func(info->stream, "0x%" PRIx64, (uint64_t)addr);
    if (name) {
        info->fprintf_func(info->stream, " <%s>", name);
    }
}

/* Disassembler for the monitor.  */
void monitor_disas(MonitorHMP *hmp, CPUState *cpu, uint64_t pc,
                   int nb_insn, bool is_physical)
{
    int count, i;
    CPUDebug s;
    g_autoptr(GString) ds = g_string_new("");

    disas_initialize_debug_target(&s, cpu);
    s.info.fprintf_func = disas_gstring_printf;
    s.info.stream = (FILE *)ds;  /* abuse this slot */
    s.info.show_opcodes = true;
    if (monitor_disas_symbol && !is_physical) {
        s.info.print_address_func = monitor_print_address;
    }

    if (is_physical) {
        s.info.read_memory_func = physical_read_memory;
    } else {
        s.info.read_memory_func = virtual_read_memory;
    }
    s.info.buffer_vma = pc;

    if (s.info.cap_arch >= 0 &&
        cap_disas_monitor(&s.info, pc, nb_insn,
                          is_physical ? NULL : monitor_disas_symbol)) {
        monitor_puts(MONITOR(hmp), ds->str);
        return;
    }

    if (!s.info.print_insn) {
        monitor_hmp_printf(hmp, "0x%08" PRIx64
                           ": Asm output not supported on this arch\n", pc);
        return;
    }

    for (i = 0; i < nb_insn; i++) {
        g_autofree char *name = monitor_disas_symbol && !is_physical ?
                                monitor_disas_symbol(pc) : NULL;

        g_string_append_printf(ds, "0x%08" PRIx64 "%s%s%s:  ", pc,
                               name ? " <" : "", name ? name : "",
                               name ? ">" : "");
        count = s.info.print_insn(pc, &s.info);
        g_string_append_c(ds, '\n');
        if (count < 0) {
            break;
        }
        pc += count;
    }

    monitor_puts(MONITOR(hmp), ds->str);
}
