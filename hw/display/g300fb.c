/*
 * QEMU INMOS G300 Colour Video Controller
 *
 * The G300 is the framebuffer used by the R3000 Jazz board.  It is a
 * simpler relative of the G364: a 256-entry palette, a top-of-screen
 * register and a control word selecting 1, 2, 4 or 8 bits per pixel.
 *
 * This file is based on the G300 model in the mame-r3kjazz fork by
 * Patrick Mackinlay and hyenasky.  Their work is BSD-3-Clause licensed;
 * this QEMU port is GPL-2.0-or-later, like the rest of LEMU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * References:
 *   http://bitsavers.org/components/inmos/graphics/72-TRN-204-01_Graphics_Databook_Second_Edition_1990.pdf
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/display/g300fb.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(G300State, G300FB)

/* Register offsets in the shifted (8 bytes per register) control space. */
#define G300_PALETTE        0x000
#define G300_HALFSYNC       0x908
#define G300_BACKPORCH      0x910
#define G300_DISPLAY        0x918
#define G300_SHORTDISPLAY   0x920
#define G300_BROADPULSE     0x928
#define G300_VSYNC          0x930
#define G300_VBLANK         0x938
#define G300_VDISPLAY       0x940
#define G300_LINETIME       0x948
#define G300_TOS            0x950
#define G300_MEMINIT        0x958
#define G300_TRANSFERDELAY  0x960
#define G300_MASK           0xa00
#define G300_CONTROL        0xb00
#define G300_TOS_ALIAS      0xc00
#define G300_BOOT           0xd00

#define G300_WIDTH          1280
#define G300_HEIGHT         1024

#define G300_CONTROL_BPP        0x00060000

#define G300_TIMING_FIRST       0x121
#define G300_TIMING_LAST        0x12c
#define G300_TIMING_COUNT       (G300_TIMING_LAST + 1)

struct G300State {
    SysBusDevice parent_obj;

    MemoryRegion mem_ctrl;
    MemoryRegion mem_vram;
    qemu_irq irq;
    QemuConsole *con;

    uint32_t vram_size;
    uint8_t palette[256][3];
    uint32_t timing[G300_TIMING_COUNT];
    uint32_t control;
    uint32_t mask;
    uint32_t tos;
};

static bool g300_update_display(void *opaque)
{
    G300State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint8_t *vram_base = memory_region_get_ram_ptr(&s->mem_vram);
    uint32_t vram_mask = s->vram_size - 1;
    uint32_t vram_offset = s->tos & vram_mask;
    unsigned int (*rgb_to_pixel)(unsigned int, unsigned int, unsigned int);
    int bytes_per_pixel;
    int bpp_index, bits, pixels;
    uint8_t *dst;
    int x, y, i;

    if (surface_width(surface) != G300_WIDTH ||
        surface_height(surface) != G300_HEIGHT) {
        qemu_console_resize(s->con, G300_WIDTH, G300_HEIGHT);
        surface = qemu_console_surface(s->con);
    }

    switch (surface_bits_per_pixel(surface)) {
    case 16:
        rgb_to_pixel = rgb_to_pixel16;
        bytes_per_pixel = 2;
        break;
    case 32:
        rgb_to_pixel = rgb_to_pixel32;
        bytes_per_pixel = 4;
        break;
    default:
        return true;
    }

    bpp_index = (s->control & G300_CONTROL_BPP) >> 17;
    bits = 1 << bpp_index;
    pixels = 8 / bits;

    dst = surface_data(surface);
    /*
     * The MAME fork renders VRAM regardless of the blanking bits.  ARC sets
     * BLANK_FORCE during G300 setup and expects the console to stay visible.
     */
    for (y = 0; y < G300_HEIGHT; y++) {
        uint8_t *row = dst;
        for (x = 0; x < G300_WIDTH; x += pixels) {
            uint8_t data = vram_base[vram_offset++ & vram_mask];
            for (i = 0; i < pixels && (x + i) < G300_WIDTH; i++) {
                int index = data & ((1 << bits) - 1);
                uint32_t color;

                data >>= bits;
                index &= s->mask & 0xff;
                color = rgb_to_pixel(s->palette[index][0],
                                     s->palette[index][1],
                                     s->palette[index][2]);
                memcpy(row + (x + i) * bytes_per_pixel, &color,
                       bytes_per_pixel);
            }
        }
        dst += surface_stride(surface);
    }
    qemu_console_update_full(s->con);

    /* The real card pulses the vblank interrupt once per refresh. */
    qemu_irq_raise(s->irq);
    return true;
}

static void g300_invalidate_display(void *opaque)
{
    G300State *s = opaque;

    memory_region_set_dirty(&s->mem_vram, 0, s->vram_size);
}

static const GraphicHwOps g300_ops = {
    .invalidate = g300_invalidate_display,
    .gfx_update = g300_update_display,
};

