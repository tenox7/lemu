/*
 * Intel 82091AA Advanced Integrated Peripheral (AIP)
 *
 * A floppy controller, two 16550 UARTs, a parallel port and an IDE
 * interface, set up through an index/data pair of configuration ports.
 * Only software configuration is modelled; the hardware configuration
 * straps are not.
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/isa/superio.h"
#include "hw/isa/isa.h"
#include "hw/block/fdc.h"
#include "hw/char/parallel-isa.h"
#include "hw/char/serial-isa.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(I82091AAState, I82091AA_SUPERIO)

#define AIPID       0x00
#define AIPREV      0x01
#define AIPCFG1     0x02
#define AIPCFG2     0x03
#define FCFG1       0x10
#define FCFG2       0x11
#define PCFG1       0x20
#define PCFG2       0x21
#define SACFG1      0x30
#define SACFG2      0x31
#define SBCFG1      0x40
#define SBCFG2      0x41
#define IDECFG      0x50
#define NB_REGS     0x51

#define CFG_EN      0x01    /* bit 0 of FCFG1, PCFG1, SxCFG1, IDECFG */
#define FCFG1_370   0x02
#define PCFG1_ADDR  0x06
#define PCFG1_IRQ7  0x08
#define SCFG1_ADDR  0x0e
#define SCFG1_IRQ4  0x10
#define IDECFG_SEC  0x02

struct I82091AAState {
    ISASuperIODevice parent_obj;

    MemoryRegion io;
    uint16_t iobase;
    /* Devices are created once, then moved and switched on or off */
    bool creating;

    uint8_t index;
    uint8_t regs[NB_REGS];
};

static const uint16_t uart_base[8] = {
    0x3f8, 0x2f8, 0x220, 0x228, 0x238, 0x2e8, 0x338, 0x3e8
};

static const uint16_t parallel_base[4] = { 0x378, 0x278, 0x3bc, 0 };

static uint8_t uart_cfg(I82091AAState *s, uint8_t i)
{
    return s->regs[i ? SBCFG1 : SACFG1];
}

/* Parallel port */

static bool is_parallel_enabled(ISASuperIODevice *sio, uint8_t index)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);
    uint8_t cfg = s->regs[PCFG1];

    return s->creating ||
           ((cfg & CFG_EN) && parallel_base[(cfg & PCFG1_ADDR) >> 1]);
}

static uint16_t get_parallel_iobase(ISASuperIODevice *sio, uint8_t index)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);

    return parallel_base[(s->regs[PCFG1] & PCFG1_ADDR) >> 1] ?: 0x378;
}

static unsigned int get_parallel_irq(ISASuperIODevice *sio, uint8_t index)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);

    return (s->regs[PCFG1] & PCFG1_IRQ7) ? 7 : 5;
}

/* UARTs */

static bool is_uart_enabled(ISASuperIODevice *sio, uint8_t i)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);

    return s->creating || (uart_cfg(s, i) & CFG_EN);
}

static uint16_t get_uart_iobase(ISASuperIODevice *sio, uint8_t i)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);

    return uart_base[(uart_cfg(s, i) & SCFG1_ADDR) >> 1];
}

static unsigned int get_uart_irq(ISASuperIODevice *sio, uint8_t i)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);

    return (uart_cfg(s, i) & SCFG1_IRQ4) ? 4 : 3;
}

/* Floppy controller */

static bool is_fdc_enabled(ISASuperIODevice *sio, uint8_t index)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);

    return s->creating || (s->regs[FCFG1] & CFG_EN);
}

static uint16_t get_fdc_iobase(ISASuperIODevice *sio, uint8_t index)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);

    return (s->regs[FCFG1] & FCFG1_370) ? 0x370 : 0x3f0;
}

static unsigned int get_fdc_irq(ISASuperIODevice *sio, uint8_t index)
{
    return 6;
}

static unsigned int get_fdc_dma(ISASuperIODevice *sio, uint8_t index)
{
    return 2;
}

/* IDE interface */

static bool is_ide_enabled(ISASuperIODevice *sio, uint8_t index)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);

    return s->creating || (s->regs[IDECFG] & CFG_EN);
}

static uint16_t get_ide_iobase(ISASuperIODevice *sio, uint8_t index)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);
    uint16_t base = (s->regs[IDECFG] & IDECFG_SEC) ? 0x170 : 0x1f0;

    return index ? base + 0x206 : base;
}

static unsigned int get_ide_irq(ISASuperIODevice *sio, uint8_t index)
{
    I82091AAState *s = I82091AA_SUPERIO(sio);

    return (s->regs[IDECFG] & IDECFG_SEC) ? 15 : 14;
}

