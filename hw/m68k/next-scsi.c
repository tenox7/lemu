/*
 * NeXT SCSI: NCR53C90(A) with the NeXT DMA control and status registers
 *
 * Copyright (c) 2011 Bryce Lanham
 * Copyright (c) 2024 Mark Cave-Ayland
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The DMA buffering follows the Previous emulator's esp.c and dma.c
 * (GPL-2.0-or-later). The ESP runs in its pseudo-DMA mode: when it raises
 * DRQ, the SCSI DMA channel moves bytes between its FIFO and a 16-byte
 * burst buffer, and between the buffer and memory. So a transfer stops
 * when the channel reaches its limit and goes on when the CPU gives it
 * the next buffer, as on the real machine.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "exec/memattrs.h"
#include "system/memory.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/scsi/esp.h"
#include "hw/dma/next-dma.h"
#include "hw/m68k/next-scsi.h"
#include "migration/vmstate.h"
#include "cpu.h"

#define NEXT_SCSI_ROM_BASE 0x01000000
#define NEXT_SCSI_ROM_SIZE 0x00020000

#define ESPCTRL_CLKMASK     0xc0
#define ESPCTRL_ENABLE_INT  0x20    /* enable ESP interrupt */
#define ESPCTRL_MODE_DMA    0x10    /* 1 = DMA, 0 = PIO */
#define ESPCTRL_DMA_READ    0x08    /* 1 = SCSI to memory */
#define ESPCTRL_FLUSH       0x04    /* flush DMA buffer */
#define ESPCTRL_RESET       0x02    /* hard reset ESP */
#define ESPCTRL_CHIP_TYPE   0x01    /* 1 = WD33C92, 0 = NCR53C90(A) */

#define ESPSTAT_STATE_MASK  0xc0
#define ESPSTAT_STATE_D0S1  0x40
#define ESPSTAT_STATE_D1S0  0xc0


static ESPState *next_scsi_esp(NeXTSCSIState *s)
{
    return &s->esp.esp;
}

/*
 * The real ESP takes a little while to finish a command; QEMU's raises its
 * interrupt inside the register write that started it. NeXT drivers note
 * what they started after that write, so the interrupt reaches the CPU
 * 100 us after it, as in Previous. With much less, NeXTSTEP 3.3 sometimes
 * loses a command while it probes the bus.
 */
#define NEXT_SCSI_IRQ_DELAY_NS 100000

static void next_scsi_update_irq(NeXTSCSIState *s)
{
    qemu_set_irq(s->irq, s->esp_irq_out && (s->control & ESPCTRL_ENABLE_INT));
}

static void next_scsi_irq_timer(void *opaque)
{
    NeXTSCSIState *s = opaque;

    s->esp_irq_out = s->esp_irq;
    next_scsi_update_irq(s);
}

/*
 * The ROM waits for the selection timeout with an instruction loop made
 * for a 25 MHz 68040, which TCG runs through long before 250 ms: it then
 * prints "sc: Didn't complete" for each empty target, again and again, and
 * cannot boot from any disk past one. It gets its timeout sooner.
 */
#define NEXT_SCSI_ROM_SEL_TIMEOUT_NS (10 * SCALE_MS)

static bool next_scsi_cpu_in_rom(void)
{
    uint32_t pc;

    if (!current_cpu) {
        return false;
    }
    pc = cpu_env(current_cpu)->pc;
    /* the ROM, or its copy at 0 that the 68030 ROMs run from */
    return (pc & 0xff000000) == NEXT_SCSI_ROM_BASE || pc < NEXT_SCSI_ROM_SIZE;
}

/*
 * How long the chip takes to raise its interrupt. When no target answers
 * a selection, the chip waits out the timeout the driver set: the select
 * timeout register times 8192 times the clock conversion factor, in
 * clocks of 20 MHz, as in Previous. That is about 250 ms, and the
 * NeXTSTEP driver counts on it while it probes the bus: told at once,
 * it sometimes misses the end of the command and resets the bus.
 */
static int64_t next_scsi_irq_delay(NeXTSCSIState *s)
{
    ESPState *esp = &s->esp.esp;

    /* the chip has already set the interrupt bit in its status */
    if (esp->rregs[ESP_RINTR] == INTR_DC && esp->rregs[ESP_RSEQ] == SEQ_0 &&
        (esp->rregs[ESP_RSTAT] & ~STAT_INT) == 0) {
        int64_t sto = esp->wregs[ESP_WSEL] ? esp->wregs[ESP_WSEL] : 256;
        int64_t ccf = esp->wregs[ESP_WCCF] & 7 ? esp->wregs[ESP_WCCF] & 7 : 8;

        if (next_scsi_cpu_in_rom()) {
            return NEXT_SCSI_ROM_SEL_TIMEOUT_NS;
        }
        return sto * 8192 * ccf * 50;   /* 50 ns a clock at 20 MHz */
    }
    return NEXT_SCSI_IRQ_DELAY_NS;
}

