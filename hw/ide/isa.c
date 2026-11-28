/*
 * QEMU IDE Emulation: ISA Bus support.
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2006 Openedhand Ltd.
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
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/dma.h"
#include "system/block-backend.h"
#include "qemu/log.h"
#include "qemu/timer.h"

#include "hw/ide/isa.h"
#include "qom/object.h"
#include "ide-internal.h"

/***********************************************************/
/* ISA IDE definitions */

struct ISAIDEState {
    ISADevice parent_obj;

    IDEBus    bus;
    uint32_t  iobase;
    uint32_t  iobase2;
    uint32_t  irqnum;
    int32_t   dmanum;

    /*
     * DMA through an ISA DMA channel: the drive's DMARQ and DMACK are
     * wired to a channel of the system's DMA controller. READ and WRITE
     * DMA, and the data of ATAPI packets, go through a bounce buffer that
     * the channel fills or empties.
     */
    IDEDMA    dma;
    IsaDma    *isa_dma;
    IDEState  *dma_drive;       /* drive of the transfer, or NULL */
    uint8_t   *dma_buf;
    uint32_t  dma_len;          /* bytes in the transfer */
    uint32_t  dma_done;         /* bytes the channel has moved */
    bool      dma_to_memory;    /* the drive sends, the channel writes RAM */
    QEMUTimer *dma_timer;       /* the drive raises DMARQ */
};

/* How long the drive takes to raise DMARQ after a READ or WRITE DMA */
#define ISA_IDE_DMA_DELAY_NS    (100 * SCALE_US)

static void isa_ide_dma_stop(ISAIDEState *s)
{
    timer_del(s->dma_timer);
    if (s->dma_drive) {
        ISADMA_GET_CLASS(s->isa_dma)->release_DREQ(s->isa_dma, s->dmanum);
        s->dma_drive = NULL;
    }
    g_free(s->dma_buf);
    s->dma_buf = NULL;
}

static void isa_ide_dma_finish(ISAIDEState *s, int ret)
{
    IDEState *ide = s->dma_drive;
    int64_t sector = ide_get_sector(ide);

    isa_ide_dma_stop(s);
    if (ide->drive_kind == IDE_CD) {
        if (ret < 0) {
            block_acct_failed(blk_get_stats(ide->blk), &ide->acct);
            ide_atapi_io_error(ide, ret);
        } else {
            ide->packet_transfer_size = 0;
            ide->status = READY_STAT | SEEK_STAT;
            ide->nsector = (ide->nsector & ~7) | ATAPI_INT_REASON_IO |
                           ATAPI_INT_REASON_CD;
            ide_bus_set_irq(ide->bus);
            block_acct_done(blk_get_stats(ide->blk), &ide->acct);
        }
        ide_set_inactive(ide, false);
        return;
    }
    if (ret < 0) {
        block_acct_failed(blk_get_stats(ide->blk), &ide->acct);
        ide_dma_error(ide);
        return;
    }
    ide_set_sector(ide, sector + ide->nsector);
    ide->nsector = 0;
    ide->status = READY_STAT | SEEK_STAT;
    ide_bus_set_irq(ide->bus);
    block_acct_done(blk_get_stats(ide->blk), &ide->acct);
    ide_set_inactive(ide, false);
}

/* Read the sectors or take the reply of an ATAPI packet into the buffer */
static int isa_ide_atapi_fill(ISAIDEState *s, IDEState *ide)
{
    int i, n, ret;

    s->dma_len = MAX(ide->packet_transfer_size, 0);
    s->dma_buf = g_malloc0(MAX(s->dma_len, 1));
    if (ide->lba == -1) {
        /* not a read: the reply is in the drive's buffer */
        s->dma_len = MIN(s->dma_len, ide->io_buffer_total_len);
        memcpy(s->dma_buf, ide->io_buffer, s->dma_len);
        return 0;
    }
    if (ide->cd_sector_size == 2048) {
        return blk_pread(ide->blk, (int64_t)ide->lba * 2048, s->dma_len,
                         s->dma_buf, 0);
    }
    /* raw 2352-byte sectors, made from the 2048 bytes of data in each */
    n = s->dma_len / 2352;
    for (i = 0; i < n; i++) {
        ret = blk_pread(ide->blk, (int64_t)(ide->lba + i) * 2048, 2048,
                        s->dma_buf + i * 2352 + 16, 0);
        if (ret < 0) {
            return ret;
        }
        cd_data_to_raw(s->dma_buf + i * 2352, ide->lba + i);
    }
    return 0;
}

