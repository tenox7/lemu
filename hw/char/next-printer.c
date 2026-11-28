/*
 * NeXT laser printer interface
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the Previous emulator's printer.c (GPL-2.0-or-later).
 *
 * The NeXT 400 dpi Laser Printer hangs off a serial link like the
 * monitor's: the CPU sends commands with a long word of data, and the
 * printer sends status and asks for page data, which it takes through
 * the printer DMA channel. The printer's own status comes bit by bit
 * through its general purpose inputs and outputs.
 *
 * With connected=on, the printer is there and each printed page is
 * written as a PBM image into the folder given with pages=. Without it the
 * interface is there but the printer stays off.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/char/next-printer.h"
#include "migration/vmstate.h"

#define LP_DMA_OUT_EN   0x80
#define LP_DMA_OUT_REQ  0x40
#define LP_DMA_OUT_UNDR 0x20
#define LP_DMA_IN_EN    0x08
#define LP_DMA_IN_REQ   0x04
#define LP_DMA_IN_OVR   0x02

#define LP_ON           0x80
#define LP_INT          0x08
#define LP_DATA         0x04
#define LP_DATA_OVR     0x02

#define LP_TX_EN        0x02
#define LP_TX_LOOP      0x01

/* Commands from the CPU */
#define LP_CMD_RESET    0xff
#define LP_CMD_DATA_OUT 0xc7
#define LP_CMD_GPI_MASK 0xc5
#define LP_CMD_GPO      0xc4
#define LP_CMD_MARGINS  0xc2
#define LP_CMD_GPI_REQ  0x04
#define LP_CMD_MASK     0xc7
#define LP_CMD_NODATA   0x07
#define LP_CMD_DATA_EN  0x08
#define LP_CMD_300DPI   0x10

/* Commands from the printer */
#define LP_CMD_DATA_IN  0xc7
#define LP_CMD_GPI      0xc4
#define LP_CMD_COPY     0xe7
#define LP_CMD_DATA_REQ 0x07
#define LP_CMD_UNDERRUN 0x0f

#define LP_GPI_PWR_RDY  0x10
#define LP_GPI_RDY      0x08
#define LP_GPI_VSREQ    0x04
#define LP_GPI_BUSY     0x02
#define LP_GPI_STAT_BIT 0x01

#define LP_GPO_VSYNC    0x20
#define LP_GPO_ENABLE   0x10
#define LP_GPO_CLOCK    0x04
#define LP_GPO_CMD_BIT  0x02
#define LP_GPO_BUSY     0x01

#define STAT5_A4        0x02

/* 14 inches at 400 dpi is the longest page */
#define MAX_PAGE_LINES  (14 * 400)

static void lp_check_interrupt(NeXTPrinterState *s)
{
    bool level = (s->csr[1] & (LP_DATA | LP_DATA_OVR)) ||
                 (s->csr[0] & (LP_DMA_OUT_UNDR | LP_DMA_IN_OVR));

    if (level) {
        s->csr[1] |= LP_INT;
    } else {
        s->csr[1] &= ~LP_INT;
    }
    qemu_set_irq(s->irq, level);
}

static void lp_send_data(NeXTPrinterState *s, uint32_t data)
{
    s->data = data;
    if (s->csr[1] & LP_DATA) {
        s->csr[1] |= LP_DATA_OVR;
    } else {
        s->csr[1] |= LP_DATA;
    }
    lp_check_interrupt(s);
}

/* ---- the page ---- */

static void lp_print_start(NeXTPrinterState *s)
{
    g_free(s->page);
    s->page = NULL;
    s->page_width = ((s->margins >> 16) & 0x7F) * 4;
    s->page_size = 0;
    if (s->page_width) {
        s->page = g_malloc0(MAX_PAGE_LINES * s->page_width);
    }
}

static void lp_print_data(NeXTPrinterState *s)
{
    uint32_t room;

    if (!s->page) {
        return;
    }
    room = MAX_PAGE_LINES * s->page_width - s->page_size;
    memcpy(s->page + s->page_size, s->buf, MIN(s->buf_size, room));
    s->page_size += MIN(s->buf_size, room);
}

static void lp_print_finish(NeXTPrinterState *s)
{
    g_autofree char *name = NULL;
    g_autofree char *head = NULL;
    FILE *f;

    if (!s->page) {
        return;
    }
    if (s->pages_dir && s->page_size >= s->page_width) {
        name = g_strdup_printf("%s/next-page-%03u.pbm", s->pages_dir,
                               ++s->page_count);
        f = fopen(name, "wb");
        if (f) {
            /* PBM: 1 is black, as on the printer */
            head = g_strdup_printf("P4\n# %d dpi\n%u %u\n", s->dpi,
                                   s->page_width * 8,
                                   s->page_size / s->page_width);
            fputs(head, f);
            fwrite(s->page, 1, s->page_size - s->page_size % s->page_width,
                   f);
            fclose(f);
        } else {
            qemu_log_mask(LOG_UNIMP, "next-printer: cannot write %s\n", name);
        }
    }
    g_free(s->page);
    s->page = NULL;
    s->page_size = 0;
}

