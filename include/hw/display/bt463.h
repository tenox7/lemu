/*
 * Brooktree Bt463 RAMDAC registers
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_DISPLAY_BT463_H
#define HW_DISPLAY_BT463_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BT463 "bt463"
OBJECT_DECLARE_SIMPLE_TYPE(BT463State, BT463)

struct BT463State {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    uint16_t addr;
    uint8_t idx;
    uint32_t wtt_tmp;
    uint32_t wtt[0x10];
    uint8_t ccr[0xC];
    uint8_t reg[0x30];
    uint8_t ram[0x630];
};

#endif
