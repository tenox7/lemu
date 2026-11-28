/*
 * Brooktree Bt463 RAMDAC registers
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the Previous emulator's ramdac.c (GPL-2.0-or-later).
 *
 * The NeXTstation Color and the Turbo Color have this RAMDAC. Their
 * frame buffers are direct colour, so the palette does not change the
 * picture; software reads and writes the registers, and finds the chip
 * by its ID.
 *
 * Four byte registers: the low and high byte of an address, the register
 * or table entry at that address, and the palette entry at it. Entries
 * of three bytes take three accesses.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/display/bt463.h"
#include "migration/vmstate.h"

#define BT_ADDR_CCR     0x0100
#define BT_ADDR_REG     0x0200
#define BT_ADDR_WTT     0x0300

#define BT463_ID        0x2A
#define BT463_REV       0x0A

#define BT_REG_ID       0x0
#define BT_REG_ISR      0xe
#define BT_REG_OSR      0xf
#define BT_REG_REV      0x20

static void bt463_autoinc(BT463State *s)
{
    if (++s->idx > 2) {
        s->idx = 0;
        s->addr = (s->addr + 1) & 0x0FFF;
    }
}

static void bt463_autoinc_reg(BT463State *s)
{
    s->idx = 0;
    s->addr = (s->addr + 1) & 0x0FFF;
}

static uint8_t bt463_read_reg(BT463State *s)
{
    uint8_t result = 0;

    if ((s->addr & 0xFF) < 0x10) {
        switch (s->addr & 0x0F) {
        case BT_REG_ID:
            result = BT463_ID;
            bt463_autoinc_reg(s);
            break;
        case BT_REG_ISR:
        case BT_REG_OSR:
            result = s->reg[(s->addr & 0x0F) * 3 + s->idx];
            bt463_autoinc(s);
            break;
        default:
            result = s->reg[(s->addr & 0x0F) * 3];
            bt463_autoinc_reg(s);
            break;
        }
    } else if ((s->addr & 0xFF) == BT_REG_REV) {
        result = BT463_REV;
        bt463_autoinc_reg(s);
    }
    return result;
}

static void bt463_write_reg(BT463State *s, uint8_t val)
{
    if ((s->addr & 0xFF) < 0x10) {
        switch (s->addr & 0x0F) {
        case BT_REG_ID:
            bt463_autoinc_reg(s);
            break;
        case BT_REG_ISR:
        case BT_REG_OSR:
            s->reg[(s->addr & 0x0F) * 3 + s->idx] = val;
            bt463_autoinc(s);
            break;
        default:
            s->reg[(s->addr & 0x0F) * 3] = val;
            bt463_autoinc_reg(s);
            break;
        }
    }
}

/* Cursor colours */
static uint8_t bt463_read_ccr(BT463State *s)
{
    uint8_t result = 0;

    if ((s->addr & 0xFF) < 2) {
        result = s->ccr[(s->addr & 1) * 3 + s->idx];
        bt463_autoinc(s);
    }
    return result;
}

static void bt463_write_ccr(BT463State *s, uint8_t val)
{
    if ((s->addr & 0xFF) < 4) {
        s->ccr[(s->addr & 3) * 3 + s->idx] = val;
        bt463_autoinc(s);
    }
}

/* Window type table: 24-bit entries */
static uint8_t bt463_read_wtt(BT463State *s)
{
    uint8_t result = 0;

    if ((s->addr & 0xFF) < 0x10) {
        if (s->idx == 0) {
            s->wtt_tmp = s->wtt[s->addr & 0x0F];
        }
        result = s->wtt_tmp >> (8 * s->idx);
        bt463_autoinc(s);
    }
    return result;
}

static void bt463_write_wtt(BT463State *s, uint8_t val)
{
    if ((s->addr & 0xFF) < 0x10) {
        if (s->idx == 0) {
            s->wtt_tmp = val;
        } else {
            s->wtt_tmp |= (uint32_t)val << (8 * s->idx);
        }
        if (s->idx == 2) {
            s->wtt[s->addr & 0x0F] = s->wtt_tmp;
        }
        bt463_autoinc(s);
    }
}

static uint64_t bt463_read(void *opaque, hwaddr addr, unsigned size)
{
    BT463State *s = opaque;
    uint8_t result = 0;

    switch (addr & 3) {
    case 0:
        s->idx = 0;
        return s->addr & 0xFF;
    case 1:
        s->idx = 0;
        return (s->addr >> 8) & 0x0F;
    case 2:
        switch (s->addr & 0x0F00) {
        case BT_ADDR_CCR:
            return bt463_read_ccr(s);
        case BT_ADDR_REG:
            return bt463_read_reg(s);
        case BT_ADDR_WTT:
            return bt463_read_wtt(s);
        }
        break;
    case 3:
        if (s->addr < 0x210) {
            result = s->ram[s->addr * 3 + s->idx];
        }
        bt463_autoinc(s);
        return result;
    }
    return 0;
}

static void bt463_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    BT463State *s = opaque;

    switch (addr & 3) {
    case 0:
        s->addr = (s->addr & 0x0F00) | (val & 0xFF);
        s->idx = 0;
        break;
    case 1:
        s->addr = (s->addr & 0x00FF) | ((val & 0x0F) << 8);
        s->idx = 0;
        break;
    case 2:
        switch (s->addr & 0x0F00) {
        case BT_ADDR_CCR:
            bt463_write_ccr(s, val);
            break;
        case BT_ADDR_REG:
            bt463_write_reg(s, val);
            break;
        case BT_ADDR_WTT:
            bt463_write_wtt(s, val);
            break;
        }
        break;
    case 3:
        if (s->addr < 0x210) {
            s->ram[s->addr * 3 + s->idx] = val;
        }
        bt463_autoinc(s);
        break;
    }
}

static const MemoryRegionOps bt463_ops = {
    .read = bt463_read,
    .write = bt463_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void bt463_init(Object *obj)
{
    BT463State *s = BT463(obj);

    memory_region_init_io(&s->mem, obj, &bt463_ops, s, "bt463", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mem);
}

static const VMStateDescription bt463_vmstate = {
    .name = "bt463",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(addr, BT463State),
        VMSTATE_UINT8(idx, BT463State),
        VMSTATE_UINT32(wtt_tmp, BT463State),
        VMSTATE_UINT32_ARRAY(wtt, BT463State, 0x10),
        VMSTATE_UINT8_ARRAY(ccr, BT463State, 0xC),
        VMSTATE_UINT8_ARRAY(reg, BT463State, 0x30),
        VMSTATE_UINT8_ARRAY(ram, BT463State, 0x630),
        VMSTATE_END_OF_LIST()
    }
};

static void bt463_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Brooktree Bt463 RAMDAC";
    dc->vmsd = &bt463_vmstate;
    dc->user_creatable = false;
}

static const TypeInfo bt463_info = {
    .name = TYPE_BT463,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BT463State),
    .instance_init = bt463_init,
    .class_init = bt463_class_init,
};

static void bt463_register_types(void)
{
    type_register_static(&bt463_info);
}

type_init(bt463_register_types)