static void next_scsi_esp_irq(void *opaque, int n, int level)
{
    NeXTSCSIState *s = opaque;

    s->esp_irq = level;
    if (level) {
        if (!s->esp_irq_out && !timer_pending(s->irq_timer)) {
            timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      next_scsi_irq_delay(s));
        }
    } else {
        timer_del(s->irq_timer);
        s->esp_irq_out = false;
        next_scsi_update_irq(s);
    }
}

/* Bytes the device side has for memory */
static uint32_t next_scsi_src_avail(NeXTSCSIState *s)
{
    if (s->floppy_select) {
        return s->floppy_ops ? s->floppy_ops->avail(s->floppy_opaque) : 0;
    }
    return s->q_len;
}

static uint8_t next_scsi_src_get(NeXTSCSIState *s)
{
    uint8_t val;

    if (s->floppy_select) {
        return s->floppy_ops->get(s->floppy_opaque);
    }
    val = s->q[s->q_head];
    s->q_head = (s->q_head + 1) % NEXT_SCSI_QUEUE;
    s->q_len--;
    return val;
}

/* Whether the floppy takes a byte from memory */
static bool next_scsi_dst_ready(NeXTSCSIState *s)
{
    return s->floppy_select && s->floppy_ops &&
           s->floppy_ops->room(s->floppy_opaque);
}

static void next_scsi_dst_put(NeXTSCSIState *s, uint8_t val)
{
    s->floppy_ops->put(s->floppy_opaque, val);
}

static void next_scsi_toggle_status(NeXTSCSIState *s)
{
    if ((s->status & ESPSTAT_STATE_MASK) == ESPSTAT_STATE_D0S1) {
        s->status = (s->status & ~ESPSTAT_STATE_MASK) | ESPSTAT_STATE_D1S0;
    } else {
        s->status = (s->status & ~ESPSTAT_STATE_MASK) | ESPSTAT_STATE_D0S1;
    }
}

static uint32_t next_scsi_buf_long(NeXTSCSIState *s, int pos)
{
    return ldl_be_p(&s->buf[pos]);
}

/* Device to memory: Previous's dma_esp_write_memory() */
static void next_scsi_to_memory(NeXTSCSIState *s)
{
    NeXTDMAState *dma = s->dma;
    NeXTDMAChan *c = next_dma_chan(dma, NEXT_DMA_SCSI);

    while (c->next <= c->limit) {
        /* Fill the burst buffer from the ESP FIFO */
        while (s->buf_limit < NEXT_DMA_BURST && next_scsi_src_avail(s)) {
            s->buf[s->buf_limit++] = next_scsi_src_get(s);
            s->buf_size++;
        }
        if (s->buf_limit < NEXT_DMA_BURST) {
            break;
        }
        /* Empty it to memory */
        next_scsi_toggle_status(s);
        while (c->next < c->limit && s->buf_size > 0) {
            uint8_t b[4];

            stl_be_p(b, next_scsi_buf_long(s, NEXT_DMA_BURST - s->buf_size));
            if (!next_dma_write_mem(dma, NEXT_DMA_SCSI, b, 4)) {
                goto out;
            }
            s->buf_size -= 4;
        }
        if (s->buf_size > 0) {
            break;
        }
        s->buf_limit = s->buf_size;
    }
out:
    next_dma_update(dma, NEXT_DMA_SCSI);
}

/* Memory to device: Previous's dma_esp_read_memory() */
static void next_scsi_from_memory(NeXTSCSIState *s)
{
    NeXTDMAState *dma = s->dma;
    NeXTDMAChan *c = next_dma_chan(dma, NEXT_DMA_SCSI);

    while (c->next < c->limit) {
        if (s->buf_limit < NEXT_DMA_BURST) {
            while (c->next < c->limit && s->buf_limit < NEXT_DMA_BURST) {
                if (!next_dma_read_mem(dma, NEXT_DMA_SCSI,
                                       &s->buf[s->buf_limit], 4)) {
                    goto out;
                }
                s->buf_limit += 4;
                s->buf_size += 4;
            }
        }
        if (s->buf_limit < NEXT_DMA_BURST) {
            break;
        }
        next_scsi_toggle_status(s);
        while (s->buf_size > 0 && next_scsi_dst_ready(s)) {
            next_scsi_dst_put(s, s->buf[s->buf_limit - s->buf_size]);
            s->buf_size--;
        }
        if (s->buf_size > 0) {
            break;
        }
        s->buf_limit = s->buf_size;
    }
out:
    next_dma_update(dma, NEXT_DMA_SCSI);
}

