/*
 * NeXT Turbo memory controller (TMC) and its ADB interface
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the Previous emulator's tmc.c and adb.c (GPL-2.0-or-later).
 *
 * The Turbo machines have the TMC at 0x02200000: a second system control
 * register with the machine type, the memory and video timing, an NMI
 * register and the video retrace interrupt. Its ADB interface, at
 * 0x02208000, sends one ADB command at a time to the keyboard and mouse,
 * which are QEMU's ADB devices here: the command register has the same
 * layout as an ADB command byte, and the data registers hold up to eight
 * bytes. The interface's interrupt shares the video one.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/m68k/next-tmc.h"
#include "migration/vmstate.h"

#define TMC_ADB_BASE        0x8000
#define TMC_ADB_SIZE        0x100

/* registers */
#define TMC_SCR1            0x00
#define TMC_CONTROL         0x10
#define TMC_NMI             0x20
#define TMC_VIDEO_INTR      0x80
#define TMC_HORIZONTAL      0x88
#define TMC_VERTICAL        0x8c

#define TURBOSCR_FMASK      0x0FFF0F08

#define TMC_VI_INTERRUPT    (0x01u << 24)
#define TMC_VI_INT_MASK     (0x02u << 24)
#define TMC_VI_ENABLE       (0x04u << 24)

/* ADB registers */
#define ADB_INTSTATUS       0x00
#define ADB_INTMASK         0x08
#define ADB_SETINT          0x10
#define ADB_CONFIG          0x18
#define ADB_CTRL            0x20
#define ADB_STATUS          0x28
#define ADB_CMD             0x30
#define ADB_COUNT           0x38
#define ADB_DATA0           0x80
#define ADB_DATA1           0x88

#define ADB_INT_REJECT      0x01
#define ADB_INT_POLLSTOP    0x02
#define ADB_INT_ACCESS      0x04
#define ADB_INT_RESET       0x08

#define ADB_CTRL_EN_POLL    0x01
#define ADB_CTRL_DIS_POLL   0x02
#define ADB_CTRL_XMIT_CMD   0x04
#define ADB_CTRL_RESET_ADB  0x08

#define ADB_STAT_REQUEST    0x02
#define ADB_STAT_TIMEOUT    0x04
#define ADB_STAT_DATAPEND   0x08
#define ADB_STAT_RESET      0x10
#define ADB_STAT_ACCESS     0x20
#define ADB_STAT_POLL_EN    0x40

#define ADB_CNT_MASK        0x7F

/* ADB keyboards send this for the power key */
#define ADB_POWER_KEY       0x7f

static void next_tmc_update_irq(NeXTTMCState *s)
{
    bool video = s->video_intr & TMC_VI_INTERRUPT;
    bool adb = s->adb_intstatus & s->adb_intmask;

    qemu_set_irq(s->irq[NEXT_TMC_IRQ_DISK], video || adb);
}

/* ---- ADB ---- */

static void next_tmc_adb_interrupt(NeXTTMCState *s, uint32_t intr)
{
    if (s->adb_status & ADB_STAT_POLL_EN) {
        intr &= ~ADB_INT_ACCESS;
    }
    s->adb_intstatus |= intr;
    next_tmc_update_irq(s);
}

static void next_tmc_adb_timeout(NeXTTMCState *s)
{
    s->adb_status |= ADB_STAT_TIMEOUT;
    next_tmc_adb_interrupt(s, ADB_INT_ACCESS);
}

static bool next_tmc_adb_pending(NeXTTMCState *s)
{
    ADBBusState *bus = &s->adb_bus;

    for (int i = 0; i < bus->nb_devices; i++) {
        ADBDevice *d = bus->devices[i];

        if (ADB_DEVICE_GET_CLASS(d)->devhasdata(d)) {
            return true;
        }
    }
    return false;
}

/*
 * QEMU's ADB keyboard sends the power key as 0x7f 0x7f (down) and
 * 0xff 0xff (up): it goes to the clock chip, as in Previous, and not to
 * the software. Returns the length of what is left.
 */
static int next_tmc_adb_power(NeXTTMCState *s, const uint8_t *buf, int len)
{
    bool down;

    if (len != 2 || (buf[0] & 0x7f) != ADB_POWER_KEY ||
        (buf[1] & 0x7f) != ADB_POWER_KEY) {
        return len;
    }
    down = !(buf[0] & 0x80);
    if (down != s->power_down) {
        s->power_down = down;
        qemu_set_irq(s->irq[NEXT_TMC_IRQ_POWER], down);
    }
    return 0;
}

