/*
 * NeXT integrated channel processor (Fujitsu MB610313): DMA
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the Previous emulator's dma.c (GPL-2.0-or-later), by the
 * Previous team, which itself contains information from QEMU-NeXT.
 *
 * The chip has twelve channels with 128-byte internal buffers. Each has a
 * CSR at 0x02000000 + channel and Next/Limit/Start/Stop registers at
 * 0x02004000 + channel. Some channels also have "saved" copies of their
 * pointers 0x10 bytes below, and an init register 0x200 above that sets
 * Next and resets the channel's buffer.
 *
 * The devices move data themselves through next_dma_write_mem() and
 * next_dma_read_mem(), and say when a part is done with next_dma_update().
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/dma/next-dma.h"
#include "migration/vmstate.h"
#include "trace.h"

static const uint16_t next_dma_base[NEXT_DMA_NUM_CHANNELS] = {
    [NEXT_DMA_SCSI] = 0x010,
    [NEXT_DMA_SOUNDOUT] = 0x040,
    [NEXT_DMA_DISK] = 0x050,
    [NEXT_DMA_SOUNDIN] = 0x080,
    [NEXT_DMA_PRINTER] = 0x090,
    [NEXT_DMA_SCC] = 0x0c0,
    [NEXT_DMA_DSP] = 0x0d0,
    [NEXT_DMA_EN_TX] = 0x110,
    [NEXT_DMA_EN_RX] = 0x150,
    [NEXT_DMA_VIDEO] = 0x180,
    [NEXT_DMA_M2R] = 0x1d0,
    [NEXT_DMA_R2M] = 0x1c0,
};

/* Channels that have saved pointer registers 0x10 below their base */
static const bool next_dma_has_saved[NEXT_DMA_NUM_CHANNELS] = {
    [NEXT_DMA_SCSI] = true,
    [NEXT_DMA_SOUNDOUT] = true,
    [NEXT_DMA_SOUNDIN] = true,
    [NEXT_DMA_EN_TX] = true,
    [NEXT_DMA_EN_RX] = true,
};

/* Channels that have an init register */
static const bool next_dma_has_init[NEXT_DMA_NUM_CHANNELS] = {
    [NEXT_DMA_SCSI] = true,
    [NEXT_DMA_SOUNDOUT] = true,
    [NEXT_DMA_DISK] = true,
    [NEXT_DMA_SOUNDIN] = true,
    [NEXT_DMA_PRINTER] = true,
    [NEXT_DMA_DSP] = true,
    [NEXT_DMA_EN_TX] = true,
    [NEXT_DMA_EN_RX] = true,
    [NEXT_DMA_M2R] = true,
    [NEXT_DMA_R2M] = true,
};

static int next_dma_find(unsigned base)
{
    for (int i = 0; i < NEXT_DMA_NUM_CHANNELS; i++) {
        if (next_dma_base[i] == base) {
            return i;
        }
    }
    return -1;
}

void next_dma_set_client(NeXTDMAState *s, NeXTDMAChannel ch,
                         NeXTDMAKickFn kick, NeXTDMAInitBufFn initbuf,
                         void *opaque)
{
    s->kick[ch] = kick;
    s->initbuf[ch] = initbuf;
    s->opaque[ch] = opaque;
}

static void next_dma_kick(NeXTDMAState *s, NeXTDMAChannel ch)
{
    if (s->kick[ch]) {
        s->kick[ch](s->opaque[ch], ch);
    }
}

static void next_dma_init_buffer(NeXTDMAState *s, NeXTDMAChannel ch,
                                 unsigned offset)
{
    if (s->initbuf[ch]) {
        s->initbuf[ch](s->opaque[ch], ch, offset);
    }
}

static void next_dma_bus_error(NeXTDMAState *s, NeXTDMAChannel ch)
{
    NeXTDMAChan *c = &s->ch[ch];

    qemu_log_mask(LOG_GUEST_ERROR, "next-dma: channel %d bus error at 0x%08x\n",
                  ch, c->next);
    c->csr &= ~NEXT_DMA_ENABLE;
    c->csr |= NEXT_DMA_COMPLETE | NEXT_DMA_BUSEXC;
}

bool next_dma_write_mem(NeXTDMAState *s, NeXTDMAChannel ch,
                        const void *buf, uint32_t len)
{
    NeXTDMAChan *c = &s->ch[ch];
    hwaddr addr = NEXT_DMA_EN_ADDR(c->next);
    MemTxResult r;

    r = address_space_write(&address_space_memory, addr,
                            MEMTXATTRS_UNSPECIFIED, buf, len);
    if (r != MEMTX_OK) {
        next_dma_bus_error(s, ch);
        return false;
    }
    c->next += len;
    return true;
}

