/*
 * NeXT Nitro cache controller (NCC)
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_M68K_NEXT_NCC_H
#define HW_M68K_NEXT_NCC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_NEXT_NCC "next-ncc"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTNCCState, NEXT_NCC)

#define NEXT_NCC_BASE        0x02210000
#define NEXT_NCC_TAG_BASE    0x03E00000
#define NEXT_NCC_CACHE_BASE  0x03F00000

#define NEXT_NCC_CACHE_SIZE  (128 * 1024)
#define NEXT_NCC_TAGS        (NEXT_NCC_CACHE_SIZE >> 5)

struct NeXTNCCState {
    SysBusDevice parent_obj;

    MemoryRegion ccr_mem;       /* 0: the control register */
    MemoryRegion tag_mem;       /* 1: the tags, 1 MB */
    MemoryRegion cache_mem;     /* 2: the cache memory, 1 MB */
    MemoryRegion cache_ram;
    MemoryRegion cache_alias[7];

    uint32_t ccr;
    uint32_t tag[NEXT_NCC_TAGS];
};

#endif