/* The ESP starts a DMA command only while the channel runs */
static void next_scsi_sync_esp_dma(NeXTSCSIState *s)
{
    bool en = !s->floppy_select && next_dma_enabled(s->dma, NEXT_DMA_SCSI);

    if (en != s->esp_dma) {
        s->esp_dma = en;
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->esp), 1), en);
    }
}

static void next_scsi_pump(NeXTSCSIState *s)
{
    NeXTDMAState *dma = s->dma;

    if (s->in_pump) {
        s->pump_again = true;
        return;
    }
    s->in_pump = true;
    do {
        s->pump_again = false;
        if (!next_dma_enabled(dma, NEXT_DMA_SCSI)) {
            break;
        }
        if (next_dma_dev2mem(dma, NEXT_DMA_SCSI)) {
            if (next_scsi_src_avail(s) || s->buf_size) {
                next_scsi_to_memory(s);
            }
        } else if (s->floppy_select &&
                   (next_scsi_dst_ready(s) || s->buf_size)) {
            next_scsi_from_memory(s);
        }
    } while (s->pump_again);
    s->in_pump = false;
    next_scsi_sync_esp_dma(s);
}

/*
 * The ESP moves a DMA transfer here at once. Bytes for memory wait in the
 * queue for the channel; bytes for the drive come from memory now.
 */
static void next_scsi_esp_dma_write(void *opaque, uint8_t *buf, int len)
{
    NeXTSCSIState *s = opaque;

    for (int i = 0; i < len; i++) {
        if (s->q_len == NEXT_SCSI_QUEUE) {
            qemu_log_mask(LOG_GUEST_ERROR, "next-scsi: DMA queue full\n");
            break;
        }
        s->q[(s->q_head + s->q_len) % NEXT_SCSI_QUEUE] = buf[i];
        s->q_len++;
    }
    next_scsi_pump(s);
}

static void next_scsi_esp_dma_read(void *opaque, uint8_t *buf, int len)
{
    NeXTSCSIState *s = opaque;
    NeXTDMAState *dma = s->dma;
    NeXTDMAChan *c = next_dma_chan(dma, NEXT_DMA_SCSI);
    int i = 0;

    while (i < len) {
        if (s->buf_size > 0) {
            buf[i++] = s->buf[s->buf_limit - s->buf_size];
            s->buf_size--;
            continue;
        }
        s->buf_limit = 0;
        if (!next_dma_enabled(dma, NEXT_DMA_SCSI) || c->next >= c->limit) {
            break;
        }
        /* a burst, or what is left before the limit */
        {
            uint32_t n = MIN(NEXT_DMA_BURST, c->limit - c->next);

            if (!next_dma_read_mem(dma, NEXT_DMA_SCSI, s->buf, n)) {
                break;
            }
            s->buf_limit = s->buf_size = n;
            next_scsi_toggle_status(s);
        }
        if (c->next == c->limit) {
            next_dma_update(dma, NEXT_DMA_SCSI);
        }
    }
    if (i < len) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "next-scsi: %d bytes for the drive past the DMA limit\n",
                      len - i);
        memset(buf + i, 0, len - i);
    }
    next_scsi_sync_esp_dma(s);
}

/*
 * The bytes the channel can give the drive now: the NeXT driver sets up
 * a long write one buffer at a time, from its DMA interrupt.
 */
static uint32_t next_scsi_esp_dma_avail(void *opaque)
{
    NeXTSCSIState *s = opaque;
    NeXTDMAState *dma = s->dma;
    NeXTDMAChan *c = next_dma_chan(dma, NEXT_DMA_SCSI);
    uint32_t n = s->buf_size;

    if (!s->floppy_select && next_dma_enabled(dma, NEXT_DMA_SCSI) &&
        !next_dma_dev2mem(dma, NEXT_DMA_SCSI) && c->limit > c->next) {
        n += c->limit - c->next;
    }
    return n;
}

void next_scsi_set_floppy(NeXTSCSIState *s, const NeXTSCSIFloppyOps *ops,
                          void *opaque)
{
    s->floppy_ops = ops;
    s->floppy_opaque = opaque;
}

void next_scsi_select_floppy(NeXTSCSIState *s, bool floppy)
{
    s->floppy_select = floppy;
    next_scsi_sync_esp_dma(s);
}

