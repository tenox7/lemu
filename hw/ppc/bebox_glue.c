/*
 * BeBox motherboard logic
 *
 * Two devices live here:
 *
 * "bebox-glue" is the pair of Altera FPGAs on the local bus at 0x7ffff000
 * that route the interrupt sources to the two CPUs and control CPU 1.
 * Registers (32-bit, big-endian, at 0x0f0 + 0x100 * n):
 *
 *   0x0f0  CPU 0 interrupt mask
 *   0x1f0  CPU 1 interrupt mask
 *   0x2f0  interrupt sources (read-only)
 *   0x3f0  cross-processor: SMI to each CPU, TLBISYNC, WHO_AM_I
 *   0x4f0  CPU 1 resets, 82378 FLUSHREQ/MEMACK swap, disk LED
 *   0x5f0  unknown ("Larry's flytrap" in Be's sources)
 *
 * Writes set or clear: bit 31 is the new value and every other bit
 * written as 1 takes it. The sources and both masks share one layout
 * (bit 30 and 29 are the SMI enables of CPU 0 and CPU 1); a CPU gets INT
 * when a source is set in its mask.
 *
 * "bebox-io" holds the small ISA ports of the I/O card: the A/D converter,
 * the GeekPort, the infrared interface, the joystick and the front-panel
 * LEDs at 0xc00, and the 82378's port 92 (reset).
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include "system/runstate.h"
#include "hw/core/irq.h"
#include "hw/isa/isa.h"
#include "hw/ppc/bebox.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

/* bebox-glue */

OBJECT_DECLARE_SIMPLE_TYPE(BeBoxGlueState, BEBOX_GLUE)

#define GLUE_SET            0x80000000u
#define GLUE_SMI(n)         (0x40000000u >> (n))
#define GLUE_INT_SOURCES    0x0ffffffcu

#define XP_CPU1_INT         0x10000000u
#define XP_TLBISYNC(n)      (0x08000000u >> (n))
#define XP_WHO_AM_I         0x02000000u
#define XP_WRITABLE         0x7c000000u

#define RS_CPU1_SRESET      0x40000000u
#define RS_CPU1_HRESET      0x20000000u
#define RS_SIO_FIX          0x10000000u
#define RS_DISK_LED         0x01000000u

struct BeBoxGlueState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq cpu_int[2];
    qemu_irq cpu_smi[2];
    qemu_irq cpu1_hreset;
    qemu_irq cpu1_sreset;

    uint32_t sources;
    uint32_t mask[2];
    uint32_t xproc;
    uint32_t resets;
};

static void glue_update(BeBoxGlueState *s)
{
    int n;

    for (n = 0; n < 2; n++) {
        qemu_set_irq(s->cpu_int[n],
                     !!(s->sources & s->mask[n] & GLUE_INT_SOURCES));
        qemu_set_irq(s->cpu_smi[n],
                     !!(s->xproc & s->mask[n] & GLUE_SMI(n)));
    }
    /* Both resets are active low */
    qemu_set_irq(s->cpu1_hreset, !(s->resets & RS_CPU1_HRESET));
    qemu_set_irq(s->cpu1_sreset, !(s->resets & RS_CPU1_SRESET));
}

static uint32_t glue_set_clear(uint32_t reg, uint32_t val, uint32_t writable)
{
    uint32_t bits = val & ~GLUE_SET & writable;

    return (val & GLUE_SET) ? reg | bits : reg & ~bits;
}

static int glue_cpu(void)
{
    return current_cpu ? current_cpu->cpu_index : 0;
}

