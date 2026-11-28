/*
 * Motorola MPC105 "Eagle" PCI bridge/memory controller
 *
 * Only address map A (the PReP map) is modelled, the one the BeBox uses:
 *
 *   0x00000000-0x3fffffff  RAM, decoded by the memory bank registers
 *   0x40000000-0x7fffffff  local bus slaves (PICR1[CF_LBA_EN]), board's
 *   0x80000000-0x807fffff  ISA/PCI I/O, contiguous or discontiguous
 *   0x80800000-0x80ffffff  direct-mapped PCI configuration
 *   0x81000000-0xbf7fffff  PCI I/O 0x01000000-0x3f7fffff
 *   0xbffffff0-0xbfffffff  PCI/ISA interrupt acknowledge
 *   0xc0000000-0xfeffffff  PCI memory 0x00000000-0x3effffff
 *   0xff000000-0xffffffff  ROM, board's
 *
 * PCI masters see RAM at PCI 0x80000000.
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_host.h"
#include "hw/pci-host/mpc105.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/i8259.h"
#include "hw/core/irq.h"
#include "hw/core/cpu.h"
#include "qom/object.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(MPC105State, MPC105_PCI_HOST_BRIDGE)

#define MPC105_RAM_WINDOW   (1 * GiB)
#define MPC105_MAX_CHUNKS   256

/* Configuration registers */
#define MPC105_MSAR1        0x80
#define MPC105_MSAR2        0x84
#define MPC105_EMSAR1       0x88
#define MPC105_EMSAR2       0x8c
#define MPC105_MEAR1        0x90
#define MPC105_MEAR2        0x94
#define MPC105_EMEAR1       0x98
#define MPC105_EMEAR2       0x9c
#define MPC105_MBEN         0xa0
#define MPC105_PICR1        0xa8
#define MPC105_PICR2        0xac
#define MPC105_ALT_OSV1     0xba
#define MPC105_ALT_OSV2     0xbb
#define MPC105_ERRENR1      0xc0
#define MPC105_ERRDR1       0xc1
#define MPC105_BUS_ESR      0xc3
#define MPC105_ERRENR2      0xc4
#define MPC105_ERRDR2       0xc5
#define MPC105_PCI_ESR      0xc7
#define MPC105_MCCR1        0xf0
#define MPC105_MCCR2        0xf4
#define MPC105_MCCR3        0xf8
#define MPC105_MCCR4        0xfc

#define PICR1_LE_MODE       0x00000020
#define PICR1_FLASH_WR_EN   0x00001000
#define PICR1_CF_MP_ID      0x00008000
#define PICR1_XATS          0x00010000
#define PICR1_XIO_MODE      0x00080000
#define PICR1_RCS0          0x00100000
#define PICR1_RO            0x0011c100

#define MCCR1_MEMGO         0x00080000
#define MCCR1_FNR           0x00400000
#define MCCR1_32N64         0x00200000

struct MPC105State {
    PCIHostState parent_obj;

    MemoryRegion *ram;
    PCIDevice *pci;

    qemu_irq irqs[MPC105_PCI_NUM_IRQS];
    qemu_irq flash_wr_en;

    AddressSpace pci_io_as;
    MemoryRegion pci_io;
    MemoryRegion isa_io;
    MemoryRegion pci_io_high;
    MemoryRegion pci_memory;
    MemoryRegion pci_memory_window;
    MemoryRegion intack;
    MemoryRegion ram_window;
    MemoryRegion chunks[MPC105_MAX_CHUNKS];
    unsigned nb_chunks;
    MemoryRegion bm;
    MemoryRegion bm_ram;
    MemoryRegion bm_pci_memory;
    AddressSpace bm_as;
};

static inline uint32_t mpc105_reg(MPC105State *s, int reg)
{
    return pci_get_long(s->pci->config + reg);
}

/*
 * Lay RAM out over the enabled banks, in bank order. A bank bigger than
 * what is left of RAM mirrors it, and banks past the end of RAM stay
 * empty, which is how the boot code finds the size of each bank.
 */
