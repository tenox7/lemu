/*
 * Be Inc. BeBox (dual PowerPC 603/603e)
 *
 * Two 603 CPUs share a Motorola MPC105 host bridge (address map A). PCI
 * device 11 is an Intel 82378 SIO bridge to ISA, 12 an NCR 53C810 SCSI
 * controller and 13-15 the three slots. On ISA sit an Intel 82091AA
 * Super I/O (floppy, two UARTs, parallel, IDE), the 8042 keyboard
 * controller, a bq3285 RTC, a TL16C554 with two more UARTs and two MIDI
 * ports, a CS4231A codec and the GeekPort. Motherboard logic on the
 * local bus routes every interrupt source to either CPU and holds CPU 1
 * in reset until CPU 0 lets it go. The boot ROM is an AMD flash at
 * 0xfff00000, a 128 KiB Am29F010 or a 512 KiB Am29F040: Be's "nub" in
 * the first sector, the main boot ROM after it.
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "system/qtest.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/block/flash.h"
#include "hw/char/serial.h"
#include "hw/char/serial-mm.h"
#include "hw/isa/isa.h"
#include "hw/isa/superio.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/mpc105.h"
#include "hw/ppc/bebox.h"
#include "hw/ppc/ppc.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/scsi/scsi.h"
#include "hw/core/sysbus.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/kvm.h"
#include "system/reset.h"
#include "system/system.h"
#include "system/tcg.h"
#include "target/ppc/cpu-qom.h"
#include "cpu.h"
#include "trace.h"

#define TYPE_BEBOX_MACHINE MACHINE_TYPE_NAME("bebox")
OBJECT_DECLARE_SIMPLE_TYPE(BeBoxMachineState, BEBOX_MACHINE)

#define BEBOX_FLASH_BASE    0xfff00000
#define BEBOX_ROM_WINDOW    (1 * MiB)
#define BEBOX_RESET_VECTOR  0xfff00100

/* The 60x bus runs at 33.24 MHz and the time base at a quarter of it */
#define BEBOX_BUS_HZ        33238632

/*
 * The sound codec is not on an ISA interrupt line but on its own bit of
 * the motherboard logic. Give it ISA IRQ 13, which nothing else uses on
 * the BeBox, and route that line only to the motherboard logic.
 */
#define BEBOX_SOUND_ISA_IRQ 13

struct BeBoxMachineState {
    MachineState parent_obj;

    PowerPCCPU *cpu[2];
    int ncpus;
    DeviceState *glue;
    qemu_irq isa_pic[ISA_NUM_IRQS];
    qemu_irq *isa_irqs;
    bool cpu1_in_reset;
    QEMUTimer *slice_timer;
    uint32_t slice_us;
};

/* ISA IRQ line to motherboard interrupt bit; 0 means the 8259 only */
static const uint8_t bebox_isa_glue_bit[ISA_NUM_IRQS] = {
    [1] = BEBOX_IRQ_KEYBOARD,
    [3] = BEBOX_IRQ_SERIAL2,
    [4] = BEBOX_IRQ_SERIAL1,
    [5] = BEBOX_IRQ_ISA5,
    [6] = BEBOX_IRQ_FLOPPY,
    [7] = BEBOX_IRQ_PARALLEL,
    [8] = BEBOX_IRQ_RTC,
    [9] = BEBOX_IRQ_ISA9,
    [10] = BEBOX_IRQ_ISA10,
    [11] = BEBOX_IRQ_ISA11,
    [12] = BEBOX_IRQ_MOUSE,
    [BEBOX_SOUND_ISA_IRQ] = BEBOX_IRQ_SOUND,
    [14] = BEBOX_IRQ_IDE,
    [15] = BEBOX_IRQ_ISA15,
};

/* Each ISA line goes to both the 8259 pair and the motherboard logic */
static void bebox_isa_irq(void *opaque, int n, int level)
{
    BeBoxMachineState *s = opaque;

    if (n != BEBOX_SOUND_ISA_IRQ) {
        qemu_set_irq(s->isa_pic[n], level);
    }
    if (bebox_isa_glue_bit[n]) {
        qemu_set_irq(qdev_get_gpio_in(s->glue, bebox_isa_glue_bit[n]), level);
    }
}

/* CPUs */

static void bebox_cpu_reset(void *opaque)
{
    PowerPCCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
    cpu_ppc_tb_reset(&cpu->env);
}

