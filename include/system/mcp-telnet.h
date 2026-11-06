/*
 * Telnet sessions for MCP clients
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_MCP_TELNET_H
#define SYSTEM_MCP_TELNET_H

#include "chardev/char-tap.h"

typedef enum McpTelnetState {
    MCP_TELNET_CLOSED,
    MCP_TELNET_CONNECTING,
    MCP_TELNET_OPEN,
} McpTelnetState;

typedef struct McpTelnet McpTelnet;

/* A session to @host:@port whose output goes to @tap; not connected yet */
McpTelnet *mcp_telnet_new(const char *host, const char *port, CharTap *tap);
/* Point a closed session somewhere else */
void mcp_telnet_set_address(McpTelnet *t, const char *host, const char *port);
/* "host:port" */
const char *mcp_telnet_address(McpTelnet *t);
/* A session to @host:@port */
bool mcp_telnet_matches(McpTelnet *t, const char *host, const char *port);
McpTelnetState mcp_telnet_state(McpTelnet *t);
/* Why the last connect failed or the session closed; NULL if neither */
const char *mcp_telnet_reason(McpTelnet *t);
/* Bytes that came since the session opened, telnet commands included */
uint64_t mcp_telnet_received(McpTelnet *t);
/* Start connecting; the tap's notifiers run when the state changes */
void mcp_telnet_open(McpTelnet *t);
/* Close the session, or stop connecting; @reason says why */
void mcp_telnet_close(McpTelnet *t, const char *reason);
/* How many bytes mcp_telnet_write takes now; -1 if the session is closed */
int mcp_telnet_room(McpTelnet *t);
/* Send @len typed bytes; @next is the byte that follows them, or -1 */
void mcp_telnet_write(McpTelnet *t, const uint8_t *buf, size_t len, int next);

#endif
