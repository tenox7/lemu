/*
 * QEMU DMA emulation
 *
 * Copyright (c) 2003-2004 Vassili Karpov (malc)
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/isa/isa.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/dma/i8257.h"
#include "hw/core/irq.h"
#include "exec/cpu-common.h"
#include "system/physmem.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "trace.h"


/* #define DEBUG_DMA */

#define dolog(...) qemu_log_mask(LOG_GUEST_ERROR, "dma: " __VA_ARGS__)
#ifdef DEBUG_DMA
#define linfo(...) fprintf (stderr, "dma: " __VA_ARGS__)
#define ldebug(...) fprintf (stderr, "dma: " __VA_ARGS__)
#else
#define linfo(...)
#define ldebug(...)
#endif

#define ADDR 0
#define COUNT 1

enum {
    CMD_MEMORY_TO_MEMORY = 0x01,
    CMD_FIXED_ADDRESS    = 0x02,
    CMD_BLOCK_CONTROLLER = 0x04,
    CMD_COMPRESSED_TIME  = 0x08,
    CMD_CYCLIC_PRIORITY  = 0x10,
    CMD_EXTENDED_WRITE   = 0x20,
    CMD_LOW_DREQ         = 0x40,
    CMD_LOW_DACK         = 0x80,
    CMD_NOT_SUPPORTED    = CMD_MEMORY_TO_MEMORY | CMD_FIXED_ADDRESS
    | CMD_COMPRESSED_TIME | CMD_CYCLIC_PRIORITY | CMD_EXTENDED_WRITE
    | CMD_LOW_DREQ | CMD_LOW_DACK

};

static void i8257_dma_run(void *opaque);

static const int channels[8] = {-1, 2, 3, 1, -1, -1, -1, 0};

static void i8257_write_page(void *opaque, uint32_t nport, uint32_t data)
{
    I8257State *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        dolog ("invalid channel %#x %#x\n", nport, data);
        return;
    }
    d->regs[ichan].page = data;
    d->regs[ichan].pageh = 0;
}

static void i8257_write_pageh(void *opaque, uint32_t nport, uint32_t data)
{
    I8257State *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        dolog ("invalid channel %#x %#x\n", nport, data);
        return;
    }
    d->regs[ichan].pageh = data;
}

static uint32_t i8257_read_page(void *opaque, uint32_t nport)
{
    I8257State *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        dolog ("invalid channel read %#x\n", nport);
        return 0;
    }
    return d->regs[ichan].page;
}

static uint32_t i8257_read_pageh(void *opaque, uint32_t nport)
{
    I8257State *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        dolog ("invalid channel read %#x\n", nport);
        return 0;
    }
    return d->regs[ichan].pageh;
}

/*
 * How far a channel's address and count registers are shifted: by words
 * (1) or by bytes (0). The EISA extended mode register picks it per
 * channel; without it each controller has its own fixed width.
 */
static int i8257_chan_shift(I8257State *d, int ichan)
{
    if (!d->ext_mode_on || d->pageh_base < 0) {
        return d->dshift;
    }
    return ((d->ext_mode[ichan] >> 2) & 3) == 1;
}

static inline void i8257_init_chan(I8257State *d, int ichan)
{
    I8257Regs *r;

    r = d->regs + ichan;
    r->now[ADDR] = r->base[ADDR] << i8257_chan_shift(d, ichan);
    r->now[COUNT] = 0;
}

static inline int i8257_getff(I8257State *d)
{
    int ff;

    ff = d->flip_flop;
    d->flip_flop = !ff;
    return ff;
}

static uint64_t i8257_read_chan(void *opaque, hwaddr nport, unsigned size)
{
    I8257State *d = opaque;
    int ichan, nreg, iport, ff, val, dir, shift;
    I8257Regs *r;

    iport = (nport >> d->dshift) & 0x0f;
    ichan = iport >> 1;
    nreg = iport & 1;
    r = d->regs + ichan;
    shift = i8257_chan_shift(d, ichan);

    dir = ((r->mode >> 5) & 1) ? -1 : 1;
    ff = i8257_getff(d);
    if (nreg)
        val = (r->base[COUNT] << shift) - r->now[COUNT];
    else
        val = r->now[ADDR] + r->now[COUNT] * dir;

    ldebug ("read_chan %#x -> %d\n", iport, val);
    return (val >> (shift + (ff << 3))) & 0xff;
}