static uint64_t glue_read(void *opaque, hwaddr addr, unsigned size)
{
    BeBoxGlueState *s = opaque;
    uint32_t val;

    switch (addr) {
    case 0x0f0:
        val = s->mask[0];
        break;
    case 0x1f0:
        val = s->mask[1];
        break;
    case 0x2f0:
        val = s->sources;
        break;
    case 0x3f0:
        val = s->xproc | (glue_cpu() == 1 ? XP_WHO_AM_I : 0);
        break;
    case 0x4f0:
        val = s->resets;
        break;
    case 0x5f0:
        val = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bebox-glue: read at 0x%03"
                      HWADDR_PRIx "\n", addr);
        val = 0;
        break;
    }
    trace_bebox_glue_read(glue_cpu(), addr, val);
    return val;
}

static void glue_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    BeBoxGlueState *s = opaque;
    uint32_t old;

    trace_bebox_glue_write(glue_cpu(), addr, val);
    switch (addr) {
    case 0x0f0:
        s->mask[0] = glue_set_clear(s->mask[0], val,
                                    GLUE_SMI(0) | GLUE_INT_SOURCES);
        break;
    case 0x1f0:
        s->mask[1] = glue_set_clear(s->mask[1], val,
                                    GLUE_SMI(1) | GLUE_INT_SOURCES);
        break;
    case 0x3f0:
        s->xproc = glue_set_clear(s->xproc, val, XP_WRITABLE);
        if (s->xproc & XP_CPU1_INT) {
            qemu_log_mask(LOG_UNIMP, "bebox-glue: CPU1_INT is for "
                          "revision 1 boards\n");
        }
        break;
    case 0x4f0:
        old = s->resets;
        s->resets = glue_set_clear(s->resets, val, 0x7fffffff);
        if ((old ^ s->resets) & RS_DISK_LED) {
            trace_bebox_glue_disk_led(!!(s->resets & RS_DISK_LED));
        }
        break;
    case 0x2f0:
    case 0x5f0:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bebox-glue: write at 0x%03"
                      HWADDR_PRIx "\n", addr);
        break;
    }
    glue_update(s);
}

static const MemoryRegionOps glue_ops = {
    .read = glue_read,
    .write = glue_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void glue_set_source(void *opaque, int n, int level)
{
    BeBoxGlueState *s = opaque;
    uint32_t old = s->sources;

    if (level) {
        s->sources |= 1u << n;
    } else {
        s->sources &= ~(1u << n);
    }
    if (old != s->sources) {
        glue_update(s);
    }
}

static void glue_reset_hold(Object *obj, ResetType type)
{
    BeBoxGlueState *s = BEBOX_GLUE(obj);

    s->mask[0] = s->mask[1] = 0;
    s->xproc = XP_TLBISYNC(0) | XP_TLBISYNC(1);
    /* CPU 1 starts in reset */
    s->resets = 0;
}

static void glue_reset_exit(Object *obj, ResetType type)
{
    glue_update(BEBOX_GLUE(obj));
}

static void glue_init(Object *obj)
{
    BeBoxGlueState *s = BEBOX_GLUE(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &glue_ops, s, "bebox-glue", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_in(dev, glue_set_source, 32);
    qdev_init_gpio_out_named(dev, s->cpu_int, "cpu-int", 2);
    qdev_init_gpio_out_named(dev, s->cpu_smi, "cpu-smi", 2);
    qdev_init_gpio_out_named(dev, &s->cpu1_hreset, "cpu1-hreset", 1);
    qdev_init_gpio_out_named(dev, &s->cpu1_sreset, "cpu1-sreset", 1);
}

static const VMStateDescription vmstate_glue = {
    .name = "bebox-glue",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sources, BeBoxGlueState),
        VMSTATE_UINT32_ARRAY(mask, BeBoxGlueState, 2),
        VMSTATE_UINT32(xproc, BeBoxGlueState),
        VMSTATE_UINT32(resets, BeBoxGlueState),
        VMSTATE_END_OF_LIST()
    }
};

static void glue_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = glue_reset_hold;
    rc->phases.exit = glue_reset_exit;
    dc->vmsd = &vmstate_glue;
    dc->desc = "BeBox motherboard interrupt and CPU control";
    dc->user_creatable = false;
}

/* bebox-io */