/*
 * The UART, parallel and IDE interrupt lines are fixed once the devices
 * exist: the BeBox boot ROM sets them up the way the board wires them.
 */
static void i82091aa_check_irq(I82091AAState *s, const char *dev,
                               unsigned int want, unsigned int have)
{
    if (want != have) {
        qemu_log_mask(LOG_UNIMP, "i82091aa: %s moved to IRQ %u, "
                      "but it stays on IRQ %u\n", dev, want, have);
    }
}

static void i82091aa_reconfigure(I82091AAState *s)
{
    ISASuperIODevice *sio = ISA_SUPERIO(s);
    int i;

    if (sio->parallel[0]) {
        isa_parallel_set_enabled(sio->parallel[0],
                                 is_parallel_enabled(sio, 0));
        isa_parallel_set_iobase(sio->parallel[0],
                                get_parallel_iobase(sio, 0));
        if (s->regs[PCFG1] & CFG_EN) {
            i82091aa_check_irq(s, "parallel port", get_parallel_irq(sio, 0),
                               object_property_get_uint(
                                   OBJECT(sio->parallel[0]), "irq", NULL));
        }
    }
    for (i = 0; i < 2; i++) {
        if (sio->serial[i]) {
            isa_serial_set_enabled(sio->serial[i], is_uart_enabled(sio, i));
            isa_serial_set_iobase(sio->serial[i], get_uart_iobase(sio, i));
            if (uart_cfg(s, i) & CFG_EN) {
                i82091aa_check_irq(s, i ? "serial port B" : "serial port A",
                                   get_uart_irq(sio, i),
                                   object_property_get_uint(
                                       OBJECT(sio->serial[i]), "irq", NULL));
            }
        }
    }
    if (sio->floppy) {
        isa_fdc_set_enabled(sio->floppy, is_fdc_enabled(sio, 0));
        isa_fdc_set_iobase(sio->floppy, get_fdc_iobase(sio, 0));
    }
    if (sio->ide && (s->regs[IDECFG] & (IDECFG_SEC | CFG_EN)) != CFG_EN) {
        qemu_log_mask(LOG_UNIMP, "i82091aa: IDE can only be the enabled "
                      "primary interface (IDECFG=%02x)\n", s->regs[IDECFG]);
    }
}

static void i82091aa_reset(DeviceState *d)
{
    I82091AAState *s = I82091AA_SUPERIO(d);

    /* Software motherboard configuration defaults */
    memset(s->regs, 0, sizeof(s->regs));
    s->index = 0;
    s->regs[AIPCFG1] = (s->iobase >= 0x100 ? 0x10 : 0) |
                       ((s->iobase == 0x24 || s->iobase == 0x398) ? 0x08 : 0);
    s->regs[FCFG1] = CFG_EN;
    s->regs[SACFG1] = SCFG1_IRQ4;
    s->regs[IDECFG] = CFG_EN;
    i82091aa_reconfigure(s);
}

static uint8_t i82091aa_reg_read(I82091AAState *s, uint8_t reg)
{
    switch (reg) {
    case AIPID:
        return 0xa0;
    case AIPREV:
        return 0x00;
    case FCFG2:
        /* The FDC is always idle */
        return s->regs[FCFG2] | 0x02;
    case AIPCFG1:
    case AIPCFG2:
    case FCFG1:
    case PCFG1:
    case PCFG2:
    case SACFG1:
    case SACFG2:
    case SBCFG1:
    case SBCFG2:
    case IDECFG:
        return s->regs[reg];
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "i82091aa: read of register %02x\n",
                      reg);
        return 0xff;
    }
}

static void i82091aa_reg_write(I82091AAState *s, uint8_t reg, uint8_t val)
{
    switch (reg) {
    case AIPCFG1:
        s->regs[reg] = (s->regs[reg] & 0x48) | (val & 0x31);
        return;
    case AIPCFG2:
        s->regs[reg] = val & 0xf8;
        return;
    case FCFG1:
        s->regs[reg] = val & 0x83;
        break;
    case FCFG2:
        s->regs[reg] = val & 0x0d;
        return;
    case PCFG1:
        s->regs[reg] = val & 0xef;
        break;
    case PCFG2:
        s->regs[reg] = val & 0x2f;
        return;
    case SACFG1:
    case SBCFG1:
        s->regs[reg] = val & 0x9f;
        break;
    case SACFG2:
    case SBCFG2:
        s->regs[reg] = val & 0x1f;
        return;
    case IDECFG:
        s->regs[reg] = val & 0x07;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "i82091aa: write of %02x to register %02x\n", val, reg);
        return;
    }
    i82091aa_reconfigure(s);
}

