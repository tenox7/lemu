/*
 * NeXT Cube/Station Framebuffer Emulation
 *
 * Copyright (c) 2011 Bryce Lanham
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The screen is 1120x832. The monochrome machines have 2 bits per pixel,
 * 0 being white, in 256 kB of VRAM; the colour machines 16 bits per pixel
 * (4 bits each of red, green, blue and alpha, big-endian) in 2 MB. Lines
 * are 1152 pixels apart, but on the Turbo machines 1120 (Previous's
 * sdlscreen.c).
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
#include "qemu/units.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "framebuffer.h"
#include "ui/pixel_ops.h"
#include "hw/m68k/next-cube.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(NeXTFbState, NEXTFB)

#define NEXTFB_WIDTH    1120
#define NEXTFB_HEIGHT   832

struct NeXTFbState {
    SysBusDevice parent_obj;

    MemoryRegion fb_mr;
    MemoryRegionSection fbsection;
    QemuConsole *con;

    bool color;
    bool turbo;
    uint32_t cols;
    uint32_t rows;
    uint32_t stride;
    int invalidate;
};

static void nextfb_draw_line_mono(void *opaque, uint8_t *d, const uint8_t *s,
                                  int width, int pitch)
{
    static const uint32_t pal[4] = {
        0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000
    };
    uint32_t *buf = (uint32_t *)d;

    for (int i = 0; i < width / 4; i++) {
        uint8_t src = s[i];

        buf[i * 4 + 0] = pal[(src >> 6) & 3];
        buf[i * 4 + 1] = pal[(src >> 4) & 3];
        buf[i * 4 + 2] = pal[(src >> 2) & 3];
        buf[i * 4 + 3] = pal[src & 3];
    }
}

static void nextfb_draw_line_color(void *opaque, uint8_t *d, const uint8_t *s,
                                   int width, int pitch)
{
    uint32_t *buf = (uint32_t *)d;

    for (int i = 0; i < width; i++) {
        uint16_t c = lduw_be_p(s + i * 2);
        uint32_t r = ((c >> 12) & 0xF) * 0x11;
        uint32_t g = ((c >> 8) & 0xF) * 0x11;
        uint32_t b = ((c >> 4) & 0xF) * 0x11;

        buf[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
}

static bool nextfb_update(void *opaque)
{
    NeXTFbState *s = NEXTFB(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0;
    int last = 0;

    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, &s->fb_mr, 0,
                                          s->rows, s->stride);
    }

    framebuffer_update_display(surface, &s->fbsection, s->cols, s->rows,
                               s->stride, s->cols * 4, 0, s->invalidate,
                               s->color ? nextfb_draw_line_color
                                        : nextfb_draw_line_mono,
                               s, &first, &last);
    s->invalidate = 0;

    if (first >= 0) {
        qemu_console_update(s->con, 0, first, s->cols, last - first + 1);
    }
    return true;
}

static void nextfb_invalidate(void *opaque)
{
    NeXTFbState *s = NEXTFB(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps nextfb_ops = {
    .invalidate  = nextfb_invalidate,
    .gfx_update  = nextfb_update,
};

static void nextfb_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    NeXTFbState *s = NEXTFB(dev);
    uint32_t line_pixels = s->turbo ? NEXTFB_WIDTH : 1152;

    s->cols = NEXTFB_WIDTH;
    s->rows = NEXTFB_HEIGHT;
    s->stride = s->color ? line_pixels * 2 : line_pixels / 4;

    memory_region_init_ram(&s->fb_mr, OBJECT(dev), "next-video",
                           s->color ? 2 * MiB : 256 * KiB, errp);
    if (*errp) {
        return;
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->fb_mr);

    s->invalidate = 1;
    s->con = qemu_graphic_console_create(dev, 0, &nextfb_ops, s);
    qemu_console_resize(s->con, s->cols, s->rows);
}

static const Property nextfb_properties[] = {
    DEFINE_PROP_BOOL("color", NeXTFbState, color, false),
    DEFINE_PROP_BOOL("turbo", NeXTFbState, turbo, false),
};

static void nextfb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = nextfb_realize;
    device_class_set_props(dc, nextfb_properties);

    /* The VRAM is RAM and migrates by itself; there is no other state */
}

static const TypeInfo nextfb_info = {
    .name          = TYPE_NEXTFB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTFbState),
    .class_init    = nextfb_class_init,
};

static void nextfb_register_types(void)
{
    type_register_static(&nextfb_info);
}

type_init(nextfb_register_types)
