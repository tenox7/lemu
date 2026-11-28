/*
 * NeXT Nitro cache controller (NCC)
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the Previous emulator's ncc.c (GPL-2.0-or-later).
 *
 * The 40 MHz Turbo boards (Nitro) have a second level cache. As in
 * Previous it does not cache anything here: software sees the control
 * register, the tags and the cache memory, which the ROM tests.
 *
 * Control register (0x02210000): bit 0 enable, 1 synchronous, 2 tag
 * speed, 3-4 size (64 KB << n), 5-7 revision, 8 compare, 28-31 slot.
 * Tags (0x03E00000, one long word for each 32 bytes): bits 0-1 valid
 * for the two halves, bits 16-31 the address bits compared.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/m68k/next-ncc.h"
#include "migration/vmstate.h"

#define NCC_CCR_READABLE    0xF00001FF
#define NCC_CCR_WRITABLE    0x0000011F
#define NCC_TAG_MASK        0xFFFF0003

static MemTxResult next_ncc_ccr_read(void *opaque, hwaddr addr,
                                     uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    NeXTNCCState *s = opaque;

    if (addr & 3) {
        return MEMTX_DECODE_ERROR;
    }
    *data = s->ccr & NCC_CCR_READABLE;
    return MEMTX_OK;
}

static MemTxResult next_ncc_ccr_write(void *opaque, hwaddr addr,
                                      uint64_t data, unsigned size,
                                      MemTxAttrs attrs)
{
    NeXTNCCState *s = opaque;

    if (addr & 3) {
        return MEMTX_DECODE_ERROR;
    }
    s->ccr = data & NCC_CCR_WRITABLE;
    return MEMTX_OK;
}

/* the control register takes long words only */
static const MemoryRegionOps next_ncc_ccr_ops = {
    .read_with_attrs = next_ncc_ccr_read,
    .write_with_attrs = next_ncc_ccr_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static uint64_t next_ncc_tag_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTNCCState *s = opaque;

    return s->tag[(addr >> 5) % NEXT_NCC_TAGS] & NCC_TAG_MASK;
}

static void next_ncc_tag_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    NeXTNCCState *s = opaque;

    s->tag[(addr >> 5) % NEXT_NCC_TAGS] = val & NCC_TAG_MASK;
}

static const MemoryRegionOps next_ncc_tag_ops = {
    .read = next_ncc_tag_read,
    .write = next_ncc_tag_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void next_ncc_realize(DeviceState *dev, Error **errp)
{
    NeXTNCCState *s = NEXT_NCC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->ccr_mem, OBJECT(dev), &next_ncc_ccr_ops, s,
                          "next.ncc", 0x10000);
    memory_region_init_io(&s->tag_mem, OBJECT(dev), &next_ncc_tag_ops, s,
                          "next.ncc.tags", 0x100000);
    if (!memory_region_init_ram(&s->cache_ram, OBJECT(dev), "next.ncc.cache",
                                NEXT_NCC_CACHE_SIZE, errp)) {
        return;
    }
    /* the 128 KB repeat through 1 MB */
    memory_region_init(&s->cache_mem, OBJECT(dev), "next.ncc.cache-window",
                       0x100000);
    memory_region_add_subregion(&s->cache_mem, 0, &s->cache_ram);
    for (int i = 0; i < 7; i++) {
        memory_region_init_alias(&s->cache_alias[i], OBJECT(dev),
                                 "next.ncc.cache", &s->cache_ram, 0,
                                 NEXT_NCC_CACHE_SIZE);
        memory_region_add_subregion(&s->cache_mem,
                                    (i + 1) * NEXT_NCC_CACHE_SIZE,
                                    &s->cache_alias[i]);
    }
    sysbus_init_mmio(sbd, &s->ccr_mem);
    sysbus_init_mmio(sbd, &s->tag_mem);
    sysbus_init_mmio(sbd, &s->cache_mem);
}

static void next_ncc_reset_hold(Object *obj, ResetType type)
{
    NeXTNCCState *s = NEXT_NCC(obj);

    s->ccr = 0;
    memset(s->tag, 0, sizeof(s->tag));
}

static const VMStateDescription next_ncc_vmstate = {
    .name = "next-ncc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ccr, NeXTNCCState),
        VMSTATE_UINT32_ARRAY(tag, NeXTNCCState, NEXT_NCC_TAGS),
        VMSTATE_END_OF_LIST()
    }
};

static void next_ncc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT Nitro cache controller";
    dc->realize = next_ncc_realize;
    dc->vmsd = &next_ncc_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = next_ncc_reset_hold;
}

static const TypeInfo next_ncc_info = {
    .name = TYPE_NEXT_NCC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTNCCState),
    .class_init = next_ncc_class_init,
};

static void next_ncc_register_types(void)
{
    type_register_static(&next_ncc_info);
}

type_init(next_ncc_register_types)
