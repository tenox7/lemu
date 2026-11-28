/*
 * ACube Sam440ep: a PowerPC 440EP board that runs AmigaOS 4.1
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the sam460ex and bamboo boards.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 *
 * The board, after the ACube U-Boot source (u-boot-2015.d, Sam440ep):
 *
 *   0x00000000  DDR SDRAM, up to 1 GB
 *   0xa0000000  PCI memory
 *   0xe8000000  PCI io
 *   0xeec00000  PCI config
 *   0xef600000  on-chip peripherals: UARTs, I2C, GPIO, EMACs, ZMII
 *   0xfff00000  boot flash, U-Boot in its top 512 KB
 *
 * PCI: a PCI-PCI bridge at 0a, the Radeon Mobility M9 at 0c and the CS4281
 * sound chip at 0e; behind the bridge the SiI3114 SATA at 04, the USB
 * controller at 05 and the slots at 06, 07 and 08. The interrupts of all of
 * them go to five UIC inputs: 25, 26, 27, 29 and 50 (UIC1 18).
 *
 * The two on-chip EMACs work through the MAL and are the default NICs
 * (ppc440ep_eth.device); a 3c905c (eth3com.device) goes in PCI slot 07.
 * The Radeon is an rv250 (PCI 1002:4c66), the M9's ID, which ACube's U-Boot
 * looks for; AmigaOS composites windows, menus and the AmiDock with its
 * R200 3D engine.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/block-backend.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "ppc440.h"
#include "hw/ppc/ppc.h"
#include "hw/ppc/ppc4xx.h"
#include "hw/pci-host/ppc4xx.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bridge.h"
#include "hw/block/flash.h"
#include "hw/char/serial-mm.h"
#include "hw/i2c/ppc4xx_i2c.h"
#include "hw/i2c/smbus_eeprom.h"
#include "hw/ide/pci.h"
#include "hw/usb/usb.h"
#include "hw/intc/ppc-uic.h"
#include "net/net.h"

#define SAM440EP_FIRMWARE   "u-boot-sam440ep.bin"

#define FLASH_BASE          0xfff00000
#define FLASH_SIZE          (1 * MiB)
#define UBOOT_LOAD_BASE     0xfff80000
#define UBOOT_ENTRY         0xfffffffc

#define PCI_CONFIG_BASE     0xeec00000
#define SAM_PCI_MEM_BASE    0xa0000000
#define SAM_PCI_MEM_SIZE    0x40000000

#define INIT_RAM_BASE       0x70000000
#define INIT_RAM_SIZE       (64 * KiB)

#define SAM_PCI_IO_BASE         0xe8000000
#define SAM_PCI_IO_SIZE         0x00010000

/* 66.67 MHz in, a 1333 MHz VCO: 667 MHz CPU, 133 MHz PLB, 66 MHz OPB */
#define SYS_CLK             66666666
#define CPU_FREQ            666666666
#define PLB_FREQ            133333333

/* PCI devfns on the board */
#define SAM_DEV_BRIDGE      0x0a
#define SAM_DEV_RADEON      0x0c
#define SAM_DEV_AUDIO       0x0e
#define SAM_DEV_SATA        0x04    /* behind the bridge */
#define SAM_DEV_USB         0x05    /* behind the bridge */
#define SAM_DEV_SLOT1       0x06    /* behind the bridge, slots 06-08 */

/* The five UIC inputs the PCI interrupts go to */
enum { SAM_IRQ25, SAM_IRQ26, SAM_IRQ27, SAM_IRQ29, SAM_IRQ50, SAM_PCI_IRQS };

/*
 * 440EP clocks: CPR0 and SDR0 are indirect DCRs, an address register and a
 * data register. Only the registers U-Boot reads to work out the clocks
 * have fixed values; the rest keep what is written.
 */
#define CPR0_DCR_BASE       0x0c
#define SDR0_DCR_BASE       0x0e

#define CPR0_PLLC           0x040
#define CPR0_PLLD           0x060
#define CPR0_PRIMAD0        0x080
#define CPR0_PRIMBD0        0x0a0
#define CPR0_OPBD0          0x0c0
#define CPR0_PERD           0x0e0
#define CPR0_SPCID          0x120

#define SDR0_SDSTP0         0x020
#define SDR0_SDSTP1         0x021

typedef struct Sam440epIndirectDcr {
    uint32_t addr;
    GHashTable *regs;
} Sam440epIndirectDcr;