static void i8257_write_chan(void *opaque, hwaddr nport, uint64_t data,
                             unsigned int size)
{
    I8257State *d = opaque;
    int iport, ichan, nreg;
    I8257Regs *r;

    iport = (nport >> d->dshift) & 0x0f;
    ichan = iport >> 1;
    nreg = iport & 1;
    r = d->regs + ichan;
    /* EISA: an ISA-style write clears the high page or high count */
    if (nreg == ADDR) {
        r->pageh = 0;
    } else {
        d->counth[ichan] = 0;
    }
    if (i8257_getff(d)) {
        r->base[nreg] = (r->base[nreg] & 0xff) | ((data << 8) & 0xff00);
        i8257_init_chan(d, ichan);
    } else {
        r->base[nreg] = (r->base[nreg] & 0xff00) | (data & 0xff);
    }
}

static void i8257_write_cont(void *opaque, hwaddr nport, uint64_t data,
                             unsigned int size)
{
    I8257State *d = opaque;
    int iport, ichan = 0;

    iport = (nport >> d->dshift) & 0x0f;
    switch (iport) {
    case 0x00:                  /* command */
        if ((data != 0) && (data & CMD_NOT_SUPPORTED)) {
            qemu_log_mask(LOG_UNIMP, "%s: cmd 0x%02"PRIx64" not supported\n",
                          __func__, data);
            return;
        }
        d->command = data;
        break;

    case 0x01:
        ichan = data & 3;
        if (data & 4) {
            d->status |= 1 << (ichan + 4);
        }
        else {
            d->status &= ~(1 << (ichan + 4));
        }
        d->status &= ~(1 << ichan);
        i8257_dma_run(d);
        break;

    case 0x02:                  /* single mask */
        if (data & 4)
            d->mask |= 1 << (data & 3);
        else
            d->mask &= ~(1 << (data & 3));
        i8257_dma_run(d);
        break;

    case 0x03:                  /* mode */
        {
            ichan = data & 3;
#ifdef DEBUG_DMA
            {
                int op, ai, dir, opmode;
                op = (data >> 2) & 3;
                ai = (data >> 4) & 1;
                dir = (data >> 5) & 1;
                opmode = (data >> 6) & 3;

                linfo ("ichan %d, op %d, ai %d, dir %d, opmode %d\n",
                       ichan, op, ai, dir, opmode);
            }
#endif
            d->regs[ichan].mode = data;
            break;
        }

    case 0x04:                  /* clear flip flop */
        d->flip_flop = 0;
        break;

    case 0x05:                  /* reset */
        d->flip_flop = 0;
        d->mask = ~0;
        d->status = 0;
        d->command = 0;
        memset(d->sg_status, 0, sizeof(d->sg_status));
        d->sg_irq = 0;
        qemu_irq_lower(d->sg_irq_out);
        break;

    case 0x06:                  /* clear mask for all channels */
        d->mask = 0;
        i8257_dma_run(d);
        break;

    case 0x07:                  /* write mask for all channels */
        d->mask = data;
        i8257_dma_run(d);
        break;

    default:
        dolog ("unknown iport %#x\n", iport);
        break;
    }

#ifdef DEBUG_DMA
    if (0xc != iport) {
        linfo ("write_cont: nport %#06x, ichan % 2d, val %#06x\n",
               nport, ichan, data);
    }
#endif
}

static uint64_t i8257_read_cont(void *opaque, hwaddr nport, unsigned size)
{
    I8257State *d = opaque;
    int iport, val;

    iport = (nport >> d->dshift) & 0x0f;
    switch (iport) {
    case 0x00:                  /* status */
        val = d->status;
        d->status &= 0xf0;
        break;
    case 0x01:                  /* mask */
    case 0x07:                  /* EISA: read all mask bits */
        val = d->mask;
        break;
    default:
        val = 0;
        break;
    }

    ldebug ("read_cont: nport %#06x, iport %#04x val %#x\n", nport, iport, val);
    return val;
}

static bool i8257_dma_has_autoinitialization(IsaDma *obj, int nchan)
{
    I8257State *d = I8257(obj);
    return (d->regs[nchan & 3].mode >> 4) & 1;
}

