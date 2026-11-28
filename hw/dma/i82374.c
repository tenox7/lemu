/*
 * QEMU Intel 82374 emulation (Enhanced DMA controller)
 *
 * Copyright (c) 2010 Hervé Poussineau
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
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/isa/isa.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/dma/i8257.h"
#include "qom/object.h"

#define TYPE_I82374 "i82374"
OBJECT_DECLARE_SIMPLE_TYPE(I82374State, I82374)

//#define DEBUG_I82374

#ifdef DEBUG_I82374
#define DPRINTF(fmt, ...) \
do { fprintf(stderr, "i82374: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
do {} while (0)
#endif

struct I82374State {
    ISADevice parent_obj;

    uint32_t iobase;
    uint8_t commands[8];
    PortioList port_list;
    I8257State *dma[2];
    qemu_irq irq13;
    /* IRQ13 as the scatter-gather lists drive it */
    bool sg_irq_level;
};

static bool i82374_sg_irq_needed(void *opaque)
{
    I82374State *s = opaque;

    return s->sg_irq_level;
}

static const VMStateDescription vmstate_i82374_sg_irq = {
    .name = "i82374/sg-irq",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i82374_sg_irq_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(sg_irq_level, I82374State),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_i82374 = {
    .name = "i82374",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(commands, I82374State, 8),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_i82374_sg_irq,
        NULL
    },
};

/*
 * Both DMA controllers' scatter-gather interrupts go out on IRQ13, which
 * other devices may share: on the 40p it is also the 53C810's. Drive the
 * line only when the OR of the two changes, so that a master clear or a
 * list start lowers IRQ13 only if a list had raised it.
 */
static void i82374_set_sg_irq(void *opaque, int n, int level)
{
    I82374State *s = opaque;
    bool out = s->dma[0]->sg_irq || s->dma[1]->sg_irq;

    if (out != s->sg_irq_level) {
        s->sg_irq_level = out;
        qemu_set_irq(s->irq13, out);
    }
}

/*
 * The scatter-gather registers, one per channel (channel 4, the cascade,
 * has none): commands at 0x10, status at 0x18, descriptor table pointers
 * at 0x20. The DMA controllers do the work.
 */
static I8257State *i82374_chan(I82374State *s, int nchan, int *ichan)
{
    *ichan = nchan & 3;
    return s->dma[nchan >> 2];
}

static uint32_t i82374_read_isr(void *opaque, uint32_t nport)
{
    I82374State *s = opaque;
    uint32_t val;

    val = (i8257_sg_irq_status(s->dma[0]) & 0x0f) |
          ((i8257_sg_irq_status(s->dma[1]) & 0x0e) << 4);
    DPRINTF("%s: %08x=%08x\n", __func__, nport, val);
    return val;
}

static void i82374_write_command(void *opaque, uint32_t nport, uint32_t data)
{
    I82374State *s = opaque;
    int nchan = (nport - s->iobase) & 7;
    int ichan;
    I8257State *d = i82374_chan(s, nchan, &ichan);

    DPRINTF("%s: %08x=%08x\n", __func__, nport, data);
    if (nchan == 4) {
        return;
    }
    s->commands[nchan] = data;
    i8257_sg_command(d, ichan, data);
}

static uint32_t i82374_read_status(void *opaque, uint32_t nport)
{
    I82374State *s = opaque;
    int nchan = (nport - s->iobase) & 7;
    int ichan;
    I8257State *d = i82374_chan(s, nchan, &ichan);
    uint32_t val = nchan == 4 ? 0 : i8257_sg_status(d, ichan);

    DPRINTF("%s: %08x=%08x\n", __func__, nport, val);
    return val;
}

static void i82374_write_descriptor(void *opaque, uint32_t nport, uint32_t data)
{
    I82374State *s = opaque;
    int off = nport - s->iobase - 0x20;
    int nchan = off >> 2, shift = (off & 3) * 8, ichan;
    I8257State *d = i82374_chan(s, nchan, &ichan);
    uint32_t ptr;

    DPRINTF("%s: %08x=%08x\n", __func__, nport, data);
    if (nchan == 4) {
        return;
    }
    ptr = i8257_sg_get_pointer(d, ichan);
    ptr = (ptr & ~(0xffu << shift)) | ((data & 0xff) << shift);
    i8257_sg_set_pointer(d, ichan, ptr);
}

