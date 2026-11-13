/*
 * Helpers that the MCP tools in ui/ share
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_UI_MCP_H
#define UI_UI_MCP_H

#include "qapi/qapi-types-mcp.h"
#include "ui/console.h"
#include "ui/input.h"

/*
 * The input context: the MCP call's, or @local for a QMP caller.
 * Declare @local with g_auto(QemuInputCtx) so it ends with the call.
 */
QemuInputCtx *mcp_ctx(QemuInputCtx *local);

/* The console a tool's console and head arguments name */
QemuConsole *mcp_console(QObject *arg, int head, Error **errp);

/* PNG of @img, a capture of a @screen_w x @screen_h screen */
McpImage *mcp_image_encode(pixman_image_t *img, int screen_w, int screen_h,
                           ScreenRegion *region, double scale, Error **errp);

#endif
