/*
 * QEMU SiI3112A and SiI3114 PCI to Serial ATA Controller Emulation
 *
 * Copyright (C) 2017 BALATON Zoltan <balaton@eik.bme.hu>
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* For documentation on this and similar cards see:
 * http://wiki.osdev.org/User:Quok/Silicon_Image_Datasheets
 *
 * The SiI3114 is the four port version of the SiI3112: ports 0 and 1 are
 * where the SiI3112 has them, ports 2 and 3 are 0x200 above them in BAR5.
 */

#include "qemu/osdep.h"
#include "hw/ide/pci.h"
#include "qemu/module.h"
#include "trace.h"
#include "qom/object.h"
#include "ide-internal.h"

#define TYPE_SII3112_PCI "sii3112"
#define TYPE_SII3114_PCI "sii3114"
OBJECT_DECLARE_TYPE(SiI3112PCIState, SiI3112PCIClass, SII3112_PCI)

#define SII_MAX_PORTS 4

typedef struct SiI3112Regs {
    uint32_t confstat;
    uint32_t scontrol;
    uint16_t sien;
    uint8_t swdata;
} SiI3112Regs;

struct SiI3112PCIState {
    PCIIDEState i;
    MemoryRegion mmio;
    SiI3112Regs regs[SII_MAX_PORTS];
    /* ports 2 and 3 of the SiI3114 */
    IDEBus bus23[2];
    BMDMAState bmdma23[2];
};

struct SiI3112PCIClass {
    PCIDeviceClass parent_class;
    int ports;
};

/* Where each port's registers are in BAR5 */
static const struct {
    hwaddr bmdma;   /* bus master DMA: command, status, PRD address */
    hwaddr bmdma2;  /* the same with the SATA interrupt bits */
    hwaddr tf;      /* task file; device control at +0xa, status at +0x20 */
    hwaddr scr;     /* SControl, SStatus, SError; SIEN at +0x48 */
} sii_port[SII_MAX_PORTS] = {
    { 0x000, 0x010, 0x080, 0x100 },
    { 0x008, 0x018, 0x0c0, 0x180 },
    { 0x200, 0x210, 0x280, 0x300 },
    { 0x208, 0x218, 0x2c0, 0x380 },
};

static int sii_ports(SiI3112PCIState *d)
{
    return SII3112_PCI_GET_CLASS(d)->ports;
}

static IDEBus *sii_bus(SiI3112PCIState *d, int port)
{
    return port < 2 ? &d->i.bus[port] : &d->bus23[port - 2];
}

static BMDMAState *sii_bmdma(SiI3112PCIState *d, int port)
{
    return port < 2 ? &d->i.bmdma[port] : &d->bmdma23[port - 2];
}

/*
 * Find the port and the kind of register an address of BAR5 belongs to.
 * Returns the port, or -1; *off is the offset in the register block.
 */
enum { SII_BMDMA, SII_BMDMA2, SII_TF, SII_SCR };

static int sii_decode(SiI3112PCIState *d, hwaddr addr, int *kind, hwaddr *off)
{
    for (int p = 0; p < sii_ports(d); p++) {
        if (addr >= sii_port[p].bmdma && addr < sii_port[p].bmdma + 8) {
            *kind = SII_BMDMA;
            *off = addr - sii_port[p].bmdma;
            return p;
        }
        if (addr >= sii_port[p].bmdma2 && addr < sii_port[p].bmdma2 + 8) {
            *kind = SII_BMDMA2;
            *off = addr - sii_port[p].bmdma2;
            return p;
        }
        if (addr >= sii_port[p].tf && addr < sii_port[p].tf + 0x40) {
            *kind = SII_TF;
            *off = addr - sii_port[p].tf;
            return p;
        }
        if (addr >= sii_port[p].scr && addr < sii_port[p].scr + 0x80) {
            *kind = SII_SCR;
            *off = addr - sii_port[p].scr;
            return p;
        }
    }
    return -1;
}

static bool sii_irq_pending(SiI3112PCIState *d, int port)
{
    return d->regs[port].confstat & (1UL << 11);
}

/* The sii3112_reg_read and sii3112_reg_write functions implement the
 * Internal Register Space - BAR5 (section 6.7 of the data sheet).
 */