static void bebox_cpu1_stop(CPUState *cs, run_on_cpu_data data)
{
    cpu_reset(cs);
    cs->halted = 1;
}

static void bebox_cpu1_start(CPUState *cs, run_on_cpu_data data)
{
    cpu_reset(cs);
    cs->halted = 0;
}

/*
 * HRESET holds CPU 1 at reset while it is asserted. SRESET is not
 * modelled: BeOS and NetBSD release HRESET with SRESET still asserted.
 */
static void bebox_cpu1_hreset(void *opaque, int n, int level)
{
    BeBoxMachineState *s = opaque;

    trace_bebox_cpu1_reset(level, -1);
    if (s->ncpus < 2 || s->cpu1_in_reset == !!level) {
        return;
    }
    s->cpu1_in_reset = level;
    async_run_on_cpu(CPU(s->cpu[1]), level ? bebox_cpu1_stop :
                     bebox_cpu1_start, RUN_ON_CPU_NULL);
}

static void bebox_cpu1_sreset(void *opaque, int n, int level)
{
    trace_bebox_cpu1_reset(-1, level);
}

/*
 * Round-robin TCG runs each CPU for up to the next timer deadline, often
 * milliseconds. Keep the slices short so the two CPUs see each other's
 * writes, interrupts and spin locks in good time.
 */
static bool bebox_round_robin(void)
{
    return !tcg_enabled() || !qemu_tcg_mttcg_enabled();
}

static void bebox_slice_timer(void *opaque)
{
    BeBoxMachineState *s = opaque;

    timer_mod(s->slice_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->slice_us * SCALE_US);
    qemu_cpu_kick(CPU(s->cpu[0]));
}

static void bebox_machine_reset(MachineState *machine, ResetType type)
{
    BeBoxMachineState *s = BEBOX_MACHINE(machine);

    qemu_devices_reset(type);
    s->cpu1_in_reset = true;
    if (s->ncpus > 1 && s->slice_us && bebox_round_robin()) {
        timer_mod(s->slice_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  s->slice_us * SCALE_US);
    }
}

/* Boot ROM */

/*
 * The BeBox shipped with a 128 KiB Am29F010. Be's flash driver and the
 * nub also take a 512 KiB Am29F040, which the boot ROM of BeOS R4 needs:
 * the image size picks the chip.
 */
typedef struct BeBoxFlashChip {
    uint32_t size;
    uint32_t sector;
    uint16_t device_id;
} BeBoxFlashChip;

static const BeBoxFlashChip bebox_flash_chips[] = {
    { 128 * KiB, 16 * KiB, 0x20 },     /* Am29F010 */
    { 512 * KiB, 64 * KiB, 0xa4 },     /* Am29F040 */
};

static const BeBoxFlashChip *bebox_flash_chip(int64_t size, bool exact)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(bebox_flash_chips); i++) {
        if (exact ? size == bebox_flash_chips[i].size :
            size <= bebox_flash_chips[i].size) {
            return &bebox_flash_chips[i];
        }
    }
    error_report("bebox: the boot ROM image must be 128 KiB (Am29F010) or "
                 "512 KiB (Am29F040)");
    exit(1);
}

static void bebox_init_flash(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 0);
    const BeBoxFlashChip *chip;

    if (dinfo) {
        BlockBackend *blk = blk_by_legacy_dinfo(dinfo);

        chip = bebox_flash_chip(blk_getlength(blk), true);
        pflash_cfi02_register(BEBOX_FLASH_BASE, "bebox.flash", chip->size,
                              blk, chip->sector,
                              BEBOX_ROM_WINDOW / chip->size, 1,
                              0x01, chip->device_id, 0x00, 0x00,
                              0x5555, 0x2aaa, 1);
        if (machine->firmware) {
            warn_report("bebox: -bios is ignored, the flash drive is used");
        }
    } else {
        const char *name = machine->firmware ?: "bebox-flash.bin";
        g_autofree char *filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, name);
        MemoryRegion *rom = g_new(MemoryRegion, 1);
        uint32_t at;

        if (!filename && !qtest_enabled()) {
            error_report("bebox: no boot ROM, give the flash with -drive "
                         "if=pflash or -bios (see machines/bebox/README.md)");
            exit(1);
        }
        /* qtest starts every machine without firmware: map an empty ROM */
        chip = filename ? bebox_flash_chip(get_image_size(filename, NULL),
                                           false)
                        : &bebox_flash_chips[0];
        memory_region_init_rom(rom, NULL, "bebox.rom", chip->size,
                               &error_fatal);
        if (filename && load_image_mr(filename, rom) < 0) {
            error_report("bebox: could not load boot ROM '%s'", filename);
            exit(1);
        }
        memory_region_add_subregion(sysmem, BEBOX_FLASH_BASE, rom);
        for (at = chip->size; at < BEBOX_ROM_WINDOW; at += chip->size) {
            MemoryRegion *alias = g_new(MemoryRegion, 1);

            memory_region_init_alias(alias, NULL, "bebox.rom-mirror", rom, 0,
                                     chip->size);
            memory_region_add_subregion(sysmem, BEBOX_FLASH_BASE + at, alias);
        }
    }
}