static void i8257_dma_hold_DREQ(IsaDma *obj, int nchan)
{
    I8257State *d = I8257(obj);
    int ichan;

    ichan = nchan & 3;
    d->status |= 1 << (ichan + 4);
    i8257_dma_run(d);
}

static void i8257_dma_release_DREQ(IsaDma *obj, int nchan)
{
    I8257State *d = I8257(obj);
    int ichan;

    ichan = nchan & 3;
    d->status &= ~(1 << (ichan + 4));
    i8257_dma_run(d);
}

/*
 * Fetch the next scatter-gather descriptor into the channel's buffer:
 * a little-endian PCI address, then the byte count less one in bits
 * 23:0 and the end of the list in bit 31.
 */
static void i8257_sg_fetch(I8257State *d, int ichan)
{
    uint32_t desc[2];

    physical_memory_read(d->sg_ptr[ichan] & 0x7fffffff, desc, sizeof(desc));
    d->sg_ptr[ichan] += 8;
    d->sg_addr[ichan] = le32_to_cpu(desc[0]);
    d->sg_len[ichan] = (le32_to_cpu(desc[1]) & 0xffffff) + 1;
    d->sg_status[ichan] |= I8257_SG_CURRENT;
    if (le32_to_cpu(desc[1]) & 0x80000000) {
        d->sg_status[ichan] |= I8257_SG_LAST;
    }
    d->regs[ichan].now[COUNT] = 0;
}

/* The last buffer is done, or software stopped the list */
static void i8257_sg_end(I8257State *d, int ichan)
{
    d->sg_status[ichan] &= ~(I8257_SG_ACTIVE | I8257_SG_CURRENT);
    d->mask |= 1 << ichan;
    d->status |= 1 << ichan;
    if (d->sg_status[ichan] & I8257_SG_IRQ13) {
        d->sg_irq |= 1 << ichan;
        qemu_irq_raise(d->sg_irq_out);
    }
}

static void i8257_channel_run(I8257State *d, int ichan)
{
    int ncont = d->dshift;
    int shift = i8257_chan_shift(d, ichan);
    int n;
    I8257Regs *r = &d->regs[ichan];

    if (d->sg_status[ichan] & I8257_SG_ACTIVE) {
        uint32_t len = d->sg_len[ichan];

        n = r->transfer_handler(r->opaque, ichan + (ncont << 2),
                                r->now[COUNT], len);
        r->now[COUNT] = n;
        if (n == len) {
            if (d->sg_status[ichan] & I8257_SG_LAST) {
                i8257_sg_end(d, ichan);
            } else {
                i8257_sg_fetch(d, ichan);
            }
        }
        return;
    }
#ifdef DEBUG_DMA
    int dir, opmode;

    dir = (r->mode >> 5) & 1;
    opmode = (r->mode >> 6) & 3;

    if (dir) {
        dolog ("DMA in address decrement mode\n");
    }
    if (opmode != 1) {
        dolog ("DMA not in single mode select %#x\n", opmode);
    }
#endif

    n = r->transfer_handler (r->opaque, ichan + (ncont << 2),
                             r->now[COUNT], (r->base[COUNT] + 1) << shift);
    r->now[COUNT] = n;
    ldebug ("dma_pos %d size %d\n", n, (r->base[COUNT] + 1) << shift);
    if (n == (r->base[COUNT] + 1) << shift) {
        ldebug("transfer done\n");
        d->status |= (1 << ichan);
    }
}

static void i8257_dma_run(void *opaque)
{
    I8257State *d = opaque;
    int ichan;
    int rearm = 0;

    if (d->running) {
        rearm = 1;
        goto out;
    } else {
        d->running = 1;
    }

    for (ichan = 0; ichan < 4; ichan++) {
        int mask;

        mask = 1 << ichan;

        if ((0 == (d->mask & mask)) && (0 != (d->status & (mask << 4)))) {
            i8257_channel_run(d, ichan);
            rearm = 1;
        }
    }

    d->running = 0;
out:
    if (rearm) {
        qemu_bh_schedule_idle(d->dma_bh);
        d->dma_bh_scheduled = true;
    }
}