static void isa_ide_start_dma(const IDEDMA *dma, IDEState *ide,
                              BlockCompletionFunc *cb)
{
    ISAIDEState *s = container_of(dma, ISAIDEState, dma);
    int64_t sector = ide_get_sector(ide);
    int ret = 0;

    if (ide->drive_kind != IDE_CD &&
        ide->dma_cmd != IDE_DMA_READ && ide->dma_cmd != IDE_DMA_WRITE) {
        qemu_log_mask(LOG_UNIMP, "isa-ide: only READ/WRITE and ATAPI DMA\n");
        return;
    }

    isa_ide_dma_stop(s);
    s->dma_drive = ide;
    s->dma_done = 0;
    if (ide->drive_kind == IDE_CD) {
        s->dma_to_memory = true;
        ret = isa_ide_atapi_fill(s, ide);
        if (ret < 0 || s->dma_len == 0) {
            isa_ide_dma_finish(s, ret);
            return;
        }
    } else {
        s->dma_to_memory = ide->dma_cmd == IDE_DMA_READ;
        s->dma_len = ide->nsector * BDRV_SECTOR_SIZE;
        if (sector < 0 || sector + ide->nsector > ide->nb_sectors) {
            isa_ide_dma_finish(s, -EINVAL);
            return;
        }
        s->dma_buf = g_malloc(s->dma_len);
        if (s->dma_to_memory) {
            ret = blk_pread(ide->blk, sector * BDRV_SECTOR_SIZE, s->dma_len,
                            s->dma_buf, 0);
        }
        if (ret < 0) {
            isa_ide_dma_finish(s, ret);
            return;
        }
    }
    /*
     * Not at once: the guest finishes writing the command before the data
     * moves and the drive interrupts, as on a real drive.
     */
    timer_mod(s->dma_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ISA_IDE_DMA_DELAY_NS);
}

static void isa_ide_dma_request(void *opaque)
{
    ISAIDEState *s = opaque;

    if (s->dma_drive) {
        ISADMA_GET_CLASS(s->isa_dma)->hold_DREQ(s->isa_dma, s->dmanum);
    }
}

/* The DMA channel moves data: dma_pos..dma_len of its current buffer */
static int isa_ide_dma_transfer(void *opaque, int nchan, int dma_pos,
                                int dma_len)
{
    ISAIDEState *s = opaque;
    IsaDmaClass *k = ISADMA_GET_CLASS(s->isa_dma);
    IDEState *ide = s->dma_drive;
    int n;

    if (!ide) {
        return dma_pos;
    }
    n = MIN(dma_len - dma_pos, s->dma_len - s->dma_done);
    if (n > 0) {
        if (s->dma_to_memory) {
            k->write_memory(s->isa_dma, nchan, s->dma_buf + s->dma_done,
                            dma_pos, n);
        } else {
            k->read_memory(s->isa_dma, nchan, s->dma_buf + s->dma_done,
                           dma_pos, n);
        }
        s->dma_done += n;
        dma_pos += n;
    }
    if (s->dma_done == s->dma_len) {
        int ret = 0;

        if (!s->dma_to_memory) {
            ret = blk_pwrite(ide->blk, ide_get_sector(ide) * BDRV_SECTOR_SIZE,
                             s->dma_len, s->dma_buf, 0);
        }
        isa_ide_dma_finish(s, ret);
    }
    return dma_pos;
}