static uint64_t sii3112_reg_read(void *opaque, hwaddr addr,
                                unsigned int size)
{
    SiI3112PCIState *d = opaque;
    BMDMAState *bm;
    uint64_t val = 0;
    hwaddr off;
    int kind, p;

    p = sii_decode(d, addr, &kind, &off);
    if (p < 0) {
        trace_sii3112_read(size, addr, 0);
        return 0;
    }
    bm = sii_bmdma(d, p);

    switch (kind) {
    case SII_BMDMA:
        switch (off) {
        case 0:
            val = bm->cmd;
            break;
        case 1:
            val = d->regs[p].swdata;
            break;
        case 2:
            val = bm->status;
            break;
        case 4 ... 7:
            val = bmdma_addr_ioport_ops.read(bm, off - 4, size);
            break;
        }
        break;
    case SII_BMDMA2:
        if (off != 0) {
            break;
        }
        val = bm->cmd;
        val |= sii_irq_pending(d, p) ? (1 << 4) : 0;    /* SATAINT */
        val |= (uint32_t)bm->status << 16;
        if ((p & 1) == 0) {
            /* the even port also shows its odd neighbour */
            BMDMAState *bm1 = sii_bmdma(d, p + 1);

            val |= sii_irq_pending(d, p + 1) ? (1 << 6) : 0;
            val |= (bm1->status & BM_STATUS_INT) ? (1 << 14) : 0;
            val |= (uint32_t)bm1->status << 24;
        }
        break;
    case SII_TF:
        if (off < 8) {
            val = pci_ide_data_le_ops.read(sii_bus(d, p), off, size);
        } else if (off == 0xa) {
            val = pci_ide_cmd_le_ops.read(sii_bus(d, p), 2, size);
        } else if (off >= 0x20 && off < 0x24) {
            /* drivers read its bytes too, such as the interrupt bit */
            val = d->regs[p].confstat >> ((off & 3) * 8);
        }
        break;
    case SII_SCR:
        switch (off & ~3) {
        case 0x00:
            val = d->regs[p].scontrol;
            break;
        case 0x04:
            /* SStatus: a device at 1.5 Gb/s, or nothing */
            val = sii_bus(d, p)->ifs[0].blk ? 0x113 : 0;
            break;
        case 0x48:
            val = (uint32_t)d->regs[p].sien << 16;
            break;
        }
        /* the 32-bit registers here can be read a byte at a time */
        val >>= (off & 3) * 8;
        break;
    }
    if (size < 4) {
        val &= (1ULL << (size * 8)) - 1;
    }
    trace_sii3112_read(size, addr, val);
    return val;
}

static void sii3112_reg_write(void *opaque, hwaddr addr,
                              uint64_t val, unsigned int size)
{
    SiI3112PCIState *d = opaque;
    BMDMAState *bm;
    hwaddr off;
    int kind, p;

    trace_sii3112_write(size, addr, val);
    p = sii_decode(d, addr, &kind, &off);
    if (p < 0) {
        return;
    }
    bm = sii_bmdma(d, p);

    switch (kind) {
    case SII_BMDMA:
    case SII_BMDMA2:
        switch (off) {
        case 0:
            bmdma_cmd_writeb(bm, val);
            break;
        case 1:
            d->regs[p].swdata = val & 0x3f;
            break;
        case 2:
            bmdma_status_writeb(bm, val);
            break;
        case 4 ... 7:
            if (kind == SII_BMDMA) {
                bmdma_addr_ioport_ops.write(bm, off - 4, val, size);
            }
            break;
        }
        break;
    case SII_TF:
        if (off < 8) {
            pci_ide_data_le_ops.write(sii_bus(d, p), off, val, size);
        } else if (off == 0xa) {
            pci_ide_cmd_le_ops.write(sii_bus(d, p), 2, val, size);
        }
        break;
    case SII_SCR:
        switch (off) {
        case 0x00:
            d->regs[p].scontrol = val & 0xfff;
            if (val & 1) {
                ide_bus_reset(sii_bus(d, p), IDE_RESET_HARDWARE);
            }
            break;
        case 0x48:
            d->regs[p].sien = (val >> 16) & 0x3eed;
            break;
        }
        break;
    }
}

