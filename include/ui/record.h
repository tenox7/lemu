/*
 * The video recorder of the MCP tool record
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_RECORD_H
#define UI_RECORD_H

/* A recording runs: input tools move and type like a person by default */
bool qemu_record_active(void);

/* The recorder holds the VM paused until the next MCP call (freeze) */
bool qemu_record_frozen(void);

#endif