static uint32_t sam_ind_get(Sam440epIndirectDcr *d, uint32_t reg)
{
    gpointer v = g_hash_table_lookup(d->regs, GUINT_TO_POINTER(reg));

    return GPOINTER_TO_UINT(v);
}

static void sam_ind_set(Sam440epIndirectDcr *d, uint32_t reg, uint32_t val)
{
    g_hash_table_insert(d->regs, GUINT_TO_POINTER(reg), GUINT_TO_POINTER(val));
}

static uint32_t sam_cpr_read(void *opaque, int dcrn)
{
    Sam440epIndirectDcr *d = opaque;

    if (dcrn == CPR0_DCR_BASE) {
        return d->addr;
    }
    switch (d->addr) {
    case CPR0_PLLC:
        return 0;                       /* feedback from PLL output A */
    case CPR0_PLLD:
        return 0x0a020501;              /* FBDV 10, FWDVA 2, FWDVB 5, LFBDV 1 */
    case CPR0_PRIMAD0:
        return 0x01000000;
    case CPR0_PRIMBD0:
        return 0x02000000;              /* PLB = VCO / 5 / 2 */
    case CPR0_OPBD0:
        return 0x02000000;              /* OPB = PLB / 2 */
    case CPR0_PERD:
        return 0x02000000;              /* EBC = PLB / 2 */
    case CPR0_SPCID:
        return 0x02000000;              /* PCI = PLB / 2 */
    default:
        return sam_ind_get(d, d->addr);
    }
}

static void sam_cpr_write(void *opaque, int dcrn, uint32_t val)
{
    Sam440epIndirectDcr *d = opaque;

    if (dcrn == CPR0_DCR_BASE) {
        d->addr = val;
    } else {
        sam_ind_set(d, d->addr, val);
    }
}

static uint32_t sam_sdr_read(void *opaque, int dcrn)
{
    Sam440epIndirectDcr *d = opaque;

    if (dcrn == SDR0_DCR_BASE) {
        return d->addr;
    }
    switch (d->addr) {
    case SDR0_SDSTP0:
        return 0;                       /* CPU clock from the PLL */
    default:
        return sam_ind_get(d, d->addr);
    }
}

static void sam_sdr_write(void *opaque, int dcrn, uint32_t val)
{
    Sam440epIndirectDcr *d = opaque;

    if (dcrn == SDR0_DCR_BASE) {
        d->addr = val;
    } else {
        sam_ind_set(d, d->addr, val);
    }
}

static void sam_indirect_dcr_init(CPUPPCState *env, int base,
                                  dcr_read_cb rd, dcr_write_cb wr)
{
    Sam440epIndirectDcr *d = g_new0(Sam440epIndirectDcr, 1);

    d->regs = g_hash_table_new(NULL, NULL);
    ppc_dcr_register(env, base, d, rd, wr);
    ppc_dcr_register(env, base + 1, d, rd, wr);
}

/*
 * DCRs that U-Boot sets up and that do nothing here: the PLB arbiters
 * and the like. They keep what is written.
 */
static uint32_t sam_dcr_store_read(void *opaque, int dcrn)
{
    uint32_t *regs = opaque;

    return regs[dcrn & 0x3ff];
}

static void sam_dcr_store_write(void *opaque, int dcrn, uint32_t val)
{
    uint32_t *regs = opaque;

    regs[dcrn & 0x3ff] = val;
}

static void sam_dcr_store_init(CPUPPCState *env, const int *dcrns, int n)
{
    uint32_t *regs = g_new0(uint32_t, 0x400);

    for (int i = 0; i < n; i++) {
        ppc_dcr_register(env, dcrns[i], regs, sam_dcr_store_read,
                         sam_dcr_store_write);
    }
}

/* PCI interrupts */

/* port n of the SiI3114: ports 2 and 3 are not in PCIIDEState */
static IDEBus *sam_sata_port(DeviceState *dev, int port)
{
    BusState *bus;

    QLIST_FOREACH(bus, &dev->child_bus, sibling) {
        if (IDE_BUS(bus)->bus_id == port) {
            return IDE_BUS(bus);
        }
    }
    g_assert_not_reached();
}

