/*
 * The exceptions that stop a CPU, for the MCP debug tools
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_EXCP_WATCH_H
#define EXEC_EXCP_WATCH_H

#include "exec/vaddr.h"

#define CPU_EXCP_WATCH_MAX      32      /* numbers in a list */

/*
 * Which exceptions stop the VM right after the CPU takes them, at the
 * first instruction of the guest's handler.  The numbers are the
 * CPU's exception_index.  Set with the CPU stopped, and read by the
 * CPU's own thread, which notes there the exception it stops for.
 */
typedef struct CPUExcpWatch {
    int n_only;                 /* only these, if any */
    int32_t only[CPU_EXCP_WATCH_MAX];
    int n_skip;                 /* never these */
    int32_t skip[CPU_EXCP_WATCH_MAX];

    bool hit;                   /* it stopped for one, as below */
    int32_t number;
    vaddr from;                 /* the instruction that took it */
    vaddr to;                   /* where the CPU went */
} CPUExcpWatch;

/* Whether exception @excp stops the CPU */
static inline bool cpu_excp_watch_match(const CPUExcpWatch *w, int32_t excp)
{
    int i;

    for (i = 0; i < w->n_skip; i++) {
        if (w->skip[i] == excp) {
            return false;
        }
    }
    for (i = 0; i < w->n_only; i++) {
        if (w->only[i] == excp) {
            return true;
        }
    }
    return !w->n_only;
}

#endif
