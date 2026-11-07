/*
 * LEMU: see the accesses to devices' registers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_IO_ACCESS_HOOK_H
#define SYSTEM_IO_ACCESS_HOOK_H

#include "exec/hwaddr.h"

/*
 * Called for each access to a memory region that is not RAM: a device's
 * registers, memory-mapped or in another address space such as the I/O
 * ports, whoever makes it: a CPU's instruction, a device, QEMU.  @addr
 * is the offset in @mr, the region that does the access (not an alias),
 * and @data the value as the one who accessed it sees it: what it wrote,
 * or what it read.  @ok is false when @mr did not take the access, as
 * where nothing is mapped, or for a size it does not do; a read then
 * got what such reads get.  Reads call it after the access, writes
 * before.  It runs in whatever thread made the access, with or without
 * the BQL, so it must not take the BQL, sleep or access memory.
 */
typedef void IoAccessHook(MemoryRegion *mr, hwaddr addr, uint64_t data,
                          unsigned size, bool write, bool ok);

/* Set while a debugger watches such accesses; NULL otherwise */
extern IoAccessHook *io_access_hook;

#endif