/* Behind the bridge: the pin on the bridge that a device's interrupt uses */
static int sam_bridge_map_irq(PCIDevice *pci_dev, int pin)
{
    /* bridge pins 0..3 are UIC 26, 29, 27 and 50 */
    static const int slots[3][4] = {
        { 3, 2, 1, 3 },     /* 06: 50, 27, 29, 50 */
        { 1, 3, 1, 3 },     /* 07: 29, 50, 29, 50 */
        { 1, 3, 2, 3 },     /* 08: 29, 50, 27, 50 */
    };
    int slot = PCI_SLOT(pci_dev->devfn);

    switch (slot) {
    case SAM_DEV_SATA:
        return 0;
    case SAM_DEV_USB:
        return 1;
    case SAM_DEV_SLOT1 ... SAM_DEV_SLOT1 + 2:
        return slots[slot - SAM_DEV_SLOT1][pin & 3];
    default:
        return 1;
    }
}

/* On the root bus: the UIC input */
static int sam_root_map_irq(PCIDevice *pci_dev, int pin)
{
    static const int bridge_pins[4] = {
        SAM_IRQ26, SAM_IRQ29, SAM_IRQ27, SAM_IRQ50
    };

    switch (PCI_SLOT(pci_dev->devfn)) {
    case SAM_DEV_BRIDGE:
        return bridge_pins[pin & 3];
    case SAM_DEV_AUDIO:
        return SAM_IRQ27;
    default:
        return SAM_IRQ25;
    }
}

static void sam440ep_cpu_reset(void *opaque)
{
    PowerPCCPU *cpu = opaque;
    CPUPPCState *env = &cpu->env;

    cpu_reset(CPU(cpu));
    /*
     * At reset a shadow TLB maps the boot flash. QEMU has no shadow TLBs,
     * so map the same 256 MB that U-Boot maps first.
     */
    env->nip = UBOOT_ENTRY;
    booke_set_tlb(&env->tlb.tlbe[0], 0xf0000000, 0xf0000000, 0x10000000);
}

static void sam440ep_load_firmware(MachineState *machine)
{
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 0);
    const char *name = machine->firmware ?: SAM440EP_FIRMWARE;
    char *filename;

    if (!pflash_cfi01_register(FLASH_BASE, "sam440ep.flash", FLASH_SIZE,
                               dinfo ? blk_by_legacy_dinfo(dinfo) : NULL,
                               64 * KiB, 1, 0x89, 0x18, 0x0000, 0x0, 1)) {
        error_report("could not register the boot flash");
        exit(1);
    }
    if (dinfo) {
        return;
    }
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, name);
    if (!filename || load_image_targphys(filename, UBOOT_LOAD_BASE,
                                         FLASH_BASE + FLASH_SIZE -
                                         UBOOT_LOAD_BASE, NULL) < 0) {
        error_report("could not load firmware '%s'", name);
        exit(1);
    }
    g_free(filename);
}

