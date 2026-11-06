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

/* JSON-RPC error codes */
#define MCP_PARSE_ERROR         -32700
#define MCP_INVALID_REQUEST     -32600
#define MCP_METHOD_NOT_FOUND    -32601
#define MCP_INVALID_PARAMS      -32602
#define MCP_UNSUPPORTED_VERSION -32022

/* The server instructions sent with the initialize reply */
extern const char mcp_instructions[];

/*
 * The hand-written 'hmp' and 'qmp' tools.  The bridge lists them too,
 * so that an agent has them even when no VM was up when it started.
 */
extern const char mcp_tool_hmp[];
extern const char mcp_tool_qmp[];

bool mcp_protocol_supported(const char *v);
QList *mcp_protocol_list(void);

/*
 * A protocol version named in params._meta, as newer, handshake-free
 * revisions of MCP send with every request.
 */
const char *mcp_meta_protocol(QDict *params);

/* Turn \r, \n, \t, \e, \\ and \xHH in @text into bytes; others stay */
GString *mcp_unescape(const char *text);

#endif