bool next_dma_read_mem(NeXTDMAState *s, NeXTDMAChannel ch,
                       void *buf, uint32_t len)
{
    NeXTDMAChan *c = &s->ch[ch];
    hwaddr addr = NEXT_DMA_EN_ADDR(c->next);
    MemTxResult r;

    r = address_space_read(&address_space_memory, addr,
                           MEMTXATTRS_UNSPECIFIED, buf, len);
    if (r != MEMTX_OK) {
        next_dma_bus_error(s, ch);
        return false;
    }
    c->next += len;
    return true;
}

void next_dma_update(NeXTDMAState *s, NeXTDMAChannel ch)
{
    NeXTDMAChan *c = &s->ch[ch];

    if (c->next == c->limit) {
        c->csr |= NEXT_DMA_COMPLETE;
        if (c->csr & NEXT_DMA_SUPDATE) {
            /* chaining: go on with the second buffer */
            c->next = c->start;
            c->limit = c->stop;
            c->csr &= ~NEXT_DMA_SUPDATE;
        } else {
            c->csr &= ~NEXT_DMA_ENABLE;
        }
    }
    if (c->csr & NEXT_DMA_COMPLETE) {
        qemu_irq_raise(s->irq[ch]);
    }
}

void next_dma_enet_done(NeXTDMAState *s, NeXTDMAChannel ch)
{
    NeXTDMAChan *c = &s->ch[ch];

    c->csr |= NEXT_DMA_COMPLETE;
    if (c->csr & NEXT_DMA_SUPDATE) {
        c->next = c->start;
        c->limit = c->stop;
        if (s->turbo) {
            c->saved_next = c->next;
        }
        c->csr &= ~NEXT_DMA_SUPDATE;
    } else {
        c->csr &= ~NEXT_DMA_ENABLE;
    }
    qemu_irq_raise(s->irq[ch]);
}

void next_dma_video_retrace(NeXTDMAState *s)
{
    NeXTDMAChan *c = &s->ch[NEXT_DMA_VIDEO];

    /* 0xEA kB is the visible part of the mono frame buffer */
    if (c->limit == 0xEA) {
        c->csr |= NEXT_DMA_COMPLETE;
        qemu_irq_raise(s->irq[NEXT_DMA_VIDEO]);
    }
}