void next_scsi_dma_run(NeXTSCSIState *s)
{
    next_scsi_pump(s);
}

static void next_scsi_dma_kick(void *opaque, NeXTDMAChannel ch)
{
    next_scsi_pump(opaque);
}

static void next_scsi_dma_initbuf(void *opaque, NeXTDMAChannel ch,
                                  unsigned offset)
{
    NeXTSCSIState *s = opaque;

    s->status = 0;
    s->buf_size = 0;
    /*
     * The buffer moves long words: a start that is not on one would let
     * a long word run past its end
     */
    s->buf_limit = offset & (NEXT_DMA_BURST - 4);
}

/* Previous's dma_esp_flush_buffer(): writes one long word */
static void next_scsi_flush(NeXTSCSIState *s)
{
    NeXTDMAState *dma = s->dma;
    NeXTDMAChan *c = next_dma_chan(dma, NEXT_DMA_SCSI);
    uint8_t b[4];

    if (!next_dma_enabled(dma, NEXT_DMA_SCSI) ||
        !next_dma_dev2mem(dma, NEXT_DMA_SCSI)) {
        return;
    }
    if (dma->turbo) {
        for (int i = 0; i < NEXT_DMA_BURST && c->next < c->limit; i++) {
            uint8_t v = 0;

            if (s->buf_size > 0) {
                v = s->buf[s->buf_limit - s->buf_size];
                s->buf_size--;
            }
            if (!next_dma_write_mem(dma, NEXT_DMA_SCSI, &v, 1)) {
                break;
            }
        }
        if (s->buf_size == 0) {
            s->buf_limit = 0;
        }
    } else if (c->next < c->limit && s->buf_limit - s->buf_size <= NEXT_DMA_BURST - 4) {
        while (s->buf_size < 4) {
            s->buf[s->buf_limit++] = 0;
            s->buf_size++;
        }
        stl_be_p(b, next_scsi_buf_long(s, s->buf_limit - s->buf_size));
        if (next_dma_write_mem(dma, NEXT_DMA_SCSI, b, 4)) {
            s->buf_size -= 4;
            if (s->buf_size == 0) {
                s->buf_limit = 0;
            }
        }
    }
    next_dma_update(dma, NEXT_DMA_SCSI);
}

static uint64_t next_scsi_csr_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTSCSIState *s = opaque;

    return addr ? s->status : s->control;
}

static void next_scsi_csr_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    NeXTSCSIState *s = opaque;

    if (addr) {
        s->status = val;
        return;
    }
    s->control = val;
    if (val & ESPCTRL_FLUSH) {
        next_scsi_flush(s);
    }
    if (val & ESPCTRL_RESET) {
        /*
         * The ESP's reset clears its status without lowering its
         * interrupt: drop the one it had, or it stays up for good.
         */
        device_cold_reset(DEVICE(&s->esp));
        timer_del(s->irq_timer);
        s->esp_irq = false;
        s->esp_irq_out = false;
    }
    next_scsi_update_irq(s);
}