/* ---- printer to CPU ---- */

static void lp_dma_request(NeXTPrinterState *s)
{
    NeXTDMAChan *c = next_dma_chan(s->dma, NEXT_DMA_PRINTER);
    uint32_t n;

    s->csr[0] |= LP_DMA_OUT_REQ;
    if (!next_dma_enabled(s->dma, NEXT_DMA_PRINTER)) {
        return;
    }
    n = MIN(c->limit - c->next, sizeof(s->buf) - s->buf_size);
    if (c->limit > c->next && n) {
        next_dma_read_mem(s->dma, NEXT_DMA_PRINTER, s->buf + s->buf_size, n);
        s->buf_size += n;
    }
    next_dma_update(s->dma, NEXT_DMA_PRINTER);
}

static void lp_command_out(NeXTPrinterState *s, uint8_t command,
                           uint32_t data)
{
    if (!(s->csr[2] & LP_TX_EN)) {
        return;
    }
    switch (command) {
    case LP_CMD_GPI:
    case LP_CMD_COPY:
        s->csr[3] = command;
        lp_send_data(s, data);
        break;
    case LP_CMD_DATA_REQ:
        s->csr[3] = command;
        lp_dma_request(s);
        break;
    case LP_CMD_UNDERRUN:
        s->csr[3] = command;
        s->csr[0] |= LP_DMA_OUT_UNDR;
        lp_check_interrupt(s);
        break;
    }
}

/* ---- the printer's status lines and serial status ---- */

static void lp_interface_status(NeXTPrinterState *s, uint8_t bits, bool set)
{
    uint8_t old = s->stat;

    if (set) {
        s->stat |= bits;
    } else {
        s->stat &= ~bits;
    }
    if ((old ^ s->stat) & s->statmask) {
        lp_command_out(s, LP_CMD_GPI, ~((uint32_t)s->stat) << 24);
    }
}

static uint8_t lp_printer_status(NeXTPrinterState *s, int num)
{
    uint8_t val;

    s->serial_phase = 8;
    lp_interface_status(s, LP_GPI_BUSY, true);
    if (num == 5) {
        val = STAT5_A4;
    } else if (num < 16) {
        val = s->serial_status[num];
    } else {
        val = 0;
    }
    /* odd parity */
    val |= 1;
    for (int i = 1; i < 8; i++) {
        if (val & (1 << i)) {
            val ^= 1;
        }
    }
    return val;
}

static uint8_t lp_printer_command(NeXTPrinterState *s, uint8_t cmd)
{
    switch (cmd) {
    case 0x01:
        return lp_printer_status(s, 0);
    case 0x02:
        return lp_printer_status(s, 1);
    case 0x04:
        return lp_printer_status(s, 2);
    case 0x08:
        return lp_printer_status(s, 4);
    case 0x0b:
        return lp_printer_status(s, 5);
    case 0x1f:
        return lp_printer_status(s, 15);
    default:
        /* commands without status */
        return lp_printer_status(s, 16);
    }
}

static void lp_gpo_access(NeXTPrinterState *s, uint8_t data)
{
    uint8_t changed = data ^ s->gpo_old;

    if (changed & LP_GPO_BUSY) {
        if (data & LP_GPO_BUSY) {
            s->gpo_cmd = 0;
        } else {
            s->gpo_stat = lp_printer_command(s, s->gpo_cmd);
        }
    }
    if (data & LP_GPO_BUSY) {
        if ((changed & LP_GPO_CLOCK) && (data & LP_GPO_CLOCK)) {
            s->gpo_cmd = (s->gpo_cmd << 1) | ((data & LP_GPO_CMD_BIT) ? 1 : 0);
        }
    } else if (s->stat & LP_GPI_BUSY) {
        if ((changed & LP_GPO_CLOCK) && (data & LP_GPO_CLOCK)) {
            s->serial_phase--;
            if (s->serial_phase == 0) {
                lp_interface_status(s, LP_GPI_BUSY, false);
            }
            lp_interface_status(s, LP_GPI_STAT_BIT,
                                s->gpo_stat & (1 << s->serial_phase));
        }
    }
    s->gpo_old = data;
}