static void mpc105_update_ram(MPC105State *s)
{
    uint64_t start = mpc105_reg(s, MPC105_MSAR1) |
                     (uint64_t)mpc105_reg(s, MPC105_MSAR2) << 32;
    uint64_t xstart = mpc105_reg(s, MPC105_EMSAR1) |
                      (uint64_t)mpc105_reg(s, MPC105_EMSAR2) << 32;
    uint64_t end = mpc105_reg(s, MPC105_MEAR1) |
                   (uint64_t)mpc105_reg(s, MPC105_MEAR2) << 32;
    uint64_t xend = mpc105_reg(s, MPC105_EMEAR1) |
                    (uint64_t)mpc105_reg(s, MPC105_EMEAR2) << 32;
    uint8_t enable = s->pci->config[MPC105_MBEN];
    bool memgo = mpc105_reg(s, MPC105_MCCR1) & MCCR1_MEMGO;
    uint64_t ram_size = s->ram ? memory_region_size(s->ram) : 0;
    uint64_t placed = 0;
    unsigned i;
    int bank;

    memory_region_transaction_begin();
    for (i = 0; i < s->nb_chunks; i++) {
        memory_region_del_subregion(&s->ram_window, &s->chunks[i]);
        object_unparent(OBJECT(&s->chunks[i]));
    }
    s->nb_chunks = 0;

    for (bank = 0; memgo && bank < 8; bank++) {
        int sh = bank * 8;
        uint32_t bstart = ((start >> sh) & 0xff) << 20 |
                          ((xstart >> sh) & 0x3) << 28;
        uint32_t bend = ((end >> sh) & 0xff) << 20 |
                        ((xend >> sh) & 0x3) << 28 | 0xfffff;
        uint64_t left, at, off;

        if (!(enable & (1 << bank)) || bend < bstart ||
            placed >= ram_size) {
            continue;
        }
        trace_mpc105_ram_bank(bank, bstart, bend, placed);

        left = (uint64_t)bend - bstart + 1;
        at = bstart;
        off = placed;
        placed += left;
        while (left && s->nb_chunks < MPC105_MAX_CHUNKS) {
            uint64_t len = MIN(left, ram_size - off);
            MemoryRegion *mr = &s->chunks[s->nb_chunks];
            g_autofree char *name = g_strdup_printf("mpc105-bank%d.%u", bank,
                                                    s->nb_chunks);

            memory_region_init_alias(mr, OBJECT(s), name, s->ram, off, len);
            memory_region_add_subregion(&s->ram_window, at, mr);
            s->nb_chunks++;
            at += len;
            left -= len;
            off = 0;
        }
    }
    memory_region_transaction_commit();
}

static void mpc105_update_picr1(MPC105State *s)
{
    uint32_t picr1 = mpc105_reg(s, MPC105_PICR1);

    if (picr1 & PICR1_LE_MODE) {
        qemu_log_mask(LOG_UNIMP, "mpc105: little-endian mode\n");
    }
    qemu_set_irq(s->flash_wr_en, !!(picr1 & PICR1_FLASH_WR_EN));
}

/* ISA/PCI I/O, 0x80000000-0x807fffff */
static hwaddr mpc105_io_address(MPC105State *s, hwaddr addr)
{
    if (mpc105_reg(s, MPC105_PICR1) & PICR1_XIO_MODE) {
        /* Discontiguous: 32 ports for each 4 KiB page */
        return (addr & 0x1f) | ((addr & 0x7ff000) >> 7);
    }
    return addr;
}

static uint64_t mpc105_io_read(void *opaque, hwaddr addr, unsigned size)
{
    MPC105State *s = opaque;
    uint8_t buf[4];

    /* Nobody answers on ISA: the bus floats high */
    if (address_space_read(&s->pci_io_as, mpc105_io_address(s, addr),
                           MEMTXATTRS_UNSPECIFIED, buf, size) != MEMTX_OK) {
        return MAKE_64BIT_MASK(0, size * 8);
    }
    switch (size) {
    case 1:
        return buf[0];
    case 2:
        return lduw_le_p(buf);
    case 4:
        return ldl_le_p(buf);
    default:
        g_assert_not_reached();
    }
}

static void mpc105_io_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MPC105State *s = opaque;
    uint8_t buf[4];

    switch (size) {
    case 1:
        buf[0] = val;
        break;
    case 2:
        stw_le_p(buf, val);
        break;
    case 4:
        stl_le_p(buf, val);
        break;
    default:
        g_assert_not_reached();
    }
    address_space_write(&s->pci_io_as, mpc105_io_address(s, addr),
                        MEMTXATTRS_UNSPECIFIED, buf, size);
}

