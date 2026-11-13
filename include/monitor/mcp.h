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
 * A hook that sees every MCP tool call: with @begin before it runs,
 * and without after it ran.  The steps of a steps call show up one by
 * one, with @top false, between the begin and the end of the steps
 * call itself.  It runs in the call's coroutine and may yield there.
 * @args may be NULL.  One hook at a time; NULL removes it.  The
 * recorder (ui/record.c) uses it.
 */
typedef void coroutine_fn McpCallHook(void *opaque, const char *tool,
                                      QDict *args, bool begin, bool top);
void mcp_set_call_hook(McpCallHook *hook, void *opaque);

/* How many MCP tool calls run now, on all MCP monitors */
int mcp_calls_running(void);

/*
 * Between mcp_quiet_runstate(true) and (false), MCP clients get no STOP
 * and RESUME events: for the recorder, which pauses the VM between
 * calls.
 */
void mcp_quiet_runstate(bool quiet);

/*
 * The longest a tool may wait: MCP_WAIT_MAX_MS less what the MCP call
 * running in this coroutine has used, so that the steps of a steps
 * call stay under it in all.  MCP_WAIT_MAX_MS outside MCP.
 */
int64_t mcp_wait_max_ms(void);

/*
 * Text as HMP prints it, for the debug tools: the integer registers of
 * @cpu (info registers without the FPU), and @count instructions at
 * the virtual (or @physical) address @pc.  NULL when QEMU was built
 * without HMP.
 */
char *mcp_cpu_registers(CPUState *cpu);
char *mcp_cpu_disas(CPUState *cpu, uint64_t pc, int count, bool physical);

/* @words 32-bit words at @addr, as HMP's x (or xp) prints them */
char *mcp_cpu_memory(CPUState *cpu, uint64_t addr, int words, bool physical);

#endif