static uint64_t i82091aa_io_read(void *opaque, hwaddr addr, unsigned size)
{
    I82091AAState *s = opaque;
    uint8_t val = addr ? i82091aa_reg_read(s, s->index) : s->index;

    trace_i82091aa_io_read(addr, s->index, val);
    return val;
}

static void i82091aa_io_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    I82091AAState *s = opaque;

    trace_i82091aa_io_write(addr, s->index, val);
    if (addr) {
        i82091aa_reg_write(s, s->index, val);
    } else {
        s->index = val;
    }
}

static const MemoryRegionOps i82091aa_io_ops = {
    .read = i82091aa_io_read,
    .write = i82091aa_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void i82091aa_realize(DeviceState *dev, Error **errp)
{
    I82091AAState *s = I82091AA_SUPERIO(dev);
    ISASuperIOClass *sc = ISA_SUPERIO_GET_CLASS(dev);
    Error *local_err = NULL;

    switch (s->iobase) {
    case 0x22:
    case 0x24:
    case 0x26e:
    case 0x398:
        break;
    default:
        error_setg(errp, "i82091aa: iobase must be 0x22, 0x24, 0x26e "
                   "or 0x398");
        return;
    }
    isa_register_ioport(ISA_DEVICE(dev), &s->io, s->iobase);

    /*
     * Create every device, with the addresses and interrupt lines the
     * BeBox boot ROM picks, then let reset switch them off.
     */
    s->regs[FCFG1] = CFG_EN;
    s->regs[PCFG1] = CFG_EN | PCFG1_IRQ7;
    s->regs[SACFG1] = CFG_EN | SCFG1_IRQ4;
    s->regs[SBCFG1] = CFG_EN | (1 << 1);
    s->regs[IDECFG] = CFG_EN;
    s->creating = true;
    sc->parent_realize(dev, &local_err);
    s->creating = false;
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    i82091aa_reset(dev);
}

static void i82091aa_init(Object *obj)
{
    I82091AAState *s = I82091AA_SUPERIO(obj);

    memory_region_init_io(&s->io, obj, &i82091aa_io_ops, s, "i82091aa", 2);
}

static int i82091aa_post_load(void *opaque, int version_id)
{
    i82091aa_reconfigure(opaque);
    return 0;
}

static const VMStateDescription vmstate_i82091aa = {
    .name = "i82091aa",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = i82091aa_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(index, I82091AAState),
        VMSTATE_UINT8_ARRAY(regs, I82091AAState, NB_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property i82091aa_properties[] = {
    DEFINE_PROP_UINT16("iobase", I82091AAState, iobase, 0x398),
};

static void i82091aa_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ISASuperIOClass *sc = ISA_SUPERIO_CLASS(klass);

    device_class_set_legacy_reset(dc, i82091aa_reset);
    device_class_set_parent_realize(dc, i82091aa_realize,
                                    &sc->parent_realize);
    device_class_set_props(dc, i82091aa_properties);
    dc->vmsd = &vmstate_i82091aa;
    dc->desc = "Intel 82091AA Advanced Integrated Peripheral";

    sc->parallel = (ISASuperIOFuncs){
        .count = 1,
        .is_enabled = is_parallel_enabled,
        .get_iobase = get_parallel_iobase,
        .get_irq    = get_parallel_irq,
    };
    sc->serial = (ISASuperIOFuncs){
        .count = 2,
        .is_enabled = is_uart_enabled,
        .get_iobase = get_uart_iobase,
        .get_irq    = get_uart_irq,
    };
    sc->floppy = (ISASuperIOFuncs){
        .count = 1,
        .is_enabled = is_fdc_enabled,
        .get_iobase = get_fdc_iobase,
        .get_irq    = get_fdc_irq,
        .get_dma    = get_fdc_dma,
    };
    sc->ide = (ISASuperIOFuncs){
        .count = 1,
        .is_enabled = is_ide_enabled,
        .get_iobase = get_ide_iobase,
        .get_irq    = get_ide_irq,
    };
}

static const TypeInfo i82091aa_type_info = {
    .name          = TYPE_I82091AA_SUPERIO,
    .parent        = TYPE_ISA_SUPERIO,
    .instance_size = sizeof(I82091AAState),
    .instance_init = i82091aa_init,
    .class_init    = i82091aa_class_init,
};

static void i82091aa_register_types(void)
{
    type_register_static(&i82091aa_type_info);
}

type_init(i82091aa_register_types)