static const MemoryRegionOps mpc105_io_ops = {
    .read = mpc105_io_read,
    .write = mpc105_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
    .valid.unaligned = true,
};

/*
 * Direct-mapped configuration: address bit n (11 to 22) drives AD line n,
 * the IDSEL of PCI device n.
 */
static inline uint32_t mpc105_idsel_to_addr(hwaddr addr)
{
    return ((ctz32(addr >> 11) + 11) << 11) | (addr & 0x7ff);
}

static void mpc105_mmcfg_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    PCIBus *bus = opaque;

    if (!(addr >> 11)) {
        return;
    }
    pci_data_write(bus, mpc105_idsel_to_addr(addr), val, size);
}

static uint64_t mpc105_mmcfg_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIBus *bus = opaque;

    if (!(addr >> 11)) {
        return MAKE_64BIT_MASK(0, size * 8);
    }
    return pci_data_read(bus, mpc105_idsel_to_addr(addr), size);
}

static const MemoryRegionOps mpc105_mmcfg_ops = {
    .read = mpc105_mmcfg_read,
    .write = mpc105_mmcfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t mpc105_intack_read(void *opaque, hwaddr addr, unsigned size)
{
    return pic_read_irq(isa_pic);
}

static void mpc105_intack_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "mpc105: write to interrupt acknowledge\n");
}

static const MemoryRegionOps mpc105_intack_ops = {
    .read = mpc105_intack_read,
    .write = mpc105_intack_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.max_access_size = 1,
};

/* One interrupt line per PCI device 12-15: the BeBox has no shared lines */
static int mpc105_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return (PCI_SLOT(pci_dev->devfn) - MPC105_PCI_IRQ_FIRST_DEV) &
           (MPC105_PCI_NUM_IRQS - 1);
}

static void mpc105_set_irq(void *opaque, int irq_num, int level)
{
    MPC105State *s = opaque;

    qemu_set_irq(s->irqs[irq_num], level);
}

static AddressSpace *mpc105_get_address_space(PCIBus *bus, void *opaque,
                                              int devfn)
{
    MPC105State *s = opaque;

    return &s->bm_as;
}

static const PCIIOMMUOps mpc105_iommu_ops = {
    .get_address_space = mpc105_get_address_space,
};

static void mpc105_pcihost_realize(DeviceState *d, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(d);
    PCIHostState *h = PCI_HOST_BRIDGE(d);
    MPC105State *s = MPC105_PCI_HOST_BRIDGE(d);
    MemoryRegion *sysmem = get_system_memory();
    Object *obj = OBJECT(d);
    int i;

    if (!s->ram) {
        error_setg(errp, "mpc105: 'ram' link is not set");
        return;
    }

    memory_region_init(&s->pci_io, obj, "pci-io", 4 * GiB);
    memory_region_init(&s->pci_memory, obj, "pci-memory", 4 * GiB);
    address_space_init(&s->pci_io_as, &s->pci_io, "mpc105-io");

    /* RAM, filled in by the bank registers */
    memory_region_init(&s->ram_window, obj, "mpc105-ram", MPC105_RAM_WINDOW);
    memory_region_add_subregion(sysmem, 0, &s->ram_window);

    /*
     * The I/O window goes through the address space API to reach the
     * configuration ports, which this device also owns.
     */
    memory_region_init_io(&s->isa_io, obj, &mpc105_io_ops, s, "mpc105-isa-io",
                          0x00800000);
    s->isa_io.disable_reentrancy_guard = true;
    memory_region_add_subregion(sysmem, 0x80000000, &s->isa_io);

    memory_region_init_alias(&s->pci_io_high, obj, "mpc105-pci-io",
                             &s->pci_io, 0x01000000, 0x3e800000);
    memory_region_add_subregion(sysmem, 0x81000000, &s->pci_io_high);

    memory_region_init_alias(&s->pci_memory_window, obj, "mpc105-pci-memory",
                             &s->pci_memory, 0, 0x3f000000);
    memory_region_add_subregion(sysmem, 0xc0000000, &s->pci_memory_window);

    memory_region_init_io(&s->intack, obj, &mpc105_intack_ops, s,
                          "mpc105-intack", 16);
    memory_region_add_subregion(sysmem, 0xbffffff0, &s->intack);

    for (i = 0; i < MPC105_PCI_NUM_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }
    qdev_init_gpio_out(d, &s->flash_wr_en, 1);

    h->bus = pci_register_root_bus(d, NULL, mpc105_set_irq, mpc105_map_irq,
                                   s, &s->pci_memory, &s->pci_io, 0,
                                   MPC105_PCI_NUM_IRQS, TYPE_PCI_BUS);

    memory_region_init_io(&h->conf_mem, obj, &pci_host_conf_le_ops, s,
                          "pci-conf-idx", 4);
    memory_region_add_subregion(&s->pci_io, 0xcf8, &h->conf_mem);
    memory_region_init_io(&h->data_mem, obj, &pci_host_data_le_ops, s,
                          "pci-conf-data", 4);
    memory_region_add_subregion(&s->pci_io, 0xcfc, &h->data_mem);

    memory_region_init_io(&h->mmcfg, obj, &mpc105_mmcfg_ops, h->bus,
                          "pci-mmcfg", 0x00800000);
    memory_region_add_subregion(sysmem, 0x80800000, &h->mmcfg);

    /* PCI masters see RAM at 0x80000000 */
    memory_region_init(&s->bm, obj, "bm-mpc105", 4 * GiB);
    memory_region_init_alias(&s->bm_pci_memory, obj, "bm-pci-memory",
                             &s->pci_memory, 0, 0x80000000);
    memory_region_init_alias(&s->bm_ram, obj, "bm-system", &s->ram_window, 0,
                             MPC105_RAM_WINDOW);
    memory_region_add_subregion(&s->bm, 0, &s->bm_pci_memory);
    memory_region_add_subregion(&s->bm, 0x80000000, &s->bm_ram);
    address_space_init(&s->bm_as, &s->bm, "mpc105-bm");
    pci_setup_iommu(h->bus, &mpc105_iommu_ops, s);

    s->pci = pci_new(PCI_DEVFN(0, 0), TYPE_MPC105_PCI_DEVICE);
    pci_realize_and_unref(s->pci, h->bus, &error_fatal);
}