static void i8257_dma_register_channel(IsaDma *obj, int nchan,
                                       IsaDmaTransferHandler transfer_handler,
                                       void *opaque)
{
    I8257State *d = I8257(obj);
    I8257Regs *r;
    int ichan;

    ichan = nchan & 3;

    r = d->regs + ichan;
    r->transfer_handler = transfer_handler;
    r->opaque = opaque;
}

static bool i8257_is_verify_transfer(I8257Regs *r)
{
    return (r->mode & 0x0c) == 0;
}

/*
 * Where a channel's transfer starts in memory. Bit 31 of a DMA address
 * (bit 7 of the high page) is dropped: on the PReP and BeBox boards RAM
 * sits at PCI 0x80000000, and these addresses are PCI addresses.
 */
static hwaddr i8257_chan_addr(I8257State *d, int ichan)
{
    I8257Regs *r = &d->regs[ichan];

    if (d->sg_status[ichan] & I8257_SG_ACTIVE) {
        return d->sg_addr[ichan] & 0x7fffffff;
    }
    return ((r->pageh & 0x7f) << 24) | (r->page << 16) | r->now[ADDR];
}

static int i8257_dma_read_memory(IsaDma *obj, int nchan, void *buf, int pos,
                                 int len)
{
    I8257State *d = I8257(obj);
    I8257Regs *r = &d->regs[nchan & 3];
    hwaddr addr = i8257_chan_addr(d, nchan & 3);

    if (i8257_is_verify_transfer(r)) {
        /*
         * If the device is expecting this verify operation then
         * it won't care about the nonexistent data. But if it
         * is expecting a real read (i.e. the guest has misprogrammed
         * the DMA controller and the device) it's going to try to do
         * something with the buffer contents. Give it zeroes.
         * (It's not clear whether this is exactly what happens if
         * you do this on real hardware. In practice no device QEMU
         * emulates has a use for verify on a memory-read transfer,
         * so we don't care beyond avoiding the guest being able to
         * trigger the caller reading uninitialized data.)
         */
        memset(buf, 0, len);
        return len;
    }

    if (r->mode & 0x20) {
        int i;
        uint8_t *p = buf;

        physical_memory_read(addr - pos - len, buf, len);
        /* What about 16bit transfers? */
        for (i = 0; i < len >> 1; i++) {
            uint8_t b = p[len - i - 1];
            p[i] = b;
        }
    }
    else
        physical_memory_read(addr + pos, buf, len);

    return len;
}

static int i8257_dma_write_memory(IsaDma *obj, int nchan, void *buf, int pos,
                                 int len)
{
    I8257State *s = I8257(obj);
    I8257Regs *r = &s->regs[nchan & 3];
    hwaddr addr = i8257_chan_addr(s, nchan & 3);

    if (i8257_is_verify_transfer(r)) {
        return len;
    }

    if (r->mode & 0x20) {
        int i;
        uint8_t *p = buf;

        physical_memory_write(addr - pos - len, buf, len);
        /* What about 16bit transfers? */
        for (i = 0; i < len; i++) {
            uint8_t b = p[len - i - 1];
            p[i] = b;
        }
    }
    else
        physical_memory_write(addr + pos, buf, len);

    return len;
}

/* request the emulator to transfer a new DMA memory block ASAP (even
 * if the idle bottom half would not have exited the iothread yet).
 */
static void i8257_dma_schedule(IsaDma *obj)
{
    I8257State *d = I8257(obj);
    if (d->dma_bh_scheduled) {
        qemu_notify_event();
    }
}

static void i8257_reset(DeviceState *dev)
{
    I8257State *d = I8257(dev);
    int i;

    i8257_write_cont(d, (0x05 << d->dshift), 0, 1);
    /* channels 0-3 move bytes, channels 5-7 words */
    for (i = 0; i < 4; i++) {
        d->ext_mode[i] = (d->dshift ? 0x04 : 0x00) | i;
    }
}

static int i8257_phony_handler(void *opaque, int nchan, int dma_pos,
                               int dma_len)
{
    trace_i8257_unregistered_dma(nchan, dma_pos, dma_len);
    return dma_pos;
}