/* Bits 4, 6, 7 of the address are not decoded: 0x20 is also 0x30 */
static MemTxResult next_scsi_read(void *opaque, hwaddr addr, uint64_t *data,
                                  unsigned size, MemTxAttrs attrs)
{
    NeXTSCSIState *s = opaque;

    addr &= 0x2f;
    if (addr == 0x0b && !s->is_53c90a) {
        /* the 53C90 has no configuration register 2 */
        *data = 0;
    } else if (addr < 0x10) {
        *data = esp_reg_read(next_scsi_esp(s), addr);
    } else if (addr == 0x20 || addr == 0x21) {
        *data = next_scsi_csr_read(s, addr & 1, size);
    } else {
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult next_scsi_write(void *opaque, hwaddr addr, uint64_t data,
                                   unsigned size, MemTxAttrs attrs)
{
    NeXTSCSIState *s = opaque;

    addr &= 0x2f;
    if (addr == 0x0b && !s->is_53c90a) {
        /* nothing there on the 53C90 */
    } else if (addr < 0x10) {
        esp_reg_write(next_scsi_esp(s), addr, data);
        /* A command may have started a transfer */
        next_scsi_pump(s);
    } else if (addr == 0x20 || addr == 0x21) {
        next_scsi_csr_write(s, addr & 1, data, size);
    } else {
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps next_scsi_ops = {
    .read_with_attrs = next_scsi_read,
    .write_with_attrs = next_scsi_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void next_scsi_init(Object *obj)
{
    NeXTSCSIState *s = NEXT_SCSI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    object_initialize_child(obj, "esp", &s->esp, TYPE_SYSBUS_ESP);

    memory_region_init_io(&s->mem, obj, &next_scsi_ops, s, "next.scsi",
                          0x100);
    sysbus_init_mmio(sbd, &s->mem);
    sysbus_init_irq(sbd, &s->irq);

    qdev_init_gpio_in_named(DEVICE(obj), next_scsi_esp_irq, "esp-irq", 1);
}

static void next_scsi_realize(DeviceState *dev, Error **errp)
{
    NeXTSCSIState *s = NEXT_SCSI(dev);
    SysBusESPState *sysbus_esp = SYSBUS_ESP(&s->esp);
    ESPState *esp = &sysbus_esp->esp;

    if (!s->dma) {
        error_setg(errp, "next-scsi needs the dma link");
        return;
    }

    esp->dma_memory_read = next_scsi_esp_dma_read;
    esp->dma_memory_write = next_scsi_esp_dma_write;
    esp->dma_read_avail = next_scsi_esp_dma_avail;
    esp->dma_opaque = s;
    esp->dma_enabled = 0;
    sysbus_esp->it_shift = 0;
    if (!sysbus_realize(SYS_BUS_DEVICE(sysbus_esp), errp)) {
        return;
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(sysbus_esp), 0,
                       qdev_get_gpio_in_named(dev, "esp-irq", 0));

    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, next_scsi_irq_timer, s);
    next_dma_set_client(s->dma, NEXT_DMA_SCSI, next_scsi_dma_kick,
                        next_scsi_dma_initbuf, s);

    scsi_bus_legacy_handle_cmdline(&esp->bus);
}

static void next_scsi_reset_hold(Object *obj, ResetType type)
{
    NeXTSCSIState *s = NEXT_SCSI(obj);

    s->control = 0;
    s->status = 0;
    s->buf_size = 0;
    s->buf_limit = 0;
    s->q_head = s->q_len = 0;
    s->esp_dma = false;
    s->esp_irq_out = false;
    if (s->irq_timer) {
        timer_del(s->irq_timer);
    }
}

static int next_scsi_post_load(void *opaque, int version_id)
{
    NeXTSCSIState *s = opaque;

    if (s->buf_limit < 0 || s->buf_limit > NEXT_DMA_BURST ||
        s->buf_size < 0 || s->buf_size > s->buf_limit ||
        s->q_head >= NEXT_SCSI_QUEUE || s->q_len > NEXT_SCSI_QUEUE) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription next_scsi_vmstate = {
    .name = "next-scsi",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = next_scsi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(control, NeXTSCSIState),
        VMSTATE_UINT8(status, NeXTSCSIState),
        VMSTATE_BOOL(esp_irq, NeXTSCSIState),
        VMSTATE_BOOL(esp_dma, NeXTSCSIState),
        VMSTATE_BOOL(esp_irq_out, NeXTSCSIState),
        VMSTATE_TIMER_PTR(irq_timer, NeXTSCSIState),
        VMSTATE_UINT32(q_head, NeXTSCSIState),
        VMSTATE_UINT32(q_len, NeXTSCSIState),
        VMSTATE_UINT8_ARRAY(q, NeXTSCSIState, NEXT_SCSI_QUEUE),
        VMSTATE_INT32(buf_size, NeXTSCSIState),
        VMSTATE_INT32(buf_limit, NeXTSCSIState),
        VMSTATE_UINT8_ARRAY(buf, NeXTSCSIState, NEXT_DMA_BURST),
        VMSTATE_BOOL(floppy_select, NeXTSCSIState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property next_scsi_properties[] = {
    DEFINE_PROP_LINK("dma", NeXTSCSIState, dma, TYPE_NEXT_DMA,
                     NeXTDMAState *),
    /* the 68040 boards have a 53C90A, the 68030 board a 53C90 */
    DEFINE_PROP_BOOL("53c90a", NeXTSCSIState, is_53c90a, true),
};

static void next_scsi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT SCSI controller";
    dc->realize = next_scsi_realize;
    dc->vmsd = &next_scsi_vmstate;
    device_class_set_props(dc, next_scsi_properties);
    rc->phases.hold = next_scsi_reset_hold;
}

static const TypeInfo next_scsi_info = {
    .name = TYPE_NEXT_SCSI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = next_scsi_init,
    .instance_size = sizeof(NeXTSCSIState),
    .class_init = next_scsi_class_init,
};

static void next_scsi_register_types(void)
{
    type_register_static(&next_scsi_info);
}

type_init(next_scsi_register_types)