static void lp_gpo(NeXTPrinterState *s, uint8_t cmd)
{
    if (cmd & LP_GPO_VSYNC) {
        lp_interface_status(s, LP_GPI_VSREQ, false);
    }
    if (cmd & LP_GPO_ENABLE) {
        lp_interface_status(s, LP_GPI_VSREQ, true);
    }
    lp_gpo_access(s, cmd);
}

/* "COPR. NeXT 1987", the printer's boot message */
static const uint32_t lp_copyright[4] = {
    0x00434f50, 0x522e204e, 0x65585420, 0x31393837
};

static void lp_check_boot_sequence(NeXTPrinterState *s)
{
    if (s->copyright > 0) {
        lp_command_out(s, LP_CMD_COPY, lp_copyright[4 - s->copyright]);
        s->copyright--;
    }
}

static void lp_printer_reset(NeXTPrinterState *s)
{
    memset(s->serial_status, 0, sizeof(s->serial_status));
    s->serial_phase = 0;
    s->copyright = 0;
    s->transfer = false;
    timer_del(s->timer);
    lp_print_finish(s);
}

static void lp_interface_off(NeXTPrinterState *s)
{
    s->copyright = 0;
    memset(s->csr, 0, sizeof(s->csr));
    s->command = 0;
    s->data = 0;
    qemu_set_irq(s->irq, 0);
}

static void lp_power_off(NeXTPrinterState *s)
{
    lp_printer_reset(s);
    s->statmask = 0;
    s->stat = 0;
}

static void lp_io(void *opaque)
{
    NeXTPrinterState *s = opaque;

    if (!s->transfer) {
        return;
    }
    lp_command_out(s, LP_CMD_DATA_REQ, 0);
    if (s->buf_size == 0) {
        lp_command_out(s, LP_CMD_UNDERRUN, 0);
        return;
    }
    lp_print_data(s);
    s->buf_size = 0;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10 * SCALE_MS);
}

static void lp_command_in(NeXTPrinterState *s, uint8_t cmd, uint32_t data)
{
    if (!(s->csr[2] & LP_TX_EN) || !(s->stat & LP_GPI_PWR_RDY)) {
        return;
    }
    switch (cmd) {
    case LP_CMD_RESET:
        if (data == 0xFFFFFFFF) {
            lp_printer_reset(s);
        }
        break;
    case LP_CMD_DATA_OUT:
        if (s->buf_size + 4 <= sizeof(s->buf)) {
            stl_be_p(s->buf + s->buf_size, data);
            s->buf_size += 4;
        }
        break;
    case LP_CMD_GPO:
        lp_gpo(s, (~data) >> 24);
        break;
    case LP_CMD_GPI_MASK:
        s->statmask = (~data) >> 24;
        break;
    case LP_CMD_GPI_REQ:
        lp_command_out(s, LP_CMD_GPI, ~((uint32_t)s->stat) << 24);
        break;
    case LP_CMD_MARGINS:
        s->margins = data;
        break;
    default:
        if ((cmd & LP_CMD_MASK) != LP_CMD_NODATA) {
            break;
        }
        s->dpi = (cmd & LP_CMD_300DPI) ? 300 : 400;
        if (cmd & LP_CMD_DATA_EN) {
            lp_print_start(s);
            s->transfer = true;
            if (s->buf_size) {
                lp_print_data(s);
                s->buf_size = 0;
            }
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      SCALE_MS);
        } else {
            if (s->transfer) {
                lp_print_finish(s);
            }
            s->transfer = false;
        }
        break;
    }
}

/* ---- registers ---- */

static void lp_csr_write(NeXTPrinterState *s, int n, uint8_t val)
{
    uint8_t old;

    switch (n) {
    case 0:
        s->csr[0] &= ~(LP_DMA_OUT_EN | LP_DMA_IN_EN);
        s->csr[0] |= val & (LP_DMA_OUT_EN | LP_DMA_IN_EN);
        if (val & LP_DMA_OUT_UNDR) {
            s->csr[0] &= ~(LP_DMA_OUT_UNDR | LP_DMA_OUT_REQ);
        }
        if (val & LP_DMA_IN_OVR) {
            s->csr[0] &= ~(LP_DMA_IN_OVR | LP_DMA_IN_REQ);
        }
        lp_check_interrupt(s);
        break;
    case 1:
        if (((val ^ s->csr[1]) & LP_ON) && s->connected) {
            if (val & LP_ON) {
                lp_interface_status(s, LP_GPI_PWR_RDY | LP_GPI_RDY, true);
            } else {
                lp_power_off(s);
            }
        }
        s->csr[1] = val;
        if (val & LP_DATA_OVR) {
            s->csr[1] &= ~(LP_DATA_OVR | LP_DATA);
        }
        lp_check_interrupt(s);
        break;
    case 2:
        old = s->csr[2];
        s->csr[2] &= ~(LP_TX_EN | LP_TX_LOOP);
        s->csr[2] |= val & (LP_TX_EN | LP_TX_LOOP);
        if ((val ^ old) & LP_TX_EN) {
            if (val & LP_TX_EN) {
                if (s->stat & LP_GPI_PWR_RDY) {
                    s->copyright = 4;
                    lp_check_boot_sequence(s);
                }
            } else {
                lp_interface_off(s);
            }
        }
        break;
    case 3:
        s->command = val;
        break;
    }
}