static uint32_t i82374_read_descriptor(void *opaque, uint32_t nport)
{
    I82374State *s = opaque;
    int off = nport - s->iobase - 0x20;
    int nchan = off >> 2, shift = (off & 3) * 8, ichan;
    I8257State *d = i82374_chan(s, nchan, &ichan);
    uint32_t val = 0;

    if (nchan != 4) {
        val = (i8257_sg_get_pointer(d, ichan) >> shift) & 0xff;
    }
    DPRINTF("%s: %08x=%08x\n", __func__, nport, val);
    return val;
}

/* The pointers are also written and read whole, as the 82378 allows */
static void i82374_write_descriptor32(void *opaque, uint32_t nport,
                                      uint32_t data)
{
    I82374State *s = opaque;
    int nchan = ((nport - s->iobase - 0x20) >> 2) & 7, ichan;
    I8257State *d = i82374_chan(s, nchan, &ichan);

    DPRINTF("%s: %08x=%08x\n", __func__, nport, data);
    if (nchan != 4) {
        i8257_sg_set_pointer(d, ichan, data);
    }
}

static uint32_t i82374_read_descriptor32(void *opaque, uint32_t nport)
{
    I82374State *s = opaque;
    int nchan = ((nport - s->iobase - 0x20) >> 2) & 7, ichan;
    I8257State *d = i82374_chan(s, nchan, &ichan);

    return nchan == 4 ? 0 : i8257_sg_get_pointer(d, ichan);
}

static const MemoryRegionPortio i82374_portio_list[] = {
    { 0x0A, 1, 1, .read = i82374_read_isr, },
    { 0x10, 8, 1, .write = i82374_write_command, },
    { 0x18, 8, 1, .read = i82374_read_status, },
    { 0x20, 0x20, 1,
      .write = i82374_write_descriptor, .read = i82374_read_descriptor, },
    { 0x20, 0x20, 4,
      .write = i82374_write_descriptor32, .read = i82374_read_descriptor32, },
    PORTIO_END_OF_LIST(),
};

static void i82374_realize(DeviceState *dev, Error **errp)
{
    I82374State *s = I82374(dev);
    ISABus *isa_bus = isa_bus_from_device(ISA_DEVICE(dev));

    if (isa_bus_get_dma(isa_bus, 0)) {
        error_setg(errp, "DMA already initialized on ISA bus");
        return;
    }
    i8257_dma_init(OBJECT(dev), isa_bus, true);
    s->dma[0] = I8257(isa_bus_get_dma(isa_bus, 0));
    s->dma[1] = I8257(isa_bus_get_dma(isa_bus, 4));
    /* the end of a scatter-gather list can raise IRQ13 */
    s->irq13 = isa_get_irq(ISA_DEVICE(dev), 13);
    qdev_init_gpio_in_named(dev, i82374_set_sg_irq, "sg-irq", 2);
    s->dma[0]->sg_irq_out = qdev_get_gpio_in_named(dev, "sg-irq", 0);
    s->dma[1]->sg_irq_out = qdev_get_gpio_in_named(dev, "sg-irq", 1);

    portio_list_init(&s->port_list, OBJECT(s), i82374_portio_list, s,
                     "i82374");
    portio_list_add(&s->port_list, isa_address_space_io(&s->parent_obj),
                    s->iobase);

    memset(s->commands, 0, sizeof(s->commands));
}

static const Property i82374_properties[] = {
    DEFINE_PROP_UINT32("iobase", I82374State, iobase, 0x400),
};

static void i82374_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    
    dc->realize = i82374_realize;
    dc->vmsd = &vmstate_i82374;
    device_class_set_props(dc, i82374_properties);
    dc->desc = "Intel 82374 DMA controller";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo i82374_info = {
    .name  = TYPE_I82374,
    .parent = TYPE_ISA_DEVICE,
    .instance_size  = sizeof(I82374State),
    .class_init = i82374_class_init,
};

static void i82374_register_types(void)
{
    type_register_static(&i82374_info);
}

type_init(i82374_register_types)