static const MemoryRegionOps channel_io_ops = {
    .read = i8257_read_chan,
    .write = i8257_write_chan,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* EISA high count registers, at the count register offsets */
static uint64_t i8257_read_counth(void *opaque, hwaddr nport, unsigned size)
{
    I8257State *d = opaque;
    int iport = (nport >> d->dshift) & 7;

    return (iport & 1) ? d->counth[iport >> 1] : 0;
}

static void i8257_write_counth(void *opaque, hwaddr nport, uint64_t data,
                               unsigned size)
{
    I8257State *d = opaque;
    int iport = (nport >> d->dshift) & 7;

    if (iport & 1) {
        d->counth[iport >> 1] = data;
    }
}

static const MemoryRegionOps counth_io_ops = {
    .read = i8257_read_counth,
    .write = i8257_write_counth,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* EISA stop registers, 3 bytes per channel, bits 1:0 reserved */
static uint64_t i8257_read_stop(void *opaque, hwaddr nport, unsigned size)
{
    I8257State *d = opaque;

    return (nport & 3) == 3 ? 0 : d->stop[nport >> 2][nport & 3];
}

static void i8257_write_stop(void *opaque, hwaddr nport, uint64_t data,
                             unsigned size)
{
    I8257State *d = opaque;

    if ((nport & 3) == 3) {
        return;
    }
    d->stop[nport >> 2][nport & 3] = (nport & 3) ? data : data & 0xfc;
}

/*
 * EISA extended mode register (0x40b, 0x4d6): bits 1:0 pick the
 * channel, bits 3:2 its width (00 8-bit by bytes, 01 16-bit by words,
 * 10 32-bit by bytes, 11 16-bit by bytes). Timing, EOP and stop bits
 * are kept but change nothing here. It is write-only.
 */
static uint64_t i8257_read_ext_mode(void *opaque, hwaddr nport,
                                    unsigned size)
{
    return 0;
}

static void i8257_write_ext_mode(void *opaque, hwaddr nport, uint64_t data,
                                 unsigned size)
{
    I8257State *d = opaque;
    int ichan = data & 3;

    d->ext_mode[ichan] = data;
    i8257_init_chan(d, ichan);
}

static const MemoryRegionOps ext_mode_io_ops = {
    .read = i8257_read_ext_mode,
    .write = i8257_write_ext_mode,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const MemoryRegionOps stop_io_ops = {
    .read = i8257_read_stop,
    .write = i8257_write_stop,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* IOport from page_base */
static const MemoryRegionPortio page_portio_list[] = {
    { 0x01, 3, 1, .write = i8257_write_page, .read = i8257_read_page, },
    { 0x07, 1, 1, .write = i8257_write_page, .read = i8257_read_page, },
    PORTIO_END_OF_LIST(),
};

/* IOport from pageh_base */
static const MemoryRegionPortio pageh_portio_list[] = {
    { 0x01, 3, 1, .write = i8257_write_pageh, .read = i8257_read_pageh, },
    { 0x07, 1, 1, .write = i8257_write_pageh, .read = i8257_read_pageh, },
    PORTIO_END_OF_LIST(),
};

static const MemoryRegionOps cont_io_ops = {
    .read = i8257_read_cont,
    .write = i8257_write_cont,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

void i8257_sg_command(I8257State *d, int ichan, uint8_t cmd)
{
    if (cmd & 0x40) {
        /* bit 7 picks EOP (1) or IRQ13 (0) at the end of the list */
        if (cmd & 0x80) {
            d->sg_status[ichan] &= ~I8257_SG_IRQ13;
        } else {
            d->sg_status[ichan] |= I8257_SG_IRQ13;
        }
    }
    switch (cmd & 3) {
    case 1:                     /* start */
        d->sg_irq &= ~(1 << ichan);
        if (!d->sg_irq) {
            qemu_irq_lower(d->sg_irq_out);
        }
        d->sg_status[ichan] &= I8257_SG_IRQ13;
        d->sg_status[ichan] |= I8257_SG_ACTIVE;
        i8257_sg_fetch(d, ichan);
        i8257_dma_run(d);
        break;
    case 2:                     /* stop */
        if (d->sg_status[ichan] & I8257_SG_ACTIVE) {
            d->sg_status[ichan] &= ~(I8257_SG_ACTIVE | I8257_SG_CURRENT);
            d->mask |= 1 << ichan;
        }
        break;
    }
}

uint8_t i8257_sg_status(I8257State *d, int ichan)
{
    return d->sg_status[ichan];
}

void i8257_sg_set_pointer(I8257State *d, int ichan, uint32_t ptr)
{
    d->sg_ptr[ichan] = ptr;
}

uint32_t i8257_sg_get_pointer(I8257State *d, int ichan)
{
    return d->sg_ptr[ichan];
}

uint8_t i8257_sg_irq_status(I8257State *d)
{
    return d->sg_irq;
}

static const VMStateDescription vmstate_i8257_regs = {
    .name = "dma_regs",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32_ARRAY(now, I8257Regs, 2),
        VMSTATE_UINT16_ARRAY(base, I8257Regs, 2),
        VMSTATE_UINT8(mode, I8257Regs),
        VMSTATE_UINT8(page, I8257Regs),
        VMSTATE_UINT8(pageh, I8257Regs),
        VMSTATE_UINT8(dack, I8257Regs),
        VMSTATE_UINT8(eop, I8257Regs),
        VMSTATE_END_OF_LIST()
    }
};

static int i8257_post_load(void *opaque, int version_id)
{
    I8257State *d = opaque;
    i8257_dma_run(d);

    return 0;
}

static bool i8257_eisa_needed(void *opaque)
{
    I8257State *d = opaque;

    return d->pageh_base >= 0;
}

static const VMStateDescription vmstate_i8257_eisa = {
    .name = "dma/eisa",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i8257_eisa_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(counth, I8257State, 4),
        VMSTATE_UINT8_2DARRAY(stop, I8257State, 4, 3),
        VMSTATE_END_OF_LIST()
    }
};

static bool i8257_ext_mode_needed(void *opaque)
{
    I8257State *d = opaque;

    return d->ext_mode_on && d->pageh_base >= 0;
}

static const VMStateDescription vmstate_i8257_ext_mode = {
    .name = "dma/ext-mode",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i8257_ext_mode_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(ext_mode, I8257State, 4),
        VMSTATE_END_OF_LIST()
    }
};

/* only once a scatter-gather list has been used */
static bool i8257_sg_needed(void *opaque)
{
    I8257State *d = opaque;
    int i;

    if (d->pageh_base < 0) {
        return false;
    }
    if (d->sg_irq) {
        return true;
    }
    for (i = 0; i < 4; i++) {
        if (d->sg_status[i] || d->sg_ptr[i] || d->sg_addr[i] ||
            d->sg_len[i]) {
            return true;
        }
    }
    return false;
}

static const VMStateDescription vmstate_i8257_sg = {
    .name = "dma/sg",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i8257_sg_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(sg_ptr, I8257State, 4),
        VMSTATE_UINT32_ARRAY(sg_addr, I8257State, 4),
        VMSTATE_UINT32_ARRAY(sg_len, I8257State, 4),
        VMSTATE_UINT8_ARRAY(sg_status, I8257State, 4),
        VMSTATE_UINT8(sg_irq, I8257State),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_i8257 = {
    .name = "dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = i8257_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(command, I8257State),
        VMSTATE_UINT8(mask, I8257State),
        VMSTATE_UINT8(flip_flop, I8257State),
        VMSTATE_INT32(dshift, I8257State),
        VMSTATE_STRUCT_ARRAY(regs, I8257State, 4, 1, vmstate_i8257_regs,
                             I8257Regs),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_i8257_eisa,
        &vmstate_i8257_ext_mode,
        &vmstate_i8257_sg,
        NULL
    }
};

static void i8257_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isa = ISA_DEVICE(dev);
    I8257State *d = I8257(dev);
    int i;

    memory_region_init_io(&d->channel_io, OBJECT(dev), &channel_io_ops, d,
                          "dma-chan", 8 << d->dshift);
    memory_region_add_subregion(isa_address_space_io(isa),
                                d->base, &d->channel_io);

    isa_register_portio_list(isa, &d->portio_page,
                             d->page_base, page_portio_list, d,
                             "dma-page");
    if (d->pageh_base >= 0) {
        isa_register_portio_list(isa, &d->portio_pageh,
                                 d->pageh_base, pageh_portio_list, d,
                                 "dma-pageh");
        memory_region_init_io(&d->counth_io, OBJECT(dev), &counth_io_ops, d,
                              "dma-counth", 8 << d->dshift);
        memory_region_add_subregion(isa_address_space_io(isa),
                                    0x400 + d->base, &d->counth_io);
        memory_region_init_io(&d->stop_io, OBJECT(dev), &stop_io_ops, d,
                              "dma-stop", 16);
        memory_region_add_subregion(isa_address_space_io(isa),
                                    0x4e0 + (d->dshift << 4), &d->stop_io);
        if (d->ext_mode_on) {
            memory_region_init_io(&d->ext_mode_io, OBJECT(dev),
                                  &ext_mode_io_ops, d, "dma-ext-mode", 1);
            memory_region_add_subregion(isa_address_space_io(isa),
                                        d->dshift ? 0x4d6 : 0x40b,
                                        &d->ext_mode_io);
        }
    }

    memory_region_init_io(&d->cont_io, OBJECT(isa), &cont_io_ops, d,
                          "dma-cont", 8 << d->dshift);
    memory_region_add_subregion(isa_address_space_io(isa),
                                d->base + (8 << d->dshift), &d->cont_io);

    for (i = 0; i < ARRAY_SIZE(d->regs); ++i) {
        d->regs[i].transfer_handler = i8257_phony_handler;
    }

    d->dma_bh = qemu_bh_new(i8257_dma_run, d);
}

static const Property i8257_properties[] = {
    DEFINE_PROP_INT32("base", I8257State, base, 0x00),
    DEFINE_PROP_INT32("page-base", I8257State, page_base, 0x80),
    DEFINE_PROP_INT32("pageh-base", I8257State, pageh_base, 0x480),
    DEFINE_PROP_INT32("dshift", I8257State, dshift, 0),
    DEFINE_PROP_BOOL("ext-mode", I8257State, ext_mode_on, false),
};

static void i8257_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    IsaDmaClass *idc = ISADMA_CLASS(klass);

    dc->realize = i8257_realize;
    device_class_set_legacy_reset(dc, i8257_reset);
    dc->vmsd = &vmstate_i8257;
    device_class_set_props(dc, i8257_properties);

    idc->has_autoinitialization = i8257_dma_has_autoinitialization;
    idc->read_memory = i8257_dma_read_memory;
    idc->write_memory = i8257_dma_write_memory;
    idc->hold_DREQ = i8257_dma_hold_DREQ;
    idc->release_DREQ = i8257_dma_release_DREQ;
    idc->schedule = i8257_dma_schedule;
    idc->register_channel = i8257_dma_register_channel;
    /* Reason: needs to be wired up by isa_bus_dma() to work */
    dc->user_creatable = false;
}

