/*
 * LEMU: see the writes to RAM that are not a CPU's own stores
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_RAM_WRITE_HOOK_H
#define SYSTEM_RAM_WRITE_HOOK_H

/*
 * Called after each write to RAM that is not a CPU's own store: a device
 * writing guest memory itself (DMA), QEMU writing it for a CPU (as for a
 * page table), a loader or a debugger.  @addr is the ram_addr_t of the
 * first byte, so the writes that reach the same RAM through an alias, a
 * bus or an IOMMU have the same one.  It runs in whatever thread wrote:
 * the main loop's, a vCPU's or an I/O thread's, with or without the BQL,
 * so it must not take the BQL or sleep.
 */
typedef void RamWriteHook(uint64_t addr, uint64_t len);

/* Set while a debugger watches such writes; NULL otherwise */
extern RamWriteHook *ram_write_hook;

#endif