static bool next_tmc_adb_is_keyboard(NeXTTMCState *s, uint8_t addr)
{
    ADBBusState *bus = &s->adb_bus;

    for (int i = 0; i < bus->nb_devices; i++) {
        if (bus->devices[i]->devaddr == addr) {
            return object_dynamic_cast(OBJECT(bus->devices[i]),
                                       TYPE_ADB_KEYBOARD);
        }
    }
    return false;
}

static void next_tmc_adb_command(NeXTTMCState *s)
{
    uint8_t cmd = s->adb_command & 0xff;
    uint8_t in[1 + 8], out[16];     /* the command and up to 8 bytes */
    int len = 1, olen;

    in[0] = cmd;
    if ((cmd & 0x0c) == 0x08) {     /* listen: the data goes with it */
        int n = MIN(s->adb_bitcount >> 3, 8);

        for (int i = 0; i < n; i++) {
            uint32_t w = i < 4 ? s->adb_data0 : s->adb_data1;

            in[1 + i] = w >> (24 - 8 * (i & 3));
        }
        len += n;
        next_tmc_adb_interrupt(s, ADB_INT_ACCESS);
    }

    adb_autopoll_block(&s->adb_bus);
    olen = adb_request(&s->adb_bus, out, in, len);
    adb_autopoll_unblock(&s->adb_bus);

    if ((cmd & 0x0f) == 0x0c && olen > 0 &&
        next_tmc_adb_is_keyboard(s, cmd >> 4)) {
        olen = next_tmc_adb_power(s, out, olen);
    }

    if ((cmd & 0x0c) == 0x0c && olen > 0) {   /* talk: data back */
        uint8_t data[8] = { 0 };

        olen = MIN(olen, 8);
        memcpy(data, out, olen);
        s->adb_data0 = ldl_be_p(data);
        s->adb_data1 = ldl_be_p(data + 4);
        s->adb_bitcount = olen << 3;
        s->adb_status |= ADB_STAT_DATAPEND;
        next_tmc_adb_interrupt(s, ADB_INT_ACCESS);
    } else if ((cmd & 0x0c) != 0x08) {
        /* reset, flush, or a talk without an answer */
        next_tmc_adb_timeout(s);
    } else if (olen < 0) {
        next_tmc_adb_timeout(s);
    }

    if (next_tmc_adb_pending(s)) {
        s->adb_status |= ADB_STAT_REQUEST;
        next_tmc_adb_interrupt(s, ADB_INT_ACCESS);
    }
}

static void next_tmc_adb_reset(NeXTTMCState *s)
{
    uint8_t cmd = 0, out[16];

    adb_autopoll_block(&s->adb_bus);
    adb_request(&s->adb_bus, out, &cmd, 1);     /* bus reset */
    adb_autopoll_unblock(&s->adb_bus);
}

