/*
 * NeXT Turbo memory controller (TMC) and its ADB interface
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_M68K_NEXT_TMC_H
#define HW_M68K_NEXT_TMC_H

#include "hw/core/sysbus.h"
#include "hw/input/adb.h"
#include "qom/object.h"

#define TYPE_NEXT_TMC "next-tmc"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTTMCState, NEXT_TMC)

enum {
    NEXT_TMC_IRQ_DISK,      /* video retrace and ADB */
    NEXT_TMC_IRQ_NMI,
    NEXT_TMC_IRQ_POWER,     /* the ADB power key, to the clock chip */
    NEXT_TMC_NUM_IRQS
};

/* The machine type in the Turbo SCR1 */
#define NEXT_TMC_STATION        0x4
#define NEXT_TMC_STATION_COLOR  0x5
#define NEXT_TMC_CUBE           0x8

struct NeXTTMCState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq[NEXT_TMC_NUM_IRQS];
    ADBBusState adb_bus;

    uint8_t machine_type;
    bool station;

    uint32_t scr1;
    uint32_t control;
    uint32_t nmi;
    uint32_t horizontal;
    uint32_t vertical;
    uint32_t video_intr;

    /* ADB interface */
    uint32_t adb_intstatus;
    uint32_t adb_intmask;
    uint32_t adb_config;
    uint32_t adb_status;
    uint32_t adb_command;
    uint32_t adb_bitcount;
    uint32_t adb_data0;
    uint32_t adb_data1;
    bool power_down;
};

#endif