/* ISA */

static void bebox_create_uart(BeBoxMachineState *s, ISABus *isa_bus,
                              hwaddr port, int serial_index, int glue_bit,
                              uint8_t modem_inputs)
{
    DeviceState *dev = qdev_new(TYPE_SERIAL_MM);
    Chardev *chr = serial_hd(serial_index);

    if (!chr) {
        g_autofree char *label = g_strdup_printf("discarding-serial%d",
                                                 serial_index);

        chr = qemu_chr_new(label, "null", NULL);
    }
    qdev_prop_set_uint8(dev, "regshift", 0);
    qdev_prop_set_uint8(dev, "endianness", DEVICE_LITTLE_ENDIAN);
    qdev_prop_set_uint32(dev, "baudbase", 115200);
    qdev_prop_set_uint8(dev, "modem-inputs", modem_inputs);
    qdev_prop_set_chr(dev, "chardev", chr);
    object_property_add_child(OBJECT(s), "tl16c554[*]", OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(isa_address_space_io(NULL), port,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev),
                                                       0));
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(s->glue, glue_bit));
}

static void bebox_init(MachineState *machine)
{
    BeBoxMachineState *s = BEBOX_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *dev, *pcihost, *i82378;
    PCIBus *pci_bus;
    ISABus *isa_bus;
    ISADevice *isa_dev;
    int i;

    if (kvm_enabled()) {
        error_report("machine %s does not support the KVM accelerator",
                     MACHINE_GET_CLASS(machine)->name);
        exit(EXIT_FAILURE);
    }
    if (machine->ram_size > 256 * MiB) {
        error_report("bebox: at most 256 MiB of RAM");
        exit(EXIT_FAILURE);
    }

    /* CPUs: CPU 1 waits in reset for CPU 0 */
    s->ncpus = machine->smp.cpus;
    for (i = 0; i < s->ncpus; i++) {
        PowerPCCPU *cpu = POWERPC_CPU(object_new(machine->cpu_type));
        CPUPPCState *env = &cpu->env;

        object_property_set_bool(OBJECT(cpu), "start-powered-off", i != 0,
                                 &error_fatal);
        qdev_realize_and_unref(DEVICE(cpu), NULL, &error_fatal);
        if (PPC_INPUT(env) != PPC_FLAGS_INPUT_6xx) {
            error_report("only 6xx bus is supported on this machine");
            exit(1);
        }
        /*
         * BeOS's idle threads spin on "b .": let them give the host
         * thread to the other CPU, which does the work.
         */
        env->idle_yield = true;
        cpu_ppc_tb_init(env, BEBOX_BUS_HZ / 4);
        qemu_register_reset(bebox_cpu_reset, cpu);
        s->cpu[i] = cpu;
    }

    /* Motherboard logic */
    s->glue = qdev_new(TYPE_BEBOX_GLUE);
    object_property_add_child(OBJECT(machine), "glue", OBJECT(s->glue));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->glue), &error_fatal);
    memory_region_add_subregion(sysmem, BEBOX_GLUE_BASE,
                                sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(s->glue), 0));
    for (i = 0; i < s->ncpus; i++) {
        DeviceState *cpudev = DEVICE(s->cpu[i]);

        qdev_connect_gpio_out_named(s->glue, "cpu-int", i,
                                    qdev_get_gpio_in(cpudev,
                                                     PPC6xx_INPUT_INT));
        qdev_connect_gpio_out_named(s->glue, "cpu-smi", i,
                                    qdev_get_gpio_in(cpudev,
                                                     PPC6xx_INPUT_SMI));
    }
    qdev_connect_gpio_out_named(s->glue, "cpu1-hreset", 0,
                                qemu_allocate_irq(bebox_cpu1_hreset, s, 0));
    qdev_connect_gpio_out_named(s->glue, "cpu1-sreset", 0,
                                qemu_allocate_irq(bebox_cpu1_sreset, s, 0));

    bebox_init_flash(machine);

    /* MPC105 */
    pcihost = qdev_new(TYPE_MPC105_PCI_HOST_BRIDGE);
    object_property_set_link(OBJECT(pcihost), "ram", OBJECT(machine->ram),
                             &error_fatal);
    object_property_add_child(OBJECT(machine), "mpc105", OBJECT(pcihost));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pcihost), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(pcihost), 0,
                       qdev_get_gpio_in(s->glue, BEBOX_IRQ_SCSI));
    sysbus_connect_irq(SYS_BUS_DEVICE(pcihost), 1,
                       qdev_get_gpio_in(s->glue, BEBOX_IRQ_PCI1));
    sysbus_connect_irq(SYS_BUS_DEVICE(pcihost), 2,
                       qdev_get_gpio_in(s->glue, BEBOX_IRQ_PCI2));
    sysbus_connect_irq(SYS_BUS_DEVICE(pcihost), 3,
                       qdev_get_gpio_in(s->glue, BEBOX_IRQ_PCI3));
    pci_bus = PCI_BUS(qdev_get_child_bus(pcihost, "pci.0"));

    /* 82378 SIO: 8259s, timer, DMA */
    i82378 = DEVICE(pci_new(PCI_DEVFN(11, 0), "i82378"));
    qdev_realize_and_unref(i82378, BUS(pci_bus), &error_fatal);
    qdev_connect_gpio_out(i82378, 0,
                          qdev_get_gpio_in(s->glue, BEBOX_IRQ_8259));
    isa_bus = ISA_BUS(qdev_get_child_bus(i82378, "isa.0"));
    for (i = 0; i < ISA_NUM_IRQS; i++) {
        s->isa_pic[i] = isa_bus_get_irq(isa_bus, i);
    }
    s->isa_irqs = qemu_allocate_irqs(bebox_isa_irq, s, ISA_NUM_IRQS);
    isa_bus_register_input_irqs(isa_bus, s->isa_irqs);

    /*
     * bq3285 RTC. The year register holds the last two digits of the
     * year, which BeOS reads as 1970 to 2069. There is no century
     * byte: 0x32 is CMOS RAM under the boot ROM's checksum, and a
     * bad checksum sets the clock back to December 1994.
     */
    isa_dev = isa_new(TYPE_MC146818_RTC);
    qdev_prop_set_int32(DEVICE(isa_dev), "base_year", 2000);
    qdev_prop_set_bit(DEVICE(isa_dev), "century-byte", false);
    isa_realize_and_unref(isa_dev, isa_bus, &error_fatal);

    /* 82091AA: floppy, serial 1 and 2, parallel, IDE, and the 8042 */
    isa_dev = isa_new(TYPE_I82091AA_SUPERIO);
    isa_realize_and_unref(isa_dev, isa_bus, &error_fatal);

    /*
     * TL16C554: serial 3 and 4, MIDI 1 and 2. The modem status inputs of
     * MIDI 1 tell the motherboard revision: RI, DSR and CTS (0x70) for
     * rev 5. The others get DCD, DSR and CTS (0xb0), as with no cable.
     */
    bebox_create_uart(s, isa_bus, 0x380, 2, BEBOX_IRQ_SERIAL3, 0xb0);
    bebox_create_uart(s, isa_bus, 0x388, 3, BEBOX_IRQ_SERIAL4, 0xb0);
    bebox_create_uart(s, isa_bus, 0x3a0, 4, BEBOX_IRQ_MIDI1, 0x70);
    bebox_create_uart(s, isa_bus, 0x3a8, 5, BEBOX_IRQ_MIDI2, 0x00);

    /* GeekPort, A/D and D/A, front-panel LEDs */
    isa_create_simple(isa_bus, TYPE_BEBOX_IO);

    /*
     * Sound: Be's driver plays on DMA 6 and records on DMA 7, both set to
     * 8-bit transfers in the 82378's extended mode register. QEMU's codec
     * only plays.
     */
    isa_dev = isa_new("cs4231a");
    qdev_prop_set_uint32(DEVICE(isa_dev), "iobase", 0x830);
    qdev_prop_set_uint32(DEVICE(isa_dev), "irq", BEBOX_SOUND_ISA_IRQ);
    qdev_prop_set_uint32(DEVICE(isa_dev), "dma", 6);
    if (machine->audiodev) {
        qdev_prop_set_string(DEVICE(isa_dev), "audiodev", machine->audiodev);
    }
    isa_realize_and_unref(isa_dev, isa_bus, &error_fatal);

    /* PCI devices */
    if (defaults_enabled()) {
        dev = DEVICE(pci_create_simple(pci_bus, PCI_DEVFN(12, 0),
                                       "lsi53c810"));
        lsi53c8xx_handle_legacy_cmdline(dev);
    }
    if (vga_interface_type != VGA_NONE) {
        PCIDevice *vga = pci_new(PCI_DEVFN(13, 0), "cirrus-vga-gd5434");

        /*
         * The boot ROM places BARs one after the other without aligning
         * them, starting at 16 MiB; the card's 32 MiB BAR would not decode.
         */
        qdev_prop_set_uint32(DEVICE(vga), "bar0-mb", 16);
        /*
         * BeOS fills with color-expanding BLTs whose source pitch is 0,
         * and programs the GD5434's 32 bpp pitch in its double units.
         */
        qdev_prop_set_bit(DEVICE(vga), "colorexpand-packed", true);
        qdev_prop_set_bit(DEVICE(vga), "gd5434-pitch32", true);
        pci_realize_and_unref(vga, pci_bus, &error_fatal);
    }

    if (s->ncpus > 1) {
        s->slice_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bebox_slice_timer,
                                      s);
    }
    s->cpu1_in_reset = true;
}

