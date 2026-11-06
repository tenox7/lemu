/*
 * MCP monitor: the Model Context Protocol, for AI agents
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MONITOR_MCP_H
#define MONITOR_MCP_H

#include "monitor/monitor.h"
#include "ui/input.h"

#define TYPE_MONITOR_MCP "monitor-mcp"
OBJECT_DECLARE_TYPE(MonitorMCP, MonitorMCPClass, MONITOR_MCP);

void monitor_new_mcp(const char *id, const char *chardev_id, Error **errp);

/*
 * The input context of the MCP tool call running in this coroutine,
 * or NULL when the command was not called through MCP.  Input sent
 * with it is released when the client disconnects or cancels.
 */
QemuInputCtx *mcp_current_input_ctx(void);

/*
 * The debug tools stop and start the VM for every step.  The client
 * whose call does that gets no STOP and RESUME events until the call
 * ends: its result says where the VM is.  Other clients get them all.
 */
void mcp_quiet_runstate_events(void);

/*
 * Text as HMP prints it, for the debug tools: the integer registers of
 * @cpu (info registers without the FPU), and @count instructions at
 * the virtual address @pc.  NULL when QEMU was built without HMP.
 */
char *mcp_cpu_registers(CPUState *cpu);
char *mcp_cpu_disas(CPUState *cpu, uint64_t pc, int count);

/* @words 32-bit words at @addr, as HMP's x (or xp) prints them */
char *mcp_cpu_memory(CPUState *cpu, uint64_t addr, int words, bool physical);

#endif