static uint64_t next_printer_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTPrinterState *s = opaque;
    unsigned off = addr & 0xf;
    uint64_t val = 0;

    if (off < 4) {
        for (unsigned i = 0; i < size; i++) {
            val = (val << 8) | s->csr[(off + i) & 3];
        }
        return val;
    }
    if (off < 8) {
        val = extract32(s->data, (8 - off - size) * 8, size * 8);
        s->csr[1] &= ~LP_DATA;
        lp_check_boot_sequence(s);
        lp_check_interrupt(s);
    }
    return val;
}

static void next_printer_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    NeXTPrinterState *s = opaque;
    unsigned off = addr & 0xf;

    if (off < 4) {
        for (unsigned i = 0; i < size; i++) {
            lp_csr_write(s, (off + i) & 3, val >> (8 * (size - 1 - i)));
        }
    } else if (off == 4 && size == 4) {
        if (s->csr[2] & LP_TX_LOOP) {
            lp_command_out(s, s->command, val);
        } else {
            lp_command_in(s, s->command, val);
        }
    }
}

static const MemoryRegionOps next_printer_ops = {
    .read = next_printer_read,
    .write = next_printer_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void next_printer_reset_hold(Object *obj, ResetType type)
{
    NeXTPrinterState *s = NEXT_PRINTER(obj);

    lp_interface_off(s);
    lp_power_off(s);
    s->buf_size = 0;
    s->gpo_cmd = s->gpo_stat = s->gpo_old = 0;
}

static void next_printer_realize(DeviceState *dev, Error **errp)
{
    NeXTPrinterState *s = NEXT_PRINTER(dev);

    if (!s->dma) {
        error_setg(errp, "next-printer needs the dma link");
        return;
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lp_io, s);
}

static void next_printer_unrealize(DeviceState *dev)
{
    NeXTPrinterState *s = NEXT_PRINTER(dev);

    lp_print_finish(s);
    timer_free(s->timer);
}

static void next_printer_init(Object *obj)
{
    NeXTPrinterState *s = NEXT_PRINTER(obj);

    memory_region_init_io(&s->mem, obj, &next_printer_ops, s,
                          "next.printer", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription next_printer_vmstate = {
    .name = "next-printer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(csr, NeXTPrinterState, 4),
        VMSTATE_UINT8(command, NeXTPrinterState),
        VMSTATE_UINT32(data, NeXTPrinterState),
        VMSTATE_UINT8(stat, NeXTPrinterState),
        VMSTATE_UINT8(statmask, NeXTPrinterState),
        VMSTATE_UINT32(margins, NeXTPrinterState),
        VMSTATE_UINT8_ARRAY(serial_status, NeXTPrinterState, 16),
        VMSTATE_UINT8(serial_phase, NeXTPrinterState),
        VMSTATE_INT32(copyright, NeXTPrinterState),
        VMSTATE_UINT8(gpo_cmd, NeXTPrinterState),
        VMSTATE_UINT8(gpo_stat, NeXTPrinterState),
        VMSTATE_UINT8(gpo_old, NeXTPrinterState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property next_printer_properties[] = {
    DEFINE_PROP_LINK("dma", NeXTPrinterState, dma, TYPE_NEXT_DMA,
                     NeXTDMAState *),
    DEFINE_PROP_BOOL("connected", NeXTPrinterState, connected, false),
    DEFINE_PROP_STRING("pages", NeXTPrinterState, pages_dir),
};

static void next_printer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT laser printer interface";
    dc->realize = next_printer_realize;
    dc->unrealize = next_printer_unrealize;
    dc->vmsd = &next_printer_vmstate;
    device_class_set_props(dc, next_printer_properties);
    rc->phases.hold = next_printer_reset_hold;
}

static const TypeInfo next_printer_info = {
    .name = TYPE_NEXT_PRINTER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTPrinterState),
    .instance_init = next_printer_init,
    .class_init = next_printer_class_init,
};

static void next_printer_register_types(void)
{
    type_register_static(&next_printer_info);
}

type_init(next_printer_register_types)
