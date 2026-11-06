/*
 * MCP: what the MCP monitor and the lemu-mcp bridge share
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MONITOR_MCP_COMMON_H
#define MONITOR_MCP_COMMON_H

#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define MCP_PROTOCOL_LATEST     "2025-11-25"

/*
 * The longest a tool waits in one call.  MCP clients give up on a call
 * that sends nothing for a while (Claude Code: 30 minutes for a stdio
 * server).  Calls send progress every 5 s, but only to a client that
 * asks for it, so stay under that.
 */
#define MCP_WAIT_MAX_MS         (25 * 60 * 1000)

/* JSON-RPC error codes */
#define MCP_PARSE_ERROR         -32700
#define MCP_INVALID_REQUEST     -32600
#define MCP_METHOD_NOT_FOUND    -32601
#define MCP_INVALID_PARAMS      -32602
#define MCP_UNSUPPORTED_VERSION -32022

/* The server instructions sent with the initialize reply */
extern const char mcp_instructions[];

/*
 * The hand-written 'hmp', 'qmp' and 'steps' tools.  The bridge lists
 * them too, so that an agent has them even when no VM was up when it
 * started.
 */
extern const char mcp_tool_hmp[];
extern const char mcp_tool_qmp[];
extern const char mcp_tool_steps[];

bool mcp_protocol_supported(const char *v);
QList *mcp_protocol_list(void);

/*
 * A protocol version named in params._meta, as newer, handshake-free
 * revisions of MCP send with every request.
 */
const char *mcp_meta_protocol(QDict *params);

/* Turn \r, \n, \t, \e, \\ and \xHH in @text into bytes; others stay */
GString *mcp_unescape(const char *text);

/*
 * The lemu-mcp bridge: an MCP server on stdin and stdout for all running
 * VMs.  It is its own program, and every system emulator also runs it
 * when its first option is -mcp-bridge, so the kits need no extra file.
 * @cmd is how to start the bridge, for the usage text.  Not on Windows.
 */
int lemu_mcp_main(int argc, char **argv, const char *cmd);

#endif