static const Property mpc105_pcihost_properties[] = {
    DEFINE_PROP_LINK("ram", MPC105State, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void mpc105_pcihost_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mpc105_pcihost_realize;
    device_class_set_props(dc, mpc105_pcihost_properties);
    dc->fw_name = "pci";
    dc->user_creatable = false;
}

/* The PCI function: the configuration registers */

static MPC105State *mpc105_host(PCIDevice *d)
{
    return MPC105_PCI_HOST_BRIDGE(pci_get_bus(d)->qbus.parent);
}

static bool ranges_overlap_reg(uint32_t addr, int len, uint32_t reg,
                               int reg_len)
{
    return addr < reg + reg_len && reg < addr + len;
}

static uint32_t mpc105_config_read(PCIDevice *d, uint32_t addr, int len)
{
    uint32_t val = pci_default_read_config(d, addr, len);

    /* CF_MP_ID tells which processor is reading */
    if (ranges_overlap_reg(addr, len, MPC105_PICR1 + 1, 1) &&
        current_cpu && current_cpu->cpu_index == 1) {
        val |= (PICR1_CF_MP_ID >> 8) << ((MPC105_PICR1 + 1 - addr) * 8);
    }
    return val;
}

static void mpc105_config_write(PCIDevice *d, uint32_t addr, uint32_t val,
                                int len)
{
    MPC105State *s = mpc105_host(d);

    trace_mpc105_config_write(addr, val, len);
    pci_default_write_config(d, addr, val, len);

    if (ranges_overlap_reg(addr, len, MPC105_MSAR1, MPC105_MBEN + 1 -
                           MPC105_MSAR1) ||
        ranges_overlap_reg(addr, len, MPC105_MCCR1, 4)) {
        mpc105_update_ram(s);
    }
    if (ranges_overlap_reg(addr, len, MPC105_PICR1, 4)) {
        mpc105_update_picr1(s);
    }
}

static void mpc105_pci_reset(DeviceState *dev)
{
    PCIDevice *d = PCI_DEVICE(dev);
    uint8_t *c = d->config;
    int i;

    pci_set_word(c + PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    pci_set_word(c + PCI_STATUS, PCI_STATUS_FAST_BACK);
    for (i = MPC105_MSAR1; i <= MPC105_MBEN; i++) {
        c[i] = 0;
    }
    /* Straps: ROM on the 60x bus, map A */
    pci_set_long(c + MPC105_PICR1, 0xff000010 | PICR1_RCS0 | PICR1_XATS);
    pci_set_long(c + MPC105_PICR2, 0x000c060c);
    c[MPC105_ALT_OSV1] = 0x04;
    c[MPC105_ALT_OSV2] = 0x00;
    c[MPC105_ERRENR1] = 0x01;
    c[MPC105_ERRDR1] = 0;
    c[MPC105_BUS_ESR] = 0;
    c[MPC105_ERRENR2] = 0;
    c[MPC105_ERRDR2] = 0;
    c[MPC105_PCI_ESR] = 0;
    pci_set_long(c + 0xc8, 0);
    /* Straps: 8-bit flash, 64-bit data bus */
    pci_set_long(c + MPC105_MCCR1, 0xff020000 | MCCR1_FNR);
    pci_set_long(c + MPC105_MCCR2, 0x00000003);
    pci_set_long(c + MPC105_MCCR3, 0x00000000);
    pci_set_long(c + MPC105_MCCR4, 0x00100000);

    if (pci_get_bus(d)) {
        MPC105State *s = mpc105_host(d);

        mpc105_update_ram(s);
        mpc105_update_picr1(s);
    }
}

static void mpc105_pci_realize(PCIDevice *d, Error **errp)
{
    uint8_t *w = d->wmask;
    uint8_t *w1c = d->w1cmask;
    int i;

    /* The standard header is read-only, apart from command and status */
    memset(w, 0, PCI_CONFIG_SPACE_SIZE);
    pci_set_word(w + PCI_COMMAND, 0xffff);
    pci_set_word(w1c + PCI_STATUS, 0xf900);

    pci_set_word(w + 0x70, 0xffff);
    for (i = MPC105_MSAR1; i < MPC105_MBEN; i++) {
        w[i] = 0xff;
    }
    /* The extended address registers only have two bits per bank */
    for (i = MPC105_EMSAR1; i < MPC105_EMSAR1 + 8; i++) {
        w[i] = 0x03;
    }
    for (i = MPC105_EMEAR1; i < MPC105_EMEAR1 + 8; i++) {
        w[i] = 0x03;
    }
    w[MPC105_MBEN] = 0xff;
    pci_set_long(w + MPC105_PICR1, ~PICR1_RO);
    pci_set_long(w + MPC105_PICR2, ~0x2c008800);
    w[MPC105_ALT_OSV1] = 0xff;
    w[MPC105_ALT_OSV2] = 0xff;
    w[MPC105_ERRENR1] = 0xff;
    w1c[MPC105_ERRDR1] = 0xff;
    w1c[MPC105_BUS_ESR] = 0xff;
    w[MPC105_ERRENR2] = 0xff;
    w1c[MPC105_ERRDR2] = 0xff;
    w1c[MPC105_PCI_ESR] = 0xff;
    pci_set_long(w + MPC105_MCCR1, ~(MCCR1_FNR | MCCR1_32N64));
    pci_set_long(w + MPC105_MCCR2, 0xfffcffff);
    pci_set_long(w + MPC105_MCCR3, 0xfff7ffff);
    pci_set_long(w + MPC105_MCCR4, 0xff3fffff);

    mpc105_pci_reset(DEVICE(d));
}

static void mpc105_pci_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize = mpc105_pci_realize;
    k->config_read = mpc105_config_read;
    k->config_write = mpc105_config_write;
    k->vendor_id = PCI_VENDOR_ID_MOTOROLA;
    k->device_id = PCI_DEVICE_ID_MOTOROLA_MPC105;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    device_class_set_legacy_reset(dc, mpc105_pci_reset);
    dc->desc = "Motorola MPC105 PCI Bridge/Memory Controller";
    /* Reason: part of the host bridge, created by it */
    dc->user_creatable = false;
}

static const TypeInfo mpc105_types[] = {
    {
        .name = TYPE_MPC105_PCI_HOST_BRIDGE,
        .parent = TYPE_PCI_HOST_BRIDGE,
        .instance_size = sizeof(MPC105State),
        .class_init = mpc105_pcihost_class_init,
    },
    {
        .name = TYPE_MPC105_PCI_DEVICE,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(PCIDevice),
        .class_init = mpc105_pci_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(mpc105_types)