static void isa_ide_dma_reset(const IDEDMA *dma)
{
    isa_ide_dma_stop(container_of(dma, ISAIDEState, dma));
}

static const IDEDMAOps isa_ide_dma_ops = {
    .start_dma = isa_ide_start_dma,
    .reset = isa_ide_dma_reset,
};

static void isa_ide_reset(DeviceState *d)
{
    ISAIDEState *s = ISA_IDE(d);

    if (s->isa_dma) {
        isa_ide_dma_stop(s);
    }
    ide_bus_reset(&s->bus, IDE_RESET_HARDWARE);
}

static const VMStateDescription vmstate_ide_isa = {
    .name = "isa-ide",
    .version_id = 3,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_IDE_BUS(bus, ISAIDEState),
        VMSTATE_IDE_DRIVES(bus.ifs, ISAIDEState),
        VMSTATE_END_OF_LIST()
    }
};

static void isa_ide_realizefn(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    ISAIDEState *s = ISA_IDE(dev);

    ide_bus_init(&s->bus, sizeof(s->bus), dev, 0, 2);
    ide_init_ioport(&s->bus, isadev, s->iobase, s->iobase2);
    ide_bus_init_output_irq(&s->bus, isa_get_irq(isadev, s->irqnum));
    if (s->dmanum >= 0) {
        s->isa_dma = isa_bus_get_dma(isa_bus_from_device(isadev), s->dmanum);
        if (!s->isa_dma) {
            error_setg(errp, "isa-ide: no ISA DMA controller for channel %d",
                       s->dmanum);
            return;
        }
        ISADMA_GET_CLASS(s->isa_dma)->register_channel(s->isa_dma, s->dmanum,
                                                       isa_ide_dma_transfer,
                                                       s);
        s->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, isa_ide_dma_request, s);
        s->dma.ops = &isa_ide_dma_ops;
        s->bus.dma = &s->dma;
    }
    vmstate_register_any(VMSTATE_IF(dev), &vmstate_ide_isa, s);
    ide_bus_register_restart_cb(&s->bus);
}

ISADevice *isa_ide_init(ISABus *bus, int iobase, int iobase2, int irqnum,
                        DriveInfo *hd0, DriveInfo *hd1)
{
    DeviceState *dev;
    ISADevice *isadev;
    ISAIDEState *s;

    isadev = isa_new(TYPE_ISA_IDE);
    dev = DEVICE(isadev);
    qdev_prop_set_uint32(dev, "iobase",  iobase);
    qdev_prop_set_uint32(dev, "iobase2", iobase2);
    qdev_prop_set_uint32(dev, "irq",     irqnum);
    isa_realize_and_unref(isadev, bus, &error_fatal);

    s = ISA_IDE(dev);
    if (hd0) {
        ide_bus_create_drive(&s->bus, 0, hd0);
    }
    if (hd1) {
        ide_bus_create_drive(&s->bus, 1, hd1);
    }
    return isadev;
}

static const Property isa_ide_properties[] = {
    DEFINE_PROP_UINT32("iobase",  ISAIDEState, iobase,  0x1f0),
    DEFINE_PROP_UINT32("iobase2", ISAIDEState, iobase2, 0x3f6),
    DEFINE_PROP_UINT32("irq",     ISAIDEState, irqnum,  14),
    DEFINE_PROP_INT32("dma",      ISAIDEState, dmanum,  -1),
};

static void isa_ide_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = isa_ide_realizefn;
    dc->fw_name = "ide";
    device_class_set_legacy_reset(dc, isa_ide_reset);
    device_class_set_props(dc, isa_ide_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo isa_ide_info = {
    .name          = TYPE_ISA_IDE,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(ISAIDEState),
    .class_init    = isa_ide_class_initfn,
};

static void isa_ide_register_types(void)
{
    type_register_static(&isa_ide_info);
}

type_init(isa_ide_register_types)
