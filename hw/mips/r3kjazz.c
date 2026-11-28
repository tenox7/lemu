/*
 * QEMU R3000 Jazz support
 *
 * This is the 32-bit R3000 variant of the Microsoft Jazz ARC system
 * described by the mame-r3kjazz fork.  It shares most of its peripherals
 * with the R4000 Magnum but uses the MCT-ADR bus controller, a G300
 * framebuffer and NCR53C94 SCSI.
 *
 * The RTC read helper, the dummy DMA handlers and the MAC checksum are
 * copied from hw/mips/jazz.c, which carries this notice:
 *
 * Copyright (c) 2007-2008 Hervé Poussineau
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
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "hw/core/clock.h"
#include "hw/mips/mips.h"
#include "hw/intc/i8259.h"
#include "hw/dma/i8257.h"
#include "hw/char/serial-mm.h"
#include "hw/char/parallel.h"
#include "hw/isa/isa.h"
#include "hw/block/fdc.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "net/net.h"
#include "hw/scsi/esp.h"
#include "hw/core/loader.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/rtc/mc146818rtc_regs.h"
#include "hw/timer/i8254.h"
#include "hw/display/g300fb.h"
#include "hw/audio/pcspk.h"
#include "hw/input/i8042.h"
#include "hw/input/vmmouse-mmio.h"
#include "hw/core/sysbus.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "accel/tcg/cpu-ops.h"
#include "target/mips/cpu.h"

#define TYPE_R3KJAZZ_MACHINE MACHINE_TYPE_NAME("r3kjazz")
OBJECT_DECLARE_SIMPLE_TYPE(R3kJazzMachineState, R3KJAZZ_MACHINE)

struct R3kJazzMachineState {
    MachineState parent_obj;
    bool nt_idle;
};

#define R3K_BIOS_SIZE (256 * KiB)
#define SONIC_PROM_SIZE 0x1000

static void main_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
}

/* The RTC data port is reached through the i82357 ISP at 0x90000071. */
static uint64_t rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    uint8_t val;

    address_space_read(&address_space_memory, 0x90000071,
                       MEMTXATTRS_UNSPECIFIED, &val, 1);
    return val;
}

static void rtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    uint8_t buf = val & 0xff;

    address_space_write(&address_space_memory, 0x90000071,
                        MEMTXATTRS_UNSPECIFIED, &buf, 1);
}

static const MemoryRegionOps rtc_ops = {
    .read = rtc_read,
    .write = rtc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint64_t dma_dummy_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0xff;
}

static void dma_dummy_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
}

static const MemoryRegionOps dma_dummy_ops = {
    .read = dma_dummy_read,
    .write = dma_dummy_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void r3kjazz_init_net(IOMMUMemoryRegion *dma_mr, DeviceState *adr,
                             MemoryRegion *prom)
{
    DeviceState *dev;
    SysBusDevice *sysbus;
    NICInfo *nd;
    uint8_t *mac;
    int checksum, i;

    nd = qemu_find_nic_info("dp8393x", true, "dp83932");
    if (!nd) {
        return;
    }

    dev = qdev_new("dp8393x");
    qdev_set_nic_properties(dev, nd);
    qdev_prop_set_uint8(dev, "it_shift", 2);
    qdev_prop_set_bit(dev, "big_endian", false);
    object_property_set_link(OBJECT(dev), "dma_mr", OBJECT(dma_mr),
                             &error_abort);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, 0x80001000);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(adr, 4));

    mac = memory_region_get_ram_ptr(prom);
    checksum = 0;
    for (i = 0; i < 6; i++) {
        mac[i] = nd->macaddr.a[i];
        checksum += mac[i];
        if (checksum > 0xff) {
            checksum = (checksum + 1) & 0xff;
        }
    }
    mac[7] = 0xff - checksum;
}