static void sam440ep_init(MachineState *machine)
{
    static const int plain_dcrs[] = {
        0x77,               /* PLB3 arbiter control */
        0x81, 0x82, 0x83,   /* PLB4 arbiter */
        0x84, 0x85, 0x86, 0x87,
    };
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *isa = g_new(MemoryRegion, 1);
    MemoryRegion *init_ram = g_new(MemoryRegion, 1);
    DeviceState *uic[2], *dev, *pcihost;
    SysBusDevice *sbdev;
    PowerPCCPU *cpu;
    CPUPPCState *env;
    PCIBus *pci_bus, *sec_bus;
    PCIDevice *pdev;
    NICInfo *nd;
    Ppc4xxMalState *mal;
    I2CBus *i2c;
    USBBus *usb_bus;
    uint8_t *eeprom, *boot_eeprom;
    int i;

    cpu = POWERPC_CPU(cpu_create(machine->cpu_type));
    env = &cpu->env;
    if (env->mmu_model != POWERPC_MMU_BOOKE) {
        error_report("only the BookE MMU is supported by this machine");
        exit(1);
    }
    qemu_register_reset(sam440ep_cpu_reset, cpu);
    ppc_booke_timers_init(cpu, CPU_FREQ, 0);
    ppc_dcr_init(env, NULL, NULL);

    /* clocks and system DCRs */
    sam_indirect_dcr_init(env, CPR0_DCR_BASE, sam_cpr_read, sam_cpr_write);
    sam_indirect_dcr_init(env, SDR0_DCR_BASE, sam_sdr_read, sam_sdr_write);
    sam_dcr_store_init(env, plain_dcrs, ARRAY_SIZE(plain_dcrs));

    /*
     * U-Boot locks 4 KB of the data cache at 0x70000000 and keeps its
     * first stack there. QEMU has no cache to lock: give it memory.
     */
    memory_region_init_ram(init_ram, NULL, "sam440ep.init-ram", INIT_RAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, INIT_RAM_BASE, init_ram);

    /* PLB arbiter (PLB0) */
    dev = qdev_new(TYPE_PPC4xx_PLB);
    ppc4xx_dcr_realize(PPC4xx_DCR_DEVICE(dev), cpu, &error_fatal);
    object_unref(OBJECT(dev));

    /* two interrupt controllers, UIC1 through UIC0 inputs 30 and 31 */
    for (i = 0; i < ARRAY_SIZE(uic); i++) {
        uic[i] = qdev_new(TYPE_PPC_UIC);
        qdev_prop_set_uint32(uic[i], "dcr-base", 0xc0 + i * 0x10);
        ppc4xx_dcr_realize(PPC4xx_DCR_DEVICE(uic[i]), cpu, &error_fatal);
        object_unref(OBJECT(uic[i]));
        sbdev = SYS_BUS_DEVICE(uic[i]);
        if (i == 0) {
            sysbus_connect_irq(sbdev, PPCUIC_OUTPUT_INT,
                               qdev_get_gpio_in(DEVICE(cpu),
                                                PPC40x_INPUT_INT));
            sysbus_connect_irq(sbdev, PPCUIC_OUTPUT_CINT,
                               qdev_get_gpio_in(DEVICE(cpu),
                                                PPC40x_INPUT_CINT));
        } else {
            sysbus_connect_irq(sbdev, PPCUIC_OUTPUT_INT,
                               qdev_get_gpio_in(uic[0], 30));
            sysbus_connect_irq(sbdev, PPCUIC_OUTPUT_CINT,
                               qdev_get_gpio_in(uic[0], 31));
        }
    }

    /* DDR SDRAM controller */
    if (machine->ram_size > 1 * GiB) {
        error_report("the Sam440ep takes at most 1 GB of memory");
        exit(1);
    }
    dev = qdev_new(TYPE_PPC4xx_SDRAM_DDR);
    object_property_set_link(OBJECT(dev), "dram", OBJECT(machine->ram),
                             &error_abort);
    ppc4xx_dcr_realize(PPC4xx_DCR_DEVICE(dev), cpu, &error_fatal);
    object_unref(OBJECT(dev));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(uic[1], 13));
    ppc4xx_sdram_ddr_enable(PPC4xx_SDRAM_DDR(dev));

    /* external bus controller */
    dev = qdev_new(TYPE_PPC4xx_EBC);
    ppc4xx_dcr_realize(PPC4xx_DCR_DEVICE(dev), cpu, &error_fatal);
    object_unref(OBJECT(dev));

    /* memory access layer, for the EMACs */
    dev = qdev_new(TYPE_PPC4xx_MAL);
    mal = PPC4xx_MAL(dev);
    qdev_prop_set_uint8(dev, "txc-num", 4);
    qdev_prop_set_uint8(dev, "rxc-num", 2);
    ppc4xx_dcr_realize(PPC4xx_DCR_DEVICE(dev), cpu, &error_fatal);
    object_unref(OBJECT(dev));
    sbdev = SYS_BUS_DEVICE(dev);
    /* TXEOB, RXEOB, TXDE, RXDE */
    sysbus_connect_irq(sbdev, 0, qdev_get_gpio_in(uic[0], 10));
    sysbus_connect_irq(sbdev, 1, qdev_get_gpio_in(uic[0], 11));
    sysbus_connect_irq(sbdev, 2, qdev_get_gpio_in(uic[1], 1));
    sysbus_connect_irq(sbdev, 3, qdev_get_gpio_in(uic[1], 2));

    /* UARTs */
    for (i = 0; i < 2; i++) {
        if (serial_hd(i)) {
            serial_mm_init(sysmem, 0xef600300 + i * 0x100, 0,
                           qdev_get_gpio_in(uic[0], i),
                           PLB_FREQ, serial_hd(i), DEVICE_BIG_ENDIAN);
        }
    }

    /* I2C: the environment EEPROM and the RTC */
    dev = sysbus_create_simple(TYPE_PPC4xx_I2C, 0xef600700,
                               qdev_get_gpio_in(uic[0], 2));
    i2c = PPC4xx_I2C(dev)->bus;
    /* 1 KB of EEPROM at 0x54..0x57, 256 bytes for each address */
    eeprom = g_malloc0(1 * KiB);
    for (i = 0; i < 4; i++) {
        smbus_eeprom_init_one(i2c, 0x54 + i, eeprom + i * 256);
    }
    /*
     * The boot EEPROM at 0x52 and 0x53: bytes 0xfe and 0xff of 0x53 give
     * the I2C addresses of the SPDs of the first two DIMM slots. The first
     * is a DIMM at 0x51 that matches the RAM size; there is no second.
     * U-Boot also looks for two more slots at 0x50, so nothing may answer
     * there.
     */
    boot_eeprom = g_malloc0(512);
    boot_eeprom[256 + 0xfe] = 0x51;
    boot_eeprom[256 + 0xff] = 0x58;
    smbus_eeprom_init_one(i2c, 0x52, boot_eeprom);
    smbus_eeprom_init_one(i2c, 0x53, boot_eeprom + 256);
    smbus_eeprom_init_one(i2c, 0x51, spd_data_generate(DDR, machine->ram_size));
    i2c_slave_create_simple(i2c, "m41t80", 0x68);
    sysbus_create_simple(TYPE_PPC4xx_I2C, 0xef600800,
                         qdev_get_gpio_in(uic[0], 7));

    /* GPIO: the firmware only sets pin functions */
    create_unimplemented_device("ppc440ep.gpio0", 0xef600b00, 0x100);
    create_unimplemented_device("ppc440ep.gpio1", 0xef600c00, 0x100);

    /* PCI */
    /*
     * PCI memory is a space of its own, of which U-Boot maps 1 GB at
     * 0xa0000000. On the PLB it would put the legacy VGA memory of the
     * Radeon over RAM at 0xa0000.
     */
    pcihost = qdev_new(TYPE_PPC4xx_PCI_HOST);
    qdev_prop_set_uint32(pcihost, "pci-mem-base", SAM_PCI_MEM_BASE);
    qdev_prop_set_uint32(pcihost, "pci-mem-size", SAM_PCI_MEM_SIZE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pcihost), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(pcihost), 0, PCI_CONFIG_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(pcihost), SAM_IRQ25,
                       qdev_get_gpio_in(uic[0], 25));
    sysbus_connect_irq(SYS_BUS_DEVICE(pcihost), SAM_IRQ26,
                       qdev_get_gpio_in(uic[0], 26));
    sysbus_connect_irq(SYS_BUS_DEVICE(pcihost), SAM_IRQ27,
                       qdev_get_gpio_in(uic[0], 27));
    sysbus_connect_irq(SYS_BUS_DEVICE(pcihost), SAM_IRQ29,
                       qdev_get_gpio_in(uic[0], 29));
    sysbus_connect_irq(SYS_BUS_DEVICE(pcihost), SAM_IRQ50,
                       qdev_get_gpio_in(uic[1], 50 - 32));
    pci_bus = PCI_BUS(qdev_get_child_bus(pcihost, "pci.0"));
    pci_bus_map_irqs(pci_bus, sam_root_map_irq);

    memory_region_init_alias(isa, NULL, "isa_mmio", get_system_io(), 0,
                             SAM_PCI_IO_SIZE);
    memory_region_add_subregion(sysmem, SAM_PCI_IO_BASE, isa);

    pdev = pci_new(PCI_DEVFN(SAM_DEV_BRIDGE, 0), "pci-bridge");
    qdev_prop_set_uint8(DEVICE(pdev), "chassis_nr", 1);
    qdev_prop_set_bit(DEVICE(pdev), "shpc", false);
    object_property_parse(OBJECT(pdev), "msi", "off", &error_abort);
    pci_bridge_map_irq(PCI_BRIDGE(pdev), "pci.1", sam_bridge_map_irq);
    pci_realize_and_unref(pdev, pci_bus, &error_fatal);
    sec_bus = pci_bridge_get_sec_bus(PCI_BRIDGE(pdev));

    /* the Radeon Mobility M9, with 64 MB */
    pdev = pci_new(PCI_DEVFN(SAM_DEV_RADEON, 0), "ati-vga");
    qdev_prop_set_string(DEVICE(pdev), "model", "rv250");
    qdev_prop_set_uint32(DEVICE(pdev), "vgamem_mb", 64);
    pci_realize_and_unref(pdev, pci_bus, &error_fatal);

    /* the CS4281 sound */
    pdev = pci_new(PCI_DEVFN(SAM_DEV_AUDIO, 0), "cs4281");
    if (machine->audiodev) {
        qdev_prop_set_string(DEVICE(pdev), "audiodev", machine->audiodev);
    }
    pci_realize_and_unref(pdev, pci_bus, &error_fatal);

    /* SATA, behind the bridge */
    if (defaults_enabled()) {
        PCIIDEState *s = PCI_IDE(pci_create_simple(sec_bus,
                                                   PCI_DEVFN(SAM_DEV_SATA, 0),
                                                   "sii3114"));
        DriveInfo *di[4];

        /*
         * Drive index n is port n. Without an index 1, the next drive moves
         * up to port 1, so that -cdrom (index 2) works on its own.
         */
        for (i = 0; i < 4; i++) {
            di[i] = drive_get_by_index(IF_IDE, i);
        }
        if (!di[1]) {
            di[1] = di[2];
            di[2] = di[3];
            di[3] = NULL;
        }
        for (i = 0; i < 4; i++) {
            if (di[i]) {
                ide_bus_create_drive(sam_sata_port(DEVICE(s), i), 0, di[i]);
            }
        }
    }

    /* USB, behind the bridge, with a keyboard and a mouse */
    if (!machine->usb_disabled) {
        pci_create_simple(sec_bus, PCI_DEVFN(SAM_DEV_USB, 0), "isp1561-ohci");
        usb_bus = USB_BUS(object_resolve_type_unambiguous(TYPE_USB_BUS,
                                                          &error_abort));
        if (defaults_enabled()) {
            usb_create_simple(usb_bus, "usb-kbd");
            usb_create_simple(usb_bus, "usb-mouse");
        }
    }

    /*
     * The two on-chip EMACs behind the ZMII bridge, with their PHYs at 24
     * and 25, take the first two NICs, those with model=ppc4xx-emac or no
     * model. Their MAC addresses go to bytes 0xf0-0xfb of the boot EEPROM
     * at 0x53, where our U-Boot reads them when its environment has no
     * ethaddr and eth1addr; AmigaOS takes them from the saved environment.
     */
    sysbus_create_simple(TYPE_PPC4XX_ZMII, 0xef600d00, NULL);
    for (i = 0; i < 2; i++) {
        MACAddr mac;

        dev = qdev_new(TYPE_PPC4XX_EMAC);
        object_property_set_link(OBJECT(dev), "mal", OBJECT(mal),
                                 &error_abort);
        qdev_prop_set_uint32(dev, "tx-channel", i * 2);
        qdev_prop_set_uint32(dev, "rx-channel", i);
        qdev_prop_set_uint32(dev, "phy-addr", 24 + i);
        nd = qemu_find_nic_info(TYPE_PPC4XX_EMAC, true, NULL);
        if (nd) {
            qdev_set_nic_properties(dev, nd);
            mac = nd->macaddr;
        } else {
            qemu_macaddr_default_if_unset(&mac);
            qdev_prop_set_macaddr(dev, "mac", mac.a);
        }
        memcpy(boot_eeprom + 256 + 0xf0 + i * 6, mac.a, 6);
        sbdev = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbdev, &error_fatal);
        sysbus_mmio_map(sbdev, 0, 0xef600e00 + i * 0x100);
        sysbus_connect_irq(sbdev, 0, qdev_get_gpio_in(uic[1], 28 + i * 2));
    }

    /*
     * A 3c905c (eth3com.device) goes in slot 07. Its INTA is IRQ 29, as
     * U-Boot sets it; slot 06's INTA is IRQ 50 on UIC1, which the AmigaOS
     * 4.1 kernel never enables. Any other NICs take the free slots.
     */
    nd = qemu_find_nic_info("3c905c", false, NULL);
    if (nd) {
        pdev = pci_new(PCI_DEVFN(SAM_DEV_SLOT1 + 1, 0), "3c905c");
        qdev_set_nic_properties(DEVICE(pdev), nd);
        pci_realize_and_unref(pdev, sec_bus, &error_fatal);
    }
    pci_init_nic_devices(sec_bus, "3c905c");

    sam440ep_load_firmware(machine);
}

static void sam440ep_machine_init(MachineClass *mc)
{
    mc->desc = "ACube Sam440ep";
    mc->init = sam440ep_init;
    mc->block_default_type = IF_IDE;
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("440epb");
    mc->default_ram_size = 512 * MiB;
    mc->default_ram_id = "ppc4xx.sdram";
    mc->default_nic = TYPE_PPC4XX_EMAC;
    machine_add_audiodev_property(mc);
}

DEFINE_MACHINE("sam440ep", sam440ep_machine_init)