static const MemoryRegionOps sii3112_reg_ops = {
    .read = sii3112_reg_read,
    .write = sii3112_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* the PCI irq level is the logical OR of the channels */
static void sii3112_update_irq(SiI3112PCIState *s)
{
    int i, set = 0;

    for (i = 0; i < sii_ports(s); i++) {
        set |= sii_irq_pending(s, i);
    }
    pci_set_irq(PCI_DEVICE(s), (set ? 1 : 0));
}

static void sii3112_set_irq(void *opaque, int channel, int level)
{
    SiI3112PCIState *s = opaque;

    trace_sii3112_set_irq(channel, level);
    if (level) {
        s->regs[channel].confstat |= (1UL << 11);
    } else {
        s->regs[channel].confstat &= ~(1UL << 11);
    }

    sii3112_update_irq(s);
}

static void sii3112_reset(DeviceState *dev)
{
    SiI3112PCIState *s = SII3112_PCI(dev);
    int i;

    for (i = 0; i < sii_ports(s); i++) {
        s->regs[i].confstat = 0x6515 << 16;
        ide_bus_reset(sii_bus(s, i), IDE_RESET_HARDWARE);
    }
}

static void sii3112_pci_realize(PCIDevice *dev, Error **errp)
{
    SiI3112PCIState *d = SII3112_PCI(dev);
    PCIIDEState *s = PCI_IDE(dev);
    DeviceState *ds = DEVICE(dev);
    int ports = sii_ports(d);
    MemoryRegion *mr;
    int i;

    pci_config_set_interrupt_pin(dev->config, 1);
    pci_set_byte(dev->config + PCI_CACHE_LINE_SIZE, 8);

    /* BAR5 is in PCI memory space */
    memory_region_init_io(&d->mmio, OBJECT(d), &sii3112_reg_ops, d,
                         "sii3112.bar5", ports > 2 ? 0x400 : 0x200);
    pci_register_bar(dev, 5, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);

    /* BAR0-BAR4 are PCI I/O space aliases into BAR5, for ports 0 and 1 */
    mr = g_new(MemoryRegion, 1);
    memory_region_init_alias(mr, OBJECT(d), "sii3112.bar0", &d->mmio, 0x80, 8);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_alias(mr, OBJECT(d), "sii3112.bar1", &d->mmio, 0x88, 4);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_alias(mr, OBJECT(d), "sii3112.bar2", &d->mmio, 0xc0, 8);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_IO, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_alias(mr, OBJECT(d), "sii3112.bar3", &d->mmio, 0xc8, 4);
    pci_register_bar(dev, 3, PCI_BASE_ADDRESS_SPACE_IO, mr);
    mr = g_new(MemoryRegion, 1);
    memory_region_init_alias(mr, OBJECT(d), "sii3112.bar4", &d->mmio, 0, 16);
    pci_register_bar(dev, 4, PCI_BASE_ADDRESS_SPACE_IO, mr);

    qdev_init_gpio_in(ds, sii3112_set_irq, ports);
    for (i = 0; i < ports; i++) {
        IDEBus *bus = sii_bus(d, i);

        ide_bus_init(bus, sizeof(*bus), ds, i, 1);
        ide_bus_init_output_irq(bus, qdev_get_gpio_in(ds, i));

        bmdma_init(bus, sii_bmdma(d, i), s);
        ide_bus_register_restart_cb(bus);
    }
}

static void sii3112_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pd = PCI_DEVICE_CLASS(klass);
    SiI3112PCIClass *sc = SII3112_PCI_CLASS(klass);

    pd->vendor_id = 0x1095;
    pd->device_id = 0x3112;
    pd->class_id = PCI_CLASS_STORAGE_RAID;
    pd->revision = 1;
    pd->realize = sii3112_pci_realize;
    sc->ports = 2;
    device_class_set_legacy_reset(dc, sii3112_reset);
    dc->desc = "SiI3112A SATA controller";
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static void sii3114_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pd = PCI_DEVICE_CLASS(klass);
    SiI3112PCIClass *sc = SII3112_PCI_CLASS(klass);

    pd->device_id = 0x3114;
    pd->revision = 2;
    sc->ports = 4;
    dc->desc = "SiI3114 SATA controller";
}

static const TypeInfo sii3112_pci_info = {
    .name = TYPE_SII3112_PCI,
    .parent = TYPE_PCI_IDE,
    .instance_size = sizeof(SiI3112PCIState),
    .class_size = sizeof(SiI3112PCIClass),
    .class_init = sii3112_pci_class_init,
};

static const TypeInfo sii3114_pci_info = {
    .name = TYPE_SII3114_PCI,
    .parent = TYPE_SII3112_PCI,
    .class_init = sii3114_pci_class_init,
};

static void sii3112_register_types(void)
{
    type_register_static(&sii3112_pci_info);
    type_register_static(&sii3114_pci_info);
}

type_init(sii3112_register_types)
