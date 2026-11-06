/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "monitor/mcp.h"
#include "monitor/mcp-common.h"

void monitor_new_mcp(const char *id, const char *chardev_id, Error **errp)
{
    error_setg(errp, "MCP monitors are only available in system emulators");
}

QemuInputCtx *mcp_current_input_ctx(void)
{
    return NULL;
}

int64_t mcp_wait_max_ms(void)
{
    return MCP_WAIT_MAX_MS;
}