static MemTxResult next_tmc_adb_read(NeXTTMCState *s, hwaddr reg,
                                     uint32_t *val)
{
    switch (reg) {
    case ADB_INTSTATUS:
        *val = s->adb_intstatus;
        break;
    case ADB_INTMASK:
        *val = s->adb_intmask;
        break;
    case ADB_CONFIG:
        *val = s->adb_config;
        break;
    case ADB_STATUS:
        *val = s->adb_status;
        break;
    case ADB_CMD:
        *val = s->adb_command;
        break;
    case ADB_COUNT:
        *val = s->adb_bitcount & ADB_CNT_MASK;
        break;
    case ADB_DATA0:
        *val = s->adb_data0;
        break;
    case ADB_DATA1:
        *val = s->adb_data1;
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult next_tmc_adb_write(NeXTTMCState *s, hwaddr reg,
                                      uint32_t val)
{
    switch (reg) {
    case ADB_INTSTATUS:
        s->adb_intstatus &= ~val;
        next_tmc_update_irq(s);
        break;
    case ADB_INTMASK:
        s->adb_intmask = val;
        next_tmc_update_irq(s);
        break;
    case ADB_SETINT:
        s->adb_intstatus |= val;
        next_tmc_update_irq(s);
        break;
    case ADB_CONFIG:
        s->adb_config = val;
        break;
    case ADB_CTRL:
        if (val & ADB_CTRL_RESET_ADB) {
            next_tmc_adb_reset(s);
            s->adb_status &= ~ADB_STAT_POLL_EN;
            next_tmc_adb_interrupt(s, ADB_INT_RESET);
        }
        if (val & ADB_CTRL_XMIT_CMD) {
            if (s->adb_status &
                (ADB_STAT_RESET | ADB_STAT_POLL_EN | ADB_STAT_ACCESS)) {
                next_tmc_adb_interrupt(s, ADB_INT_REJECT);
            } else {
                s->adb_status &= ~(ADB_STAT_DATAPEND | ADB_STAT_TIMEOUT |
                                   ADB_STAT_REQUEST);
                next_tmc_adb_command(s);
            }
        }
        if (val & ADB_CTRL_DIS_POLL) {
            s->adb_status &= ~ADB_STAT_POLL_EN;
        }
        if ((val & ADB_CTRL_EN_POLL) &&
            (s->adb_status & (ADB_STAT_RESET | ADB_STAT_ACCESS)) &&
            !(s->adb_status & ADB_STAT_POLL_EN)) {
            /* polling does not work on the real hardware either */
            next_tmc_adb_interrupt(s, ADB_INT_REJECT);
        }
        break;
    case ADB_CMD:
        s->adb_command = val;
        break;
    case ADB_COUNT:
        s->adb_bitcount = val & ADB_CNT_MASK;
        break;
    case ADB_DATA0:
        s->adb_data0 = val;
        break;
    case ADB_DATA1:
        s->adb_data1 = val;
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

/* ---- TMC registers ---- */

static uint32_t next_tmc_reg_read(NeXTTMCState *s, hwaddr reg)
{
    switch (reg) {
    case TMC_SCR1:
        return s->scr1;
    case TMC_CONTROL:
        return s->control;
    case TMC_NMI:
        return s->nmi;
    case TMC_VIDEO_INTR:
        return s->video_intr;
    case TMC_HORIZONTAL:
        return s->horizontal;
    case TMC_VERTICAL:
        return s->vertical;
    }
    return 0;
}

static void next_tmc_reg_write(NeXTTMCState *s, hwaddr reg, uint32_t val,
                               uint32_t mask)
{
    switch (reg) {
    case TMC_CONTROL:
        val &= ~0x00000400;    /* no parity memory */
        s->control = (s->control & ~mask) | (val & mask);
        break;
    case TMC_NMI:
        if (mask & 0xff) {
            s->nmi = val & 1;
            qemu_set_irq(s->irq[NEXT_TMC_IRQ_NMI], s->nmi);
        }
        break;
    case TMC_VIDEO_INTR:
        if (mask & 0xff000000) {
            s->video_intr = val & 0xff000000;
            if (s->video_intr & TMC_VI_INTERRUPT) {
                s->video_intr &= ~TMC_VI_INTERRUPT;
            }
            next_tmc_update_irq(s);
        }
        break;
    case TMC_HORIZONTAL:
        s->horizontal = (s->horizontal & ~mask) | (val & mask);
        break;
    case TMC_VERTICAL:
        s->vertical = (s->vertical & ~mask) | (val & mask);
        break;
    }
}

static int next_tmc_shift(hwaddr addr, unsigned size)
{
    return (4 - (addr & 3) - size) * 8;
}

static MemTxResult next_tmc_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    NeXTTMCState *s = opaque;
    uint32_t val = 0;

    if (addr >= TMC_ADB_BASE && addr < TMC_ADB_BASE + TMC_ADB_SIZE) {
        MemTxResult r = next_tmc_adb_read(s, (addr - TMC_ADB_BASE) & ~3,
                                          &val);
        if (r != MEMTX_OK) {
            return r;
        }
    } else {
        val = next_tmc_reg_read(s, addr & ~3);
    }
    *data = extract32(val, next_tmc_shift(addr, size), size * 8);
    return MEMTX_OK;
}

static MemTxResult next_tmc_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size, MemTxAttrs attrs)
{
    NeXTTMCState *s = opaque;
    int shift = next_tmc_shift(addr, size);

    if (addr >= TMC_ADB_BASE && addr < TMC_ADB_BASE + TMC_ADB_SIZE) {
        /* the ADB interface takes long words only */
        if (size != 4) {
            return MEMTX_DECODE_ERROR;
        }
        return next_tmc_adb_write(s, addr - TMC_ADB_BASE, data);
    }
    next_tmc_reg_write(s, addr & ~3, (uint32_t)data << shift,
                       MAKE_64BIT_MASK(shift, size * 8));
    return MEMTX_OK;
}

static const MemoryRegionOps next_tmc_ops = {
    .read_with_attrs = next_tmc_read,
    .write_with_attrs = next_tmc_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* The vertical retrace, from the system control */
static void next_tmc_vbl(void *opaque, int n, int level)
{
    NeXTTMCState *s = opaque;

    if (level && (s->video_intr & TMC_VI_INT_MASK)) {
        s->video_intr |= TMC_VI_INTERRUPT;
        next_tmc_update_irq(s);
    }
}

static void next_tmc_reset_hold(Object *obj, ResetType type)
{
    NeXTTMCState *s = NEXT_TMC(obj);

    /* 33 MHz, 60 ns main memory */
    s->scr1 = 0xF0 | 0x07 | (s->machine_type << 12) | TURBOSCR_FMASK;
    if (s->station) {
        s->scr1 |= 0xF0000000;
    }
    s->video_intr = 0;
    s->horizontal = (0x18u << 25) | (0x20 << 19) | (0x48 << 12) | 0x118;
    s->vertical = (0x08u << 25) | (0x08 << 19) | (0x30 << 12) | 0x340;
    s->control = 0x0D17038F;
    s->nmi = 0;

    s->adb_intstatus = 0;
    s->adb_intmask = 0;
    s->adb_config = 0;
    s->adb_command = 0;
    s->adb_status = 0;
    s->adb_bitcount = 0;
    s->adb_data0 = 0;
    s->adb_data1 = 0;
    s->power_down = false;
}

static void next_tmc_init(Object *obj)
{
    NeXTTMCState *s = NEXT_TMC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mem, obj, &next_tmc_ops, s, "next.tmc",
                          0x10000);
    sysbus_init_mmio(sbd, &s->mem);
    for (int i = 0; i < NEXT_TMC_NUM_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), next_tmc_vbl, "vbl", 1);
    qbus_init(&s->adb_bus, sizeof(s->adb_bus), TYPE_ADB_BUS, DEVICE(obj),
              "adb.0");
}

static const VMStateDescription next_tmc_vmstate = {
    .name = "next-tmc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(scr1, NeXTTMCState),
        VMSTATE_UINT32(control, NeXTTMCState),
        VMSTATE_UINT32(nmi, NeXTTMCState),
        VMSTATE_UINT32(horizontal, NeXTTMCState),
        VMSTATE_UINT32(vertical, NeXTTMCState),
        VMSTATE_UINT32(video_intr, NeXTTMCState),
        VMSTATE_UINT32(adb_intstatus, NeXTTMCState),
        VMSTATE_UINT32(adb_intmask, NeXTTMCState),
        VMSTATE_UINT32(adb_config, NeXTTMCState),
        VMSTATE_UINT32(adb_status, NeXTTMCState),
        VMSTATE_UINT32(adb_command, NeXTTMCState),
        VMSTATE_UINT32(adb_bitcount, NeXTTMCState),
        VMSTATE_UINT32(adb_data0, NeXTTMCState),
        VMSTATE_UINT32(adb_data1, NeXTTMCState),
        VMSTATE_BOOL(power_down, NeXTTMCState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property next_tmc_properties[] = {
    DEFINE_PROP_UINT8("machine-type", NeXTTMCState, machine_type,
                      NEXT_TMC_STATION),
    DEFINE_PROP_BOOL("station", NeXTTMCState, station, true),
};

static void next_tmc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT Turbo memory controller";
    dc->vmsd = &next_tmc_vmstate;
    dc->user_creatable = false;
    rc->phases.hold = next_tmc_reset_hold;
    device_class_set_props(dc, next_tmc_properties);
}

static const TypeInfo next_tmc_info = {
    .name = TYPE_NEXT_TMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTTMCState),
    .instance_init = next_tmc_init,
    .class_init = next_tmc_class_init,
};

static void next_tmc_register_types(void)
{
    type_register_static(&next_tmc_info);
}

type_init(next_tmc_register_types)