static const TypeInfo i8257_info = {
    .name = TYPE_I8257,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(I8257State),
    .class_init = i8257_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_ISADMA },
        { }
    }
};

static void i8257_register_types(void)
{
    type_register_static(&i8257_info);
}

type_init(i8257_register_types)

void i8257_dma_init(Object *parent, ISABus *bus, bool high_page_enable)
{
    ISADevice *isa1, *isa2;
    DeviceState *d;

    isa1 = isa_new(TYPE_I8257);
    object_property_add_child(parent, "dma[*]", OBJECT(isa1));
    d = DEVICE(isa1);
    qdev_prop_set_int32(d, "base", 0x00);
    qdev_prop_set_int32(d, "page-base", 0x80);
    qdev_prop_set_int32(d, "pageh-base", high_page_enable ? 0x480 : -1);
    qdev_prop_set_int32(d, "dshift", 0);
    isa_realize_and_unref(isa1, bus, &error_fatal);

    isa2 = isa_new(TYPE_I8257);
    object_property_add_child(parent, "dma[*]", OBJECT(isa2));
    d = DEVICE(isa2);
    qdev_prop_set_int32(d, "base", 0xc0);
    qdev_prop_set_int32(d, "page-base", 0x88);
    qdev_prop_set_int32(d, "pageh-base", high_page_enable ? 0x488 : -1);
    qdev_prop_set_int32(d, "dshift", 1);
    isa_realize_and_unref(isa2, bus, &error_fatal);

    isa_bus_dma(bus, ISADMA(isa1), ISADMA(isa2));
}