/* Memory to memory: M2R reads into the buffer, R2M writes it out */
static void next_dma_m2m_step(void *opaque)
{
    NeXTDMAState *s = opaque;
    NeXTDMAChan *r2m = &s->ch[NEXT_DMA_R2M];
    NeXTDMAChan *m2r = &s->ch[NEXT_DMA_M2R];
    int bursts = 64;

    while (bursts-- && (r2m->csr & NEXT_DMA_ENABLE) &&
           r2m->next < r2m->limit) {
        if ((m2r->csr & NEXT_DMA_ENABLE) && m2r->next < m2r->limit) {
            if (!next_dma_read_mem(s, NEXT_DMA_M2R, s->m2m_buf,
                                   NEXT_DMA_BURST)) {
                next_dma_update(s, NEXT_DMA_M2R);
                return;
            }
            next_dma_update(s, NEXT_DMA_M2R);
        }
        /* Without new data the buffer is written again */
        if (!next_dma_write_mem(s, NEXT_DMA_R2M, s->m2m_buf,
                                NEXT_DMA_BURST)) {
            next_dma_update(s, NEXT_DMA_R2M);
            return;
        }
        next_dma_update(s, NEXT_DMA_R2M);
    }
    if ((r2m->csr & NEXT_DMA_ENABLE) && r2m->next < r2m->limit) {
        timer_mod(s->m2m_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
    }
}

static void next_dma_m2m_start(NeXTDMAState *s)
{
    NeXTDMAChan *r2m = &s->ch[NEXT_DMA_R2M];
    NeXTDMAChan *m2r = &s->ch[NEXT_DMA_M2R];

    if ((m2r->csr & NEXT_DMA_ENABLE) && (r2m->csr & NEXT_DMA_ENABLE)) {
        if (((r2m->limit - r2m->next) % NEXT_DMA_BURST) ||
            ((m2r->limit - m2r->next) % NEXT_DMA_BURST)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "next-dma: memory to memory not burst aligned\n");
            return;
        }
        timer_mod(s->m2m_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
    }
}

static void next_dma_csr_write(NeXTDMAState *s, NeXTDMAChannel ch,
                               uint32_t val)
{
    NeXTDMAChan *c = &s->ch[ch];

    c->direction = val & NEXT_DMA_DEV2M;

    if (s->turbo) {
        if (val & NEXT_DMA_RESET) {
            c->csr &= ~(NEXT_DMA_COMPLETE | NEXT_DMA_SUPDATE |
                        NEXT_DMA_ENABLE);
        }
        if (val & NEXT_TDMA_BUFRESET) {
            next_dma_init_buffer(s, ch, 0);
        }
        if (val & NEXT_DMA_SETSUPDATE) {
            c->csr |= NEXT_DMA_SUPDATE;
        }
        if (val & NEXT_DMA_SETENABLE) {
            c->csr |= NEXT_DMA_ENABLE;
        }
        if (val & NEXT_DMA_CLRCOMPLETE) {
            c->csr &= ~NEXT_DMA_COMPLETE;
        }
    } else {
        if (val & NEXT_DMA_RESET) {
            c->csr &= ~(NEXT_DMA_COMPLETE | NEXT_DMA_SUPDATE |
                        NEXT_DMA_ENABLE);
        }
        if (val & NEXT_DMA_INITBUF) {
            next_dma_init_buffer(s, ch, 0);
        }
        if (val & NEXT_DMA_SETSUPDATE) {
            c->csr |= NEXT_DMA_SUPDATE;
        }
        if (val & NEXT_DMA_SETENABLE) {
            c->csr |= NEXT_DMA_ENABLE;
            if (ch == NEXT_DMA_R2M || ch == NEXT_DMA_M2R) {
                if (c->next == c->limit) {
                    c->csr &= ~NEXT_DMA_ENABLE;
                }
                next_dma_m2m_start(s);
            }
        }
        if ((val & NEXT_DMA_CLRCOMPLETE) && (c->csr & NEXT_DMA_ENABLE)) {
            c->csr &= ~NEXT_DMA_COMPLETE;
        }
    }
    if (!(c->csr & NEXT_DMA_COMPLETE)) {
        qemu_irq_lower(s->irq[ch]);
    }
    /* tell the device, which may start or stop with the channel */
    next_dma_kick(s, ch);
}

/*
 * The bits of a 32-bit register that a byte or word access at addr
 * reaches: QEMU does not align such accesses for the device.
 */
static int next_dma_shift(hwaddr addr, unsigned size)
{
    return (4 - (addr & 3) - size) * 8;
}

/* CSRs: 0x02000000 - 0x02001fff, bit 12 is not decoded */
static MemTxResult next_dma_csr_read(void *opaque, hwaddr addr,
                                     uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    NeXTDMAState *s = opaque;
    int ch = next_dma_find(addr & 0xffc & ~0xf);

    if (ch < 0 || (addr & 0xc)) {
        return MEMTX_DECODE_ERROR;
    }
    *data = extract32(s->ch[ch].csr, next_dma_shift(addr, size), size * 8);
    return MEMTX_OK;
}

static MemTxResult next_dma_csr_write_ops(void *opaque, hwaddr addr,
                                          uint64_t data, unsigned size,
                                          MemTxAttrs attrs)
{
    NeXTDMAState *s = opaque;
    int ch = next_dma_find(addr & 0xffc & ~0xf);

    if (ch < 0 || (addr & 0xc)) {
        return MEMTX_DECODE_ERROR;
    }
    /*
     * The bus copies a byte or a word into every lane of the long word,
     * as Previous does: the commands are in bits 16-23, wherever a byte
     * write goes. The 68030 ROMs write them as bytes at the start.
     */
    if (size == 1) {
        data = (data & 0xff) * 0x01010101u;
    } else if (size == 2) {
        data = (data & 0xffff) * 0x00010001u;
    }
    next_dma_csr_write(s, ch, (uint32_t)data);
    return MEMTX_OK;
}

static const MemoryRegionOps next_dma_csr_ops = {
    .read_with_attrs = next_dma_csr_read,
    .write_with_attrs = next_dma_csr_write_ops,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Pointer registers: 0x02004000 - 0x02005fff, bit 12 is not decoded.
 * Returns a pointer to the register, or NULL. *init is set for the init
 * register.
 */
static uint32_t *next_dma_reg(NeXTDMAState *s, hwaddr addr, int *chp,
                              bool *init)
{
    unsigned off = addr & 0xffc;
    unsigned reg = (off & 0xc) >> 2;
    int ch;

    *init = false;
    /*
     * The Turbo Ethernet receive channel has its saved limit (the end of
     * the last packet) here, where the MO channel is on the others.
     */
    if (s->turbo && off == 0x050) {
        *chp = NEXT_DMA_EN_RX;
        return &s->ch[NEXT_DMA_EN_RX].saved_limit;
    }
    if (off >= 0x200) {
        ch = next_dma_find(off & 0x1f0 & ~0xf);
        if (ch < 0 || !next_dma_has_init[ch] || reg != 0) {
            return NULL;
        }
        *chp = ch;
        *init = true;
        return &s->ch[ch].next;
    }
    ch = next_dma_find(off & ~0xf);
    if (ch >= 0) {
        *chp = ch;
        switch (reg) {
        case 0: return &s->ch[ch].next;
        case 1: return &s->ch[ch].limit;
        case 2: return &s->ch[ch].start;
        default: return &s->ch[ch].stop;
        }
    }
    ch = next_dma_find((off & ~0xf) + 0x10);
    if (ch >= 0 && next_dma_has_saved[ch]) {
        *chp = ch;
        switch (reg) {
        case 0: return &s->ch[ch].saved_next;
        case 1: return &s->ch[ch].saved_limit;
        case 2: return &s->ch[ch].saved_start;
        default: return &s->ch[ch].saved_stop;
        }
    }
    return NULL;
}

static MemTxResult next_dma_reg_read(void *opaque, hwaddr addr,
                                     uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    NeXTDMAState *s = opaque;
    bool init;
    int ch;
    uint32_t *r = next_dma_reg(s, addr, &ch, &init);

    if (!r) {
        return MEMTX_DECODE_ERROR;
    }
    *data = extract32(*r, next_dma_shift(addr, size), size * 8);
    return MEMTX_OK;
}

static MemTxResult next_dma_reg_write(void *opaque, hwaddr addr,
                                      uint64_t data, unsigned size,
                                      MemTxAttrs attrs)
{
    NeXTDMAState *s = opaque;
    bool init;
    int ch;
    uint32_t *r = next_dma_reg(s, addr, &ch, &init);

    if (!r) {
        return MEMTX_DECODE_ERROR;
    }
    if (s->turbo && ch == NEXT_DMA_EN_RX && (addr & 0xffc) == 0x050) {
        /* read only */
        return MEMTX_OK;
    }
    *r = deposit32(*r, next_dma_shift(addr, size), size * 8, data);
    if (init) {
        next_dma_init_buffer(s, ch, *r & 0xf);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps next_dma_reg_ops = {
    .read_with_attrs = next_dma_reg_read,
    .write_with_attrs = next_dma_reg_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void next_dma_reset_hold(Object *obj, ResetType type)
{
    NeXTDMAState *s = NEXT_DMA(obj);

    for (int i = 0; i < NEXT_DMA_NUM_CHANNELS; i++) {
        s->ch[i].csr = 0;
        next_dma_init_buffer(s, i, 0);
        qemu_irq_lower(s->irq[i]);
    }
    timer_del(s->m2m_timer);
}

static void next_dma_init(Object *obj)
{
    NeXTDMAState *s = NEXT_DMA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->csr_mem, obj, &next_dma_csr_ops, s,
                          "next.dma.csr", 0x2000);
    sysbus_init_mmio(sbd, &s->csr_mem);
    memory_region_init_io(&s->reg_mem, obj, &next_dma_reg_ops, s,
                          "next.dma.regs", 0x2000);
    sysbus_init_mmio(sbd, &s->reg_mem);

    for (int i = 0; i < NEXT_DMA_NUM_CHANNELS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static void next_dma_realize(DeviceState *dev, Error **errp)
{
    NeXTDMAState *s = NEXT_DMA(dev);

    s->m2m_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, next_dma_m2m_step, s);
}

static const VMStateDescription next_dma_chan_vmstate = {
    .name = "next-dma-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(csr, NeXTDMAChan),
        VMSTATE_UINT32(saved_next, NeXTDMAChan),
        VMSTATE_UINT32(saved_limit, NeXTDMAChan),
        VMSTATE_UINT32(saved_start, NeXTDMAChan),
        VMSTATE_UINT32(saved_stop, NeXTDMAChan),
        VMSTATE_UINT32(next, NeXTDMAChan),
        VMSTATE_UINT32(limit, NeXTDMAChan),
        VMSTATE_UINT32(start, NeXTDMAChan),
        VMSTATE_UINT32(stop, NeXTDMAChan),
        VMSTATE_UINT32(direction, NeXTDMAChan),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription next_dma_vmstate = {
    .name = "next-dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(ch, NeXTDMAState, NEXT_DMA_NUM_CHANNELS, 1,
                             next_dma_chan_vmstate, NeXTDMAChan),
        VMSTATE_UINT8_ARRAY(m2m_buf, NeXTDMAState, NEXT_DMA_BURST),
        VMSTATE_TIMER_PTR(m2m_timer, NeXTDMAState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property next_dma_properties[] = {
    DEFINE_PROP_BOOL("turbo", NeXTDMAState, turbo, false),
};

static void next_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT DMA controller";
    dc->realize = next_dma_realize;
    dc->vmsd = &next_dma_vmstate;
    device_class_set_props(dc, next_dma_properties);
    rc->phases.hold = next_dma_reset_hold;
}

static const TypeInfo next_dma_info = {
    .name = TYPE_NEXT_DMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTDMAState),
    .instance_init = next_dma_init,
    .class_init = next_dma_class_init,
};

static void next_dma_register_types(void)
{
    type_register_static(&next_dma_info);
}

type_init(next_dma_register_types)