OBJECT_DECLARE_SIMPLE_TYPE(BeBoxIOState, BEBOX_IO)

#define ADC_DONE    0x0800

struct BeBoxIOState {
    ISADevice parent_obj;

    MemoryRegion geek;
    MemoryRegion dac;
    MemoryRegion led;
    MemoryRegion port92;

    uint16_t adc_control;
    uint16_t adc_data;
    uint8_t adc_rate;
    uint8_t geek_oe;
    uint8_t joy_mux;
    uint8_t dac_regs[4];
    uint8_t digital[2];
    uint8_t led_value;
    uint8_t port92_value;
};

/*
 * MAX186 single-ended channel for each SEL2-SEL0 code. Channel 6 tells
 * the I/O card revision (pulled high on revision 5), 4 and 5 are the
 * joystick axes (centred), the rest are grounded GeekPort inputs.
 */
static uint16_t bebox_adc_convert(uint16_t control)
{
    static const uint8_t sel_to_channel[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };
    int channel = sel_to_channel[(control >> 4) & 7];

    switch (channel) {
    case 4:
    case 5:
        return 0x800;
    case 6:
        return 0xfff;
    default:
        return 0;
    }
}

/* 0x360-0x36f: A/D converter, GeekPort enable, infrared, joystick mux */
static uint64_t bebox_geek_read(void *opaque, hwaddr addr, unsigned size)
{
    BeBoxIOState *s = opaque;
    uint64_t val;

    switch (addr) {
    case 0x0:
        val = s->adc_control | ADC_DONE;
        break;
    case 0x2:
        val = s->adc_data;
        break;
    case 0x4:
        val = s->adc_rate;
        break;
    case 0x6:
        val = s->geek_oe;
        break;
    case 0xe:
        val = s->joy_mux;
        break;
    default:
        /* Infrared controller (0x368-0x36a) is not there */
        val = 0xff;
        break;
    }
    if (size == 1 && (addr & 1)) {
        val = 0;
    }
    trace_bebox_io_read(0x360 + addr, val);
    return val;
}