static void bebox_machine_instance_init(Object *obj)
{
    BeBoxMachineState *s = BEBOX_MACHINE(obj);

    s->slice_us = 100;
    object_property_add_uint32_ptr(obj, "slice", &s->slice_us,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "slice",
        "Round-robin TCG time slice of each CPU in microseconds, 0 for "
        "QEMU's own (default: 100)");
}

static void bebox_machine_class_init(ObjectClass *oc, const void *data)
{
    /*
     * The boot ROM waits to see the 8042 take each byte it writes (IBF
     * set, then clear), and reads the command byte with its keyboard
     * interrupt handler installed: the controller must come out of reset
     * with its interrupts off, or the handler takes the reply.
     */
    static GlobalProperty compat[] = {
        { "i8042", "ibf-read", "on" },
        { "i8042", "reset-mode", "0" },
        /*
         * BeOS's Restart only jumps back to the boot ROM, which self-tests
         * the 8042 and counts on that to turn translation off.
         */
        { "i8042", "selftest-reset", "on" },
        /* the IDE drive's DMA request and acknowledge go to channel 5 */
        { "isa-ide", "dma", "5" },
        /* drives of the time had no Ultra DMA; BeOS R4.5 wants MDMA set */
        { "ide-hd", "udma", "off" },
        { "ide-cd", "udma", "off" },
        /*
         * The 82378 has the EISA extended mode registers: BeOS sets the
         * sound codec's channel 6 to 8-bit transfers there.
         */
        { "i8257", "ext-mode", "on" },
        /* the slave 8259's INT drops during the acknowledge */
        { "isa-i8259", "slave-int-drop", "on" },
        /* BeOS's sound driver counts on the codec's exact DMA counts */
        { "cs4231a", "exact-dma", "on" },
        /* BeOS 1.1 and R4 overwrite the PROM and match their PAR */
        { "ne2k_isa", "dp8390-rx", "on" },
        { "ne2k_pci", "dp8390-rx", "on" },
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    compat_props_add(mc->compat_props, compat, G_N_ELEMENTS(compat));
    mc->desc = "Be Inc. BeBox (dual PowerPC 603)";
    mc->init = bebox_init;
    mc->reset = bebox_machine_reset;
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    mc->default_ram_size = 32 * MiB;
    mc->default_ram_id = "bebox.ram";
    mc->block_default_type = IF_SCSI;
    mc->default_boot_order = "c";
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("603e_v4.1");
    mc->default_display = "cirrus";
    machine_add_audiodev_property(mc);
}

static const TypeInfo bebox_machine_info = {
    .name = TYPE_BEBOX_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(BeBoxMachineState),
    .instance_init = bebox_machine_instance_init,
    .class_init = bebox_machine_class_init,
};

static void bebox_machine_register_types(void)
{
    type_register_static(&bebox_machine_info);
}

type_init(bebox_machine_register_types)
