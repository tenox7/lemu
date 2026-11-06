/*
 * lemu-mcp: the bridge as its own program (see lemu-mcp.c)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "monitor/mcp-common.h"

int main(int argc, char **argv)
{
    return lemu_mcp_main(argc, argv, argv[0]);
}