static void r3kjazz_init(MachineState *machine)
{
    const char *bios_name = "j3prom.bin";
    MemoryRegion *address_space = get_system_memory();
    char *filename;
    int bios_size, n;
    Clock *cpuclk;
    MIPSCPU *cpu;
    MIPSCPUClass *mcc;
    CPUMIPSState *env;
    qemu_irq *i8259;
    rc4030_dma *dmas;
    IOMMUMemoryRegion *dma_mr;
    MemoryRegion *isa_mem = g_new(MemoryRegion, 1);
    MemoryRegion *isa_io = g_new(MemoryRegion, 1);
    MemoryRegion *rtc = g_new(MemoryRegion, 1);
    MemoryRegion *dma_dummy = g_new(MemoryRegion, 1);
    MemoryRegion *dp8393x_prom = g_new(MemoryRegion, 1);
    MemoryRegion *cache_shadow = g_new(MemoryRegion, 1);
    MemoryRegion *bios = g_new(MemoryRegion, 1);
    MemoryRegion *bios2 = g_new(MemoryRegion, 1);
    DeviceState *dev, *adr;
    MMIOKBDState *i8042;
    SysBusDevice *sysbus;
    ISABus *isa_bus;
    ISADevice *pit;
    ISADevice *pcspk;
    DriveInfo *fds[MAX_FD];
    SysBusESPState *sysbus_esp;
    ESPState *esp;

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, 25 * 1000 * 1000);

    cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk, false);
    env = &cpu->env;
    cpu->nt_idle = R3KJAZZ_MACHINE(machine)->nt_idle;
    qemu_register_reset(main_cpu_reset, cpu);

    /*
     * Like the Magnum, the Jazz chipset returns zero for invalid data
     * accesses without raising an exception.
     */
    mcc = MIPS_CPU_GET_CLASS(cpu);
    mcc->no_data_aborts = true;

    memory_region_add_subregion(address_space, 0, machine->ram);

    /*
     * Stand-in for the R3000 data cache while Status.IsC is set.  Firmware
     * zeros this range to initialise the cache without clearing its own code;
     * redirecting those accesses keeps the executed code intact.
     */
    memory_region_init_ram(cache_shadow, NULL, "r3kjazz.cache-shadow",
                           0x04000000, &error_fatal);
    memory_region_add_subregion(address_space, 0x20000000, cache_shadow);

    memory_region_init_rom(bios, NULL, "r3kjazz.bios", R3K_BIOS_SIZE,
                           &error_fatal);
    memory_region_init_alias(bios2, NULL, "r3kjazz.bios", bios, 0,
                             R3K_BIOS_SIZE);
    memory_region_add_subregion(address_space, 0x1fc00000LL, bios);
    memory_region_add_subregion(address_space, 0xfff00000LL, bios2);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS,
                              machine->firmware ?: bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, 0xfff00000LL, R3K_BIOS_SIZE,
                                        NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }
    if ((bios_size < 0 || bios_size > R3K_BIOS_SIZE)
        && machine->firmware && !qtest_enabled()) {
        error_report("Could not load R3000 Jazz PROM '%s'", machine->firmware);
        exit(1);
    }

    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    /* MCT-ADR (RC4030-compatible) bus controller */
    adr = rc4030_init_rev1(&dmas, &dma_mr);
    sysbus = SYS_BUS_DEVICE(adr);
    /*
     * MAME's MIPS-I core maps INPUT_LINE_IRQn to CP0 Cause bit 10 + n,
     * while QEMU's env->irq[n] maps to bit 8 + n.  The local bus is
     * INPUT_LINE_IRQ1 in the Jazz board, so use IRQ3 here to land in
     * IMEX1/Cause bit 11 where the ARC PROM enables it.
     */
    sysbus_connect_irq(sysbus, 0, env->irq[4]); /* interval timer */
    sysbus_connect_irq(sysbus, 1, env->irq[3]); /* local bus / IRQ1 */
    sysbus_mmio_map(sysbus, 0, 0x80000000);
    sysbus_mmio_map(sysbus, 1, 0xf0000000);

    memory_region_init_io(dma_dummy, NULL, &dma_dummy_ops, NULL, "dummy_dma",
                          0x1000);
    memory_region_add_subregion(address_space, 0x8000d000, dma_dummy);

    memory_region_init_rom(dp8393x_prom, NULL, "dp8393x-jazz.prom",
                           SONIC_PROM_SIZE, &error_fatal);
    memory_region_add_subregion(address_space, 0x8000b000, dp8393x_prom);

    /* ISA/EISA bus: I/O at 0x90000000, memory at 0x91000000 */
    memory_region_init_io(isa_io, NULL, &unassigned_io_ops, NULL, "isa-io",
                          0x00010000);
    memory_region_init(isa_mem, NULL, "isa-mem", 0x01000000);
    memory_region_add_subregion(address_space, 0x90000000, isa_io);
    memory_region_add_subregion(address_space, 0x91000000, isa_mem);
    isa_bus = isa_bus_new(NULL, isa_mem, isa_io, &error_abort);

    /* The i82357 ISP routes ISA interrupts to CPU IRQ2. */
    i8259 = i8259_init(isa_bus, env->irq[2]);
    isa_bus_register_input_irqs(isa_bus, i8259);
    i8257_dma_init(OBJECT(adr), isa_bus, true);
    pit = isa_new(TYPE_I8254);
    qdev_prop_set_uint32(DEVICE(pit), "iobase", 0x40);
    qdev_prop_set_bit(DEVICE(pit), "idle-at-reset", true);
    isa_realize_and_unref(pit, isa_bus, &error_fatal);
    qdev_connect_gpio_out(DEVICE(pit), 0, isa_bus_get_irq(isa_bus, 0));
    pcspk = isa_new(TYPE_PC_SPEAKER);
    object_property_set_link(OBJECT(pcspk), "pit", OBJECT(pit), &error_fatal);
    isa_realize_and_unref(pcspk, isa_bus, &error_fatal);

    /* G300 framebuffer */
    dev = qdev_new(TYPE_G300FB);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, 0x60000000);
    sysbus_mmio_map(sysbus, 1, 0x40000000);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(adr, 3));

    /* Network */
    r3kjazz_init_net(dma_mr, adr, dp8393x_prom);

    /* NCR53C94 SCSI (Emulex FAS216 compatible) */
    dev = qdev_new(TYPE_SYSBUS_ESP);
    sysbus_esp = SYSBUS_ESP(dev);
    esp = &sysbus_esp->esp;
    esp->dma_memory_read = rc4030_dma_read;
    esp->dma_memory_write = rc4030_dma_write;
    esp->dma_opaque = dmas[0];
    sysbus_esp->it_shift = 0;
    esp->dma_enabled = 1;
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(adr, 5));
    sysbus_mmio_map(sysbus, 0, 0x80002000);
    scsi_bus_legacy_handle_cmdline(&esp->bus);

    /* Floppy */
    for (n = 0; n < MAX_FD; n++) {
        fds[n] = drive_get(IF_FLOPPY, 0, n);
    }
    fdctrl_init_sysbus(qdev_get_gpio_in(adr, 1), 0x80003000, fds,
                       OBJECT(adr), 1);

    /* RTC */
    mc146818rtc_set_cmos_data(mc146818_rtc_init(isa_bus, 1980, NULL),
                              RTC_REG_B, REG_B_24H | REG_B_DM);
    memory_region_init_io(rtc, NULL, &rtc_ops, NULL, "rtc", 0x1000);
    memory_region_add_subregion(address_space, 0x80004000, rtc);

    /* Keyboard controller */
    i8042 = I8042_MMIO(qdev_new(TYPE_I8042_MMIO));
    qdev_prop_set_uint64(DEVICE(i8042), "mask", 1);
    qdev_prop_set_uint32(DEVICE(i8042), "size", 0x1000);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(i8042), &error_fatal);
    qdev_connect_gpio_out(DEVICE(i8042), I8042_KBD_IRQ,
                          qdev_get_gpio_in(adr, 6));
    qdev_connect_gpio_out(DEVICE(i8042), I8042_MOUSE_IRQ,
                          qdev_get_gpio_in(adr, 7));
    memory_region_add_subregion(address_space, 0x80005000,
                                sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(i8042), 0));

    dev = qdev_new(TYPE_VMMOUSE_MMIO);
    object_property_set_link(OBJECT(dev), "i8042", OBJECT(i8042),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(address_space, 0x8000e000,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));

    /* Serial ports (WD16C552) */
    serial_mm_init(address_space, 0x80006000, 0, qdev_get_gpio_in(adr, 8),
                   4300800 / 16, serial_hd(0), DEVICE_NATIVE_ENDIAN);
    serial_mm_init(address_space, 0x80007000, 0, qdev_get_gpio_in(adr, 9),
                   4300800 / 16, serial_hd(1), DEVICE_NATIVE_ENDIAN);

    /* Parallel port */
    if (parallel_hds[0]) {
        parallel_mm_init(address_space, 0x80008000, 0,
                         qdev_get_gpio_in(adr, 0), parallel_hds[0]);
    }

    /* NVRAM */
    dev = qdev_new("ds1225y");
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, 0x80009000);

    /* 7-segment diagnostic LED */
    dev = qdev_new("jazz-led");
    qdev_prop_set_bit(dev, "decoder", true);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, 0x8000f000);
}