static void bebox_geek_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    BeBoxIOState *s = opaque;

    trace_bebox_io_write(0x360 + addr, val);
    switch (addr) {
    case 0x0:
        s->adc_control = val & ~ADC_DONE;
        s->adc_data = bebox_adc_convert(s->adc_control);
        break;
    case 0x4:
        s->adc_rate = val;
        break;
    case 0x6:
        s->geek_oe = val;
        break;
    case 0xe:
        s->joy_mux = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps bebox_geek_ops = {
    .read = bebox_geek_read,
    .write = bebox_geek_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

/* 0x390-0x397: D/A converter, GeekPort digital ports, joystick buttons */
static uint64_t bebox_dac_read(void *opaque, hwaddr addr, unsigned size)
{
    BeBoxIOState *s = opaque;
    uint64_t val;

    switch (addr) {
    case 0x0 ... 0x3:
        val = s->dac_regs[addr];
        break;
    case 0x4 ... 0x5:
        val = s->digital[addr - 4];
        break;
    case 0x7:
        /* No joystick button pressed */
        val = 0xff;
        break;
    default:
        val = 0xff;
        break;
    }
    trace_bebox_io_read(0x390 + addr, val);
    return val;
}

static void bebox_dac_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    BeBoxIOState *s = opaque;

    trace_bebox_io_write(0x390 + addr, val);
    switch (addr) {
    case 0x0 ... 0x3:
        s->dac_regs[addr] = val;
        break;
    case 0x4 ... 0x5:
        s->digital[addr - 4] = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps bebox_dac_ops = {
    .read = bebox_dac_read,
    .write = bebox_dac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* 0xc00: front-panel LED bargraphs, left in bits 7-4, right in bits 3-0 */
static uint64_t bebox_led_read(void *opaque, hwaddr addr, unsigned size)
{
    BeBoxIOState *s = opaque;

    return s->led_value;
}

static void bebox_led_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    BeBoxIOState *s = opaque;

    if (s->led_value != (uint8_t)val) {
        s->led_value = val;
        trace_bebox_io_led(val >> 4, val & 0xf);
    }
}

static const MemoryRegionOps bebox_led_ops = {
    .read = bebox_led_read,
    .write = bebox_led_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * 0x92: the 82378's port 92, alternate reset in bit 0 and alternate A20
 * gate in bit 1. BeOS restarts the BeBox by setting bit 0.
 */
static uint64_t bebox_port92_read(void *opaque, hwaddr addr, unsigned size)
{
    BeBoxIOState *s = opaque;

    return s->port92_value;
}

static void bebox_port92_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    BeBoxIOState *s = opaque;
    uint8_t old = s->port92_value;

    trace_bebox_io_write(0x92, val);
    s->port92_value = val & 0x03;
    if (!(old & 0x01) && (val & 0x01)) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static const MemoryRegionOps bebox_port92_ops = {
    .read = bebox_port92_read,
    .write = bebox_port92_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void bebox_io_reset(DeviceState *dev)
{
    BeBoxIOState *s = BEBOX_IO(dev);

    s->adc_control = 0;
    s->adc_data = 0;
    s->adc_rate = 0;
    s->geek_oe = 0;
    s->joy_mux = 0;
    memset(s->dac_regs, 0, sizeof(s->dac_regs));
    memset(s->digital, 0, sizeof(s->digital));
    s->led_value = 0;
    s->port92_value = 0;
}

static void bebox_io_realize(DeviceState *dev, Error **errp)
{
    BeBoxIOState *s = BEBOX_IO(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    memory_region_init_io(&s->geek, OBJECT(s), &bebox_geek_ops, s,
                          "bebox-geekport", 0x10);
    isa_register_ioport(isa, &s->geek, 0x360);
    memory_region_init_io(&s->dac, OBJECT(s), &bebox_dac_ops, s,
                          "bebox-dac", 0x8);
    isa_register_ioport(isa, &s->dac, 0x390);
    memory_region_init_io(&s->led, OBJECT(s), &bebox_led_ops, s,
                          "bebox-led", 0x1);
    isa_register_ioport(isa, &s->led, 0xc00);
    memory_region_init_io(&s->port92, OBJECT(s), &bebox_port92_ops, s,
                          "bebox-port92", 0x1);
    isa_register_ioport(isa, &s->port92, 0x92);
}

static const VMStateDescription vmstate_bebox_io = {
    .name = "bebox-io",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(adc_control, BeBoxIOState),
        VMSTATE_UINT16(adc_data, BeBoxIOState),
        VMSTATE_UINT8(adc_rate, BeBoxIOState),
        VMSTATE_UINT8(geek_oe, BeBoxIOState),
        VMSTATE_UINT8(joy_mux, BeBoxIOState),
        VMSTATE_UINT8_ARRAY(dac_regs, BeBoxIOState, 4),
        VMSTATE_UINT8_ARRAY(digital, BeBoxIOState, 2),
        VMSTATE_UINT8(led_value, BeBoxIOState),
        VMSTATE_UINT8(port92_value, BeBoxIOState),
        VMSTATE_END_OF_LIST()
    }
};

static void bebox_io_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bebox_io_realize;
    device_class_set_legacy_reset(dc, bebox_io_reset);
    dc->vmsd = &vmstate_bebox_io;
    dc->desc = "BeBox I/O card ports";
    dc->user_creatable = false;
}

static const TypeInfo bebox_glue_types[] = {
    {
        .name = TYPE_BEBOX_GLUE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BeBoxGlueState),
        .instance_init = glue_init,
        .class_init = glue_class_init,
    },
    {
        .name = TYPE_BEBOX_IO,
        .parent = TYPE_ISA_DEVICE,
        .instance_size = sizeof(BeBoxIOState),
        .class_init = bebox_io_class_init,
    },
};

DEFINE_TYPES(bebox_glue_types)