static uint64_t g300_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    G300State *s = opaque;
    uint32_t val = 0;

    if (addr < 0x800) {
        int index = addr >> 3;
        val = ((uint32_t)s->palette[index][2] << 0) |
              ((uint32_t)s->palette[index][1] << 8) |
              ((uint32_t)s->palette[index][0] << 16);
        return val;
    }

    if (addr >= (G300_TIMING_FIRST << 3) &&
        addr <= (G300_TIMING_LAST << 3) && addr != G300_TOS) {
        /* Timing registers are stored but the display size is fixed. */
        return s->timing[addr >> 3];
    }

    switch (addr) {
    case G300_TOS:
    case G300_TOS_ALIAS:
        val = s->tos;
        break;
    case G300_MASK:
        val = s->mask;
        break;
    case G300_CONTROL:
        val = s->control;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "g300fb: invalid read at 0x"
                      HWADDR_FMT_plx "\n", addr);
        break;
    }

    return val;
}

static void g300_ctrl_write(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size)
{
    G300State *s = opaque;
    uint32_t val = data;

    if (addr < 0x800) {
        int index = addr >> 3;
        s->palette[index][0] = val & 0xff;
        s->palette[index][1] = (val >> 8) & 0xff;
        s->palette[index][2] = (val >> 16) & 0xff;
        g300_invalidate_display(s);
        return;
    }

    if (addr >= (G300_TIMING_FIRST << 3) &&
        addr <= (G300_TIMING_LAST << 3) && addr != G300_TOS) {
        /* Timing registers are stored but the display size is fixed. */
        s->timing[addr >> 3] = val & 0xffffff;
        qemu_irq_lower(s->irq);
        return;
    }

    switch (addr) {
    case G300_BOOT:
        break;
    case G300_TOS:
    case G300_TOS_ALIAS:
        s->tos = val & 0xffffff;
        g300_invalidate_display(s);
        break;
    case G300_MASK:
        s->mask = val & 0xffffff;
        g300_invalidate_display(s);
        break;
    case G300_CONTROL:
        s->control = val & 0xffffff;
        g300_invalidate_display(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "g300fb: invalid write of 0x%x at 0x"
                      HWADDR_FMT_plx "\n", val, addr);
        break;
    }

    qemu_irq_lower(s->irq);
}

static const MemoryRegionOps g300_ctrl_ops = {
    .read = g300_ctrl_read,
    .write = g300_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g300_reset(DeviceState *dev)
{
    G300State *s = G300FB(dev);

    memset(s->palette, 0, sizeof(s->palette));
    memset(s->timing, 0, sizeof(s->timing));
    s->control = 0;
    s->mask = 0;
    s->tos = 0;
    qemu_irq_lower(s->irq);
}

static const VMStateDescription vmstate_g300fb = {
    .name = "g300fb",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_UNSAFE(palette, G300State, 0, 256 * 3),
        VMSTATE_UINT32_ARRAY(timing, G300State, G300_TIMING_COUNT),
        VMSTATE_UINT32(control, G300State),
        VMSTATE_UINT32(mask, G300State),
        VMSTATE_UINT32(tos, G300State),
        VMSTATE_END_OF_LIST()
    }
};

static void g300_realize(DeviceState *dev, Error **errp)
{
    G300State *s = G300FB(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (s->vram_size < G300_WIDTH * G300_HEIGHT) {
        error_setg(errp, "g300fb: vram_size must be at least %d bytes",
                   G300_WIDTH * G300_HEIGHT);
        return;
    }
    if (s->vram_size & (s->vram_size - 1)) {
        error_setg(errp, "g300fb: vram_size must be a power of two");
        return;
    }

    s->con = qemu_graphic_console_create(dev, 0, &g300_ops, s);

    memory_region_init_io(&s->mem_ctrl, OBJECT(dev), &g300_ctrl_ops, s,
                          "g300fb.ctrl", 0x1000);
    memory_region_init_ram(&s->mem_vram, NULL, "g300fb.vram", s->vram_size,
                           errp);
    if (*errp) {
        return;
    }
    memory_region_set_log(&s->mem_vram, true, DIRTY_MEMORY_VGA);

    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_mmio(sbd, &s->mem_ctrl);
    sysbus_init_mmio(sbd, &s->mem_vram);
}

static const Property g300_properties[] = {
    DEFINE_PROP_UINT32("vram_size", G300State, vram_size, 2 * MiB),
};

static void g300_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = g300_realize;
    device_class_set_legacy_reset(dc, g300_reset);
    dc->vmsd = &vmstate_g300fb;
    device_class_set_props(dc, g300_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->desc = "INMOS G300 framebuffer";
}

static const TypeInfo g300_info = {
    .name = TYPE_G300FB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G300State),
    .class_init = g300_class_init,
};

static void g300_register_types(void)
{
    type_register_static(&g300_info);
}

type_init(g300_register_types)