static bool r3kjazz_get_nt_idle(Object *obj, Error **errp)
{
    return R3KJAZZ_MACHINE(obj)->nt_idle;
}

static void r3kjazz_set_nt_idle(Object *obj, bool value, Error **errp)
{
    R3KJAZZ_MACHINE(obj)->nt_idle = value;
}

static void r3kjazz_instance_init(Object *obj)
{
    R3KJAZZ_MACHINE(obj)->nt_idle = true;
}

static void r3kjazz_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_bool(oc, "nt-idle", r3kjazz_get_nt_idle,
                                   r3kjazz_set_nt_idle);
    object_class_property_set_description(oc, "nt-idle",
        "Halt the CPU in the Windows NT idle loop instead of spinning");

    mc->desc = "Microsoft Jazz platform prototype (R3000)";
    mc->init = r3kjazz_init;
    mc->block_default_type = IF_SCSI;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R3000A");
    mc->default_ram_size = 16 * MiB;
    mc->default_ram_id = "r3kjazz.ram";
}

static const TypeInfo r3kjazz_type = {
    .name = TYPE_R3KJAZZ_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(R3kJazzMachineState),
    .instance_init = r3kjazz_instance_init,
    .class_init = r3kjazz_class_init,
};

static void r3kjazz_machine_init(void)
{
    type_register_static(&r3kjazz_type);
}

type_init(r3kjazz_machine_init)
