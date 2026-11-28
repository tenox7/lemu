/*
 * NeXT 68k computers: NeXT Computer, NeXTcube, NeXTstation, NeXTstation
 * Color and their Turbo versions
 *
 * Copyright (c) 2011 Bryce Lanham
 * Copyright (c) 2024 Mark Cave-Ayland
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The board follows the Previous emulator (GPL-2.0-or-later): its sysReg.c
 * (system control registers, interrupts, hardclock, event counter),
 * bmap.c, cpu/memory.c (address map, memory banks and memory write
 * functions) and ioMemTabNEXT.c (the device space).
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/datadir.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "exec/cpu-interrupt.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "exec/cpu-common.h"
#include "system/physmem.h"
#include "system/memory.h"
#include "system/system.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "hw/core/irq.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/escc.h"
#include "hw/dma/next-dma.h"
#include "hw/m68k/next-cube.h"
#include "hw/m68k/next-scsi.h"
#include "hw/net/mb8795.h"
#include "hw/input/next-kms.h"
#include "hw/block/next-mo.h"
#include "hw/block/next-fdc.h"
#include "hw/audio/next-dsp.h"
#include "hw/audio/next-sound.h"
#include "hw/display/bt463.h"
#include "hw/m68k/next-tmc.h"
#include "hw/m68k/next-ncc.h"
#include "hw/core/or-irq.h"
#include "hw/char/next-printer.h"
#include "hw/rtc/next-rtc.h"
#include "net/net.h"
#include "net/filter-next.h"
#include "qapi/error.h"
#include "qom/object_interfaces.h"
#include "qom/object.h"
#include "target/m68k/cpu.h"
#include "migration/vmstate.h"
#include "ui/console.h"

/* ------------------------------------------------------------------ */
/* System control: SCR1, SCR2, interrupts, timers                      */
/* ------------------------------------------------------------------ */

#define TYPE_NEXT_PC "next-pc"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTPC, NEXT_PC)

struct NeXTPC {
    SysBusDevice parent_obj;

    M68kCPU *cpu;
    NeXTRTC *rtc;
    NeXTDMAState *dma;
    MemoryRegion *escc_mr;
    MemoryRegion *ramdac_mr;    /* colour machines */
    MB8795State *enet;
    NeXTDSPState *dsp;

    MemoryRegion int_mem;
    MemoryRegion scr1_mem;
    MemoryRegion scr2_mem;
    MemoryRegion bright_mem;
    MemoryRegion hardclock_mem;
    MemoryRegion eventc_mem;
    MemoryRegion scc_mem;
    MemoryRegion bmap_mem;

    /* configuration */
    uint8_t model;
    bool turbo;
    bool color;
    bool new_rtc;

    uint32_t scr1;
    uint8_t scr2[4];
    uint32_t int_status;
    uint32_t int_mask;
    int32_t int_level;

    uint8_t bright;
    uint8_t col_vid;
    uint32_t scc_clock;
    uint32_t scc_irq;

    uint8_t hardclock_csr;
    uint16_t hardclock_counter;
    uint16_t hardclock_latch;
    int64_t hardclock_last;
    QEMUTimer *hardclock_timer;

    int64_t eventc_offset;
    uint32_t eventc_latch;

    QEMUTimer *vbl_timer;
    qemu_irq vbl_out;       /* to the TMC on the Turbo machines */

    uint32_t bmap[16];

    /* the DSP's interrupt lines */
    bool dsp_hreq;
    bool dsp_txd;
};

/* SCR1 fields (non-Turbo) */
#define SCR1_DMA_REVISION   1
#define SCR1_CPU_25MHZ      2
#define SCR1_BOARD_REV1     1

/* SCR2 byte 0 */
#define SCR2_DSP_RESET      0x80    /* 0 holds the DSP in reset */
#define SCR2_DSP_BLK_END    0x40    /* DSP IRQB at the end of a DMA block */
#define SCR2_DSP_UNPKD      0x20    /* unpacked DSP DMA */
#define SCR2_DSP_MODE_B     0x10
#define SCR2_DSP_MODE_A     0x08
#define SCR2_SOFTINT2       0x02
#define SCR2_SOFTINT1       0x01
/* SCR2 byte 1 */
#define SCR2_DSP_TXD_EN     0x80    /* Turbo */
/* SCR2 byte 3 */
#define SCR2_DSP_INT_EN     0x40    /* not Turbo: DSP at level 4, not 3 */
#define SCR2_DSP_MEM_EN     0x20    /* DSP memory, inverted but on Turbo */
/* SCR2 byte 2 */
#define SCR2_TIMERIPL7      0x80
#define SCR2_RTDATA         0x04
#define SCR2_RTCLK          0x02
#define SCR2_RTCE           0x01

#define INT_NONMASKABLE     0x80027640
#define INT_ZEROBITS        0xC22E7600 /* Turbo */

#define HARDCLOCK_ENABLE    0x80
#define HARDCLOCK_LATCH     0x40

#define VID_CMD_CLEAR_INT   0x01
#define VID_CMD_ENABLE_INT  0x02
#define VID_CMD_UNBLANK     0x04

#define NEXT_VBL_FREQ       68

/* BMAP registers */
#define BMAP_RCNTL          0x1
#define BMAP_BURWREN        0x3
#define BMAP_DSP_HREQ       0x20000000
#define BMAP_DSP_TXD        0x10000000
#define BMAP_DDIR           0xC
#define BMAP_DRW            0xD
#define BMAP_LO             0x40000000
#define BMAP_RESET          0x40000000
#define BMAP_HEARTBEAT      0x20000000
#define BMAP_TPE_ILBC       0x10000000
#define BMAP_TPE_RXSEL      0x80000000
#define BMAP_TPE            (BMAP_TPE_RXSEL | BMAP_TPE_ILBC)

static void next_pc_update_irq(NeXTPC *s)
{
    uint32_t pending = s->int_status & s->int_mask;
    int level;

    if (!pending) {
        level = 0;
    } else if (pending & NEXT_INT_L7_MASK) {
        level = 7;
    } else if ((pending & BIT(NEXT_INT_TIMER)) &&
               (s->scr2[2] & SCR2_TIMERIPL7)) {
        level = 7;
    } else if (pending & NEXT_INT_L6_MASK) {
        level = 6;
    } else if (pending & NEXT_INT_L5_MASK) {
        level = 5;
    } else if (pending & NEXT_INT_L4_MASK) {
        level = 4;
    } else if (pending & NEXT_INT_L3_MASK) {
        level = 3;
    } else if (pending & NEXT_INT_L2_MASK) {
        level = 2;
    } else {
        level = 1;
    }

    if (level != s->int_level) {
        s->int_level = level;
        if (s->cpu) {
            /* all NeXT interrupts are autovectored */
            m68k_set_irq_level(s->cpu, level, 24 + level);
        }
    }
}

static void next_pc_set_int(NeXTPC *s, int n, bool level)
{
    if (level) {
        s->int_status |= BIT(n);
    } else {
        s->int_status &= ~BIT(n);
    }
    next_pc_update_irq(s);
}

static void next_pc_irq(void *opaque, int n, int level)
{
    next_pc_set_int(opaque, n, level);
}

/* both SCC channels share one interrupt */
static void next_pc_scc_irq(void *opaque, int n, int level)
{
    NeXTPC *s = opaque;

    s->scc_irq = deposit32(s->scc_irq, n, 1, level);
    next_pc_set_int(s, NEXT_INT_SCC, s->scc_irq != 0);
}

/*
 * The bits of a 32-bit register that a byte or word access at addr
 * reaches: QEMU does not align such accesses for the device.
 */
static int next_reg_shift(hwaddr addr, unsigned size)
{
    return (4 - (addr & 3) - size) * 8;
}

/* Interrupt status and mask: 0x02007000, 0x02007800 */
static uint64_t next_int_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTPC *s = opaque;
    uint32_t val;

    if (addr & 0x800) {
        val = s->turbo ? s->int_mask & ~INT_ZEROBITS : s->int_mask;
    } else {
        val = s->int_status;
    }
    return extract32(val, next_reg_shift(addr, size), size * 8);
}

static void next_int_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    NeXTPC *s = opaque;
    uint32_t mask;

    if (!(addr & 0x800)) {
        /* the status register is read only */
        return;
    }
    mask = deposit32(s->int_mask, next_reg_shift(addr, size), size * 8, val);
    s->int_mask = mask | (s->turbo ? INT_ZEROBITS : INT_NONMASKABLE);
    next_pc_update_irq(s);
}

static const MemoryRegionOps next_int_ops = {
    .read = next_int_read,
    .write = next_int_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* The guest asked for a reset through the BMAP: keep the CPU registers */
static bool next_warm_reset;

/* SCR1: 0x0200c000, also 0x0200c800 */
static uint64_t next_scr1_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTPC *s = opaque;

    return extract32(s->scr1, next_reg_shift(addr, size), size * 8);
}

static void next_scr1_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
}

static const MemoryRegionOps next_scr1_ops = {
    .read = next_scr1_read,
    .write = next_scr1_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* The DSP's interrupt, as SCR2 and the BMAP route it */
static void next_pc_check_dsp(NeXTPC *s)
{
    bool l3 = false, l4 = false;

    if (s->turbo) {
        l4 = s->dsp_hreq || ((s->scr2[1] & SCR2_DSP_TXD_EN) && s->dsp_txd);
    } else if (s->scr2[3] & SCR2_DSP_INT_EN) {
        if (s->model == NEXT_MODEL_COMPUTER) {
            l4 = s->dsp_hreq || s->dsp_txd;
        } else {
            l4 = (s->dsp_hreq && (s->bmap[BMAP_BURWREN] & BMAP_DSP_HREQ)) ||
                 (s->dsp_txd && (s->bmap[BMAP_BURWREN] & BMAP_DSP_TXD));
        }
    } else {
        l3 = s->dsp_hreq;   /* the diagnostics expect this */
    }
    next_pc_set_int(s, NEXT_INT_DSP_L3, l3);
    next_pc_set_int(s, NEXT_INT_DSP_L4, l4);
}

static void next_pc_dsp_irq(void *opaque, int n, int level)
{
    NeXTPC *s = opaque;

    if (n == NEXT_DSP_HREQ) {
        s->dsp_hreq = level;
    } else {
        s->dsp_txd = level;
    }
    next_pc_check_dsp(s);
}

/*
 * Only the first 68030 board's DSP memory comes and goes with the DSP's
 * reset; the others have a bit in SCR2 for it.
 */
static bool next_pc_dsp_memreset(NeXTPC *s)
{
    return s->turbo || s->model != NEXT_MODEL_COMPUTER;
}

/* SCR2: 0x0200d000, four byte registers */
static uint8_t next_scr2_read_byte(NeXTPC *s, int n)
{
    if (n == 2) {
        if (next_rtc_read_bit(s->rtc)) {
            s->scr2[2] |= SCR2_RTDATA;
        } else {
            s->scr2[2] &= ~SCR2_RTDATA;
        }
    }
    return s->scr2[n];
}

static void next_scr2_write_byte(NeXTPC *s, int n, uint8_t val)
{
    uint8_t changed = s->scr2[n] ^ val;

    s->scr2[n] = val;

    switch (n) {
    case 0:
        if (changed & SCR2_SOFTINT1) {
            next_pc_set_int(s, NEXT_INT_SOFT1, val & SCR2_SOFTINT1);
        }
        if (changed & SCR2_SOFTINT2) {
            next_pc_set_int(s, NEXT_INT_SOFT2, val & SCR2_SOFTINT2);
        }
        if (s->dsp && (changed & SCR2_DSP_RESET)) {
            /* the mode pins are low active */
            next_dsp_set_running(s->dsp, val & SCR2_DSP_RESET,
                                 (~val >> 3) & 3);
            if (!next_pc_dsp_memreset(s)) {
                next_dsp_set_memory(s->dsp, val & SCR2_DSP_RESET);
            }
        }
        if (s->dsp && (changed & (SCR2_DSP_BLK_END | SCR2_DSP_UNPKD))) {
            next_dsp_set_dma_options(s->dsp, val & SCR2_DSP_BLK_END,
                                     val & SCR2_DSP_UNPKD);
        }
        break;
    case 1:
        if (s->turbo && (changed & SCR2_DSP_TXD_EN)) {
            next_pc_check_dsp(s);
        }
        break;
    case 3:
        if (!s->turbo && (changed & SCR2_DSP_INT_EN)) {
            next_pc_check_dsp(s);
        }
        if (s->dsp && (changed & SCR2_DSP_MEM_EN) &&
            next_pc_dsp_memreset(s)) {
            next_dsp_set_memory(s->dsp, s->turbo ? val & SCR2_DSP_MEM_EN
                                                 : !(val & SCR2_DSP_MEM_EN));
        }
        break;
    case 2:
        if (val & SCR2_RTCE) {
            if (changed & SCR2_RTCE) {
                next_rtc_start(s->rtc);
            }
            if ((changed & SCR2_RTCLK) && !(val & SCR2_RTCLK)) {
                next_rtc_write_bit(s->rtc, val & SCR2_RTDATA);
            }
        } else if (changed & SCR2_RTCE) {
            next_rtc_reset_interface(s->rtc);
        }
        if (changed & SCR2_TIMERIPL7) {
            next_pc_update_irq(s);
        }
        break;
    }
}

static uint64_t next_scr2_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTPC *s = opaque;
    uint64_t val = 0;

    addr &= 3;
    for (unsigned i = 0; i < size; i++) {
        val = (val << 8) | next_scr2_read_byte(s, (addr + i) & 3);
    }
    return val;
}

static void next_scr2_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    NeXTPC *s = opaque;

    addr &= 3;
    for (unsigned i = 0; i < size; i++) {
        next_scr2_write_byte(s, (addr + i) & 3,
                             val >> (8 * (size - 1 - i)));
    }
}

static const MemoryRegionOps next_scr2_ops = {
    .read = next_scr2_read,
    .write = next_scr2_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* Brightness: 0x02010000 */
static uint64_t next_bright_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTPC *s = opaque;

    return s->bright;
}

static void next_bright_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    NeXTPC *s = opaque;

    /* A long write to the byte port puts the first byte there */
    s->bright = val >> (8 * (size - 1));
}

static const MemoryRegionOps next_bright_ops = {
    .read = next_bright_read,
    .write = next_bright_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* Hardclock: 0x02016000 counter, 0x02016004 CSR */
static int64_t next_us(void)
{
    return qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
}

static void next_hardclock_tick(void *opaque)
{
    NeXTPC *s = opaque;
    int64_t now = next_us();

    next_pc_set_int(s, NEXT_INT_TIMER, true);
    s->hardclock_counter = s->hardclock_latch;
    if (s->hardclock_counter) {
        s->hardclock_last = now;
        timer_mod(s->hardclock_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)s->hardclock_counter * SCALE_US);
    }
}

static uint16_t next_hardclock_now(NeXTPC *s)
{
    if ((s->hardclock_csr & HARDCLOCK_ENABLE) && s->hardclock_counter) {
        int64_t diff = next_us() - s->hardclock_last;

        return diff > s->hardclock_counter ? 0 : s->hardclock_counter - diff;
    }
    return s->hardclock_counter;
}

static MemTxResult next_hardclock_read(void *opaque, hwaddr addr,
                                       uint64_t *data, unsigned size,
                                       MemTxAttrs attrs)
{
    NeXTPC *s = opaque;

    switch (addr & 7) {
    case 0:
        *data = next_hardclock_now(s) >> 8;
        break;
    case 1:
        *data = next_hardclock_now(s) & 0xff;
        break;
    case 4:
        *data = s->hardclock_csr;
        next_pc_set_int(s, NEXT_INT_TIMER, false);
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult next_hardclock_write(void *opaque, hwaddr addr,
                                        uint64_t val, unsigned size,
                                        MemTxAttrs attrs)
{
    NeXTPC *s = opaque;
    uint8_t changed;

    switch (addr & 7) {
    case 0:
        s->hardclock_latch = (s->hardclock_latch & 0x00ff) | (val << 8);
        break;
    case 1:
        s->hardclock_latch = (s->hardclock_latch & 0xff00) | (val & 0xff);
        break;
    case 4:
        changed = s->hardclock_csr ^ val;
        s->hardclock_csr = val;
        if (val & HARDCLOCK_LATCH) {
            s->hardclock_counter = s->hardclock_latch;
        }
        if ((val & HARDCLOCK_ENABLE) &&
            ((val & HARDCLOCK_LATCH) || (changed & HARDCLOCK_ENABLE))) {
            if (s->hardclock_counter) {
                s->hardclock_last = next_us();
                timer_mod(s->hardclock_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          (int64_t)s->hardclock_counter * SCALE_US);
            }
        } else if (!(val & HARDCLOCK_ENABLE) &&
                   (changed & HARDCLOCK_ENABLE)) {
            int64_t diff = next_us() - s->hardclock_last;

            s->hardclock_counter = diff > s->hardclock_counter ?
                                   0 : s->hardclock_counter - diff;
            timer_del(s->hardclock_timer);
        }
        s->hardclock_csr &= ~HARDCLOCK_LATCH;
        next_pc_set_int(s, NEXT_INT_TIMER, false);
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps next_hardclock_ops = {
    .read_with_attrs = next_hardclock_read,
    .write_with_attrs = next_hardclock_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Event counter: 0x0201a000, 20 bits of microseconds. Reading the first
 * byte latches the count, and the other bytes read the latched value, so
 * that software can read it a byte at a time.
 */
static uint64_t next_eventc_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTPC *s = opaque;

    if ((addr & 3) == 0) {
        s->eventc_latch = (next_us() - s->eventc_offset) & 0xFFFFF;
    }
    return extract32(s->eventc_latch, (4 - (addr & 3) - size) * 8, size * 8);
}

static void next_eventc_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    NeXTPC *s = opaque;

    /* a write to the first byte resets the counter */
    if ((addr & 3) == 0) {
        s->eventc_offset = next_us();
    }
}

static const MemoryRegionOps next_eventc_ops = {
    .read = next_eventc_read,
    .write = next_eventc_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * 0x02018000: SCC (0-3), serial clock (4-7), RAMDAC (0x100-0x103) and
 * colour video control (0x180, 0x190, 0x198). Address bits 9-12 are not
 * decoded.
 */
static MemTxResult next_scc_area_read(void *opaque, hwaddr addr,
                                      uint64_t *data, unsigned size,
                                      MemTxAttrs attrs)
{
    NeXTPC *s = opaque;
    unsigned off = addr & 0x1ff;

    if (off < 4) {
        uint64_t val = 0;

        for (unsigned i = 0; i < size; i++) {
            uint64_t b;

            memory_region_dispatch_read(s->escc_mr, (off + i) & 3, &b, MO_8,
                                        attrs);
            val = (val << 8) | (b & 0xff);
        }
        *data = val;
    } else if (off < 8) {
        *data = extract32(s->scc_clock, (8 - off - size) * 8, size * 8);
    } else if (off >= 0x100 && off < 0x104) {
        uint64_t val = 0;

        for (unsigned i = 0; i < size && s->ramdac_mr; i++) {
            uint64_t b;

            memory_region_dispatch_read(s->ramdac_mr, (off + i) & 3, &b,
                                        MO_8, attrs);
            val = (val << 8) | (b & 0xff);
        }
        *data = val;
    } else if (off == 0x180 || off == 0x190 || off == 0x198) {
        *data = 0;
    } else {
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult next_scc_area_write(void *opaque, hwaddr addr,
                                       uint64_t val, unsigned size,
                                       MemTxAttrs attrs)
{
    NeXTPC *s = opaque;
    unsigned off = addr & 0x1ff;

    if (off < 4) {
        for (unsigned i = 0; i < size; i++) {
            memory_region_dispatch_write(s->escc_mr, (off + i) & 3,
                                         (val >> (8 * (size - 1 - i))) & 0xff,
                                         MO_8, attrs);
        }
    } else if (off < 8) {
        s->scc_clock = deposit32(s->scc_clock, (8 - off - size) * 8,
                                 size * 8, val);
    } else if (off >= 0x100 && off < 0x104) {
        for (unsigned i = 0; i < size && s->ramdac_mr; i++) {
            memory_region_dispatch_write(s->ramdac_mr, (off + i) & 3,
                                         (val >> (8 * (size - 1 - i))) & 0xff,
                                         MO_8, attrs);
        }
    } else if (off == 0x180) {
        s->col_vid = val >> (8 * (size - 1));
        if (s->col_vid & VID_CMD_CLEAR_INT) {
            next_pc_set_int(s, NEXT_INT_DISK, false);
        }
    } else if (off == 0x190 || off == 0x198) {
        /* memory timings */
    } else {
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps next_scc_area_ops = {
    .read_with_attrs = next_scc_area_read,
    .write_with_attrs = next_scc_area_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* BMAP chip: 0x020c0000, 16 long words, bus error above 0x3f */
static uint32_t next_bmap_get(NeXTPC *s, int reg)
{
    uint32_t val = s->bmap[reg];

    if (reg == BMAP_DRW) {
        /* thin wire Ethernet: keeps the transceiver out of loopback */
        val &= ~(BMAP_HEARTBEAT | BMAP_TPE_ILBC);
        val |= BMAP_HEARTBEAT;
    }
    return val;
}

static MemTxResult next_bmap_read(void *opaque, hwaddr addr, uint64_t *data,
                                  unsigned size, MemTxAttrs attrs)
{
    NeXTPC *s = opaque;
    uint32_t val;

    if ((addr & 0xffff) > 0x3f) {
        return MEMTX_DECODE_ERROR;
    }
    val = next_bmap_get(s, (addr >> 2) & 0xf);
    *data = extract32(val, (4 - (addr & 3) - size) * 8, size * 8);
    return MEMTX_OK;
}

static MemTxResult next_bmap_write(void *opaque, hwaddr addr, uint64_t data,
                                   unsigned size, MemTxAttrs attrs)
{
    NeXTPC *s = opaque;
    int reg = (addr >> 2) & 0xf;
    uint32_t val;

    if ((addr & 0xffff) > 0x3f) {
        return MEMTX_DECODE_ERROR;
    }
    val = deposit32(s->bmap[reg], (4 - (addr & 3) - size) * 8, size * 8,
                    data);
    if (reg == BMAP_DRW && s->enet &&
        (val & BMAP_TPE) != (s->bmap[reg] & BMAP_TPE)) {
        if ((val & BMAP_TPE) == BMAP_TPE) {
            mb8795_set_tpe(s->enet, true);
        } else if ((val & BMAP_TPE) == 0) {
            mb8795_set_tpe(s->enet, false);
        }
    }
    s->bmap[reg] = val;
    if (reg == BMAP_BURWREN) {
        next_pc_check_dsp(s);
    }
    if (reg == BMAP_DDIR && (val & BMAP_RESET)) {
        next_warm_reset = true;
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps next_bmap_ops = {
    .read_with_attrs = next_bmap_read,
    .write_with_attrs = next_bmap_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,  /* MOVE16 */
    .impl.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* Vertical retrace */
static void next_vbl_tick(void *opaque)
{
    NeXTPC *s = opaque;

    if (s->turbo) {
        qemu_irq_pulse(s->vbl_out);
    } else if (s->color) {
        if (s->col_vid & VID_CMD_ENABLE_INT) {
            next_pc_set_int(s, NEXT_INT_DISK, true);
            s->col_vid &= ~VID_CMD_ENABLE_INT;
        }
    } else {
        next_dma_video_retrace(s->dma);
    }
    timer_mod(s->vbl_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / NEXT_VBL_FREQ);
}

static void next_pc_reset_hold(Object *obj, ResetType type)
{
    NeXTPC *s = NEXT_PC(obj);
    uint32_t scr1 = 0;

    memset(s->scr2, 0, sizeof(s->scr2));
    s->int_status = 0;
    s->int_mask = 0;
    s->int_level = -1;
    s->bright = 0;
    s->col_vid = 0;
    s->hardclock_csr = 0;
    s->hardclock_counter = 0xFFFF;
    s->hardclock_latch = 0;
    timer_del(s->hardclock_timer);
    s->eventc_offset = next_us();

    memset(s->bmap, 0, sizeof(s->bmap));
    s->bmap[BMAP_RCNTL] = BMAP_LO;

    if (s->turbo) {
        s->scr2[2] = 0x10; /* video mode is 25 MHz */
        s->scr2[3] = 0x80; /* local only */
        /* the model is what the Turbo machine is, the type says Turbo */
        if (s->model != NEXT_MODEL_CUBE) {
            scr1 |= 0xFu << 28;
        }
        scr1 |= NEXT_MODEL_TURBO << 12;
    } else {
        scr1 |= SCR1_DMA_REVISION << 16;
        scr1 |= s->model << 12;
        if (s->model == NEXT_MODEL_COMPUTER || s->new_rtc) {
            scr1 |= SCR1_BOARD_REV1 << 8;
        }
        scr1 |= SCR1_CPU_25MHZ;
    }
    s->scr1 = scr1;

    /* the DSP's memory as SCR2 now says */
    s->dsp_hreq = s->dsp_txd = false;
    if (s->dsp) {
        next_dsp_set_dma_options(s->dsp, false, false);
        next_dsp_set_memory(s->dsp, !s->turbo && next_pc_dsp_memreset(s));
    }

    next_rtc_reset_interface(s->rtc);
    next_pc_update_irq(s);

    timer_mod(s->vbl_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 1000);
}

static void next_pc_init(Object *obj)
{
    NeXTPC *s = NEXT_PC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    qdev_init_gpio_in(DEVICE(obj), next_pc_irq, NEXT_NUM_IRQS);
    qdev_init_gpio_in_named(DEVICE(obj), next_pc_scc_irq, "scc", 2);
    qdev_init_gpio_in_named(DEVICE(obj), next_pc_dsp_irq, "dsp",
                            NEXT_DSP_NUM_IRQS);

    memory_region_init_io(&s->int_mem, obj, &next_int_ops, s,
                          "next.intregs", 0x1000);
    sysbus_init_mmio(sbd, &s->int_mem);
    memory_region_init_io(&s->scr1_mem, obj, &next_scr1_ops, s,
                          "next.scr1", 0x1000);
    sysbus_init_mmio(sbd, &s->scr1_mem);
    memory_region_init_io(&s->scr2_mem, obj, &next_scr2_ops, s,
                          "next.scr2", 0x1000);
    sysbus_init_mmio(sbd, &s->scr2_mem);
    memory_region_init_io(&s->bright_mem, obj, &next_bright_ops, s,
                          "next.brightness", 0x2000);
    sysbus_init_mmio(sbd, &s->bright_mem);
    memory_region_init_io(&s->hardclock_mem, obj, &next_hardclock_ops, s,
                          "next.hardclock", 0x2000);
    sysbus_init_mmio(sbd, &s->hardclock_mem);
    memory_region_init_io(&s->eventc_mem, obj, &next_eventc_ops, s,
                          "next.eventcounter", 0x2000);
    sysbus_init_mmio(sbd, &s->eventc_mem);
    memory_region_init_io(&s->scc_mem, obj, &next_scc_area_ops, s,
                          "next.scc-area", 0x2000);
    sysbus_init_mmio(sbd, &s->scc_mem);
    memory_region_init_io(&s->bmap_mem, obj, &next_bmap_ops, s,
                          "next.bmap", 0x10000);
    sysbus_init_mmio(sbd, &s->bmap_mem);
}

static void next_pc_realize(DeviceState *dev, Error **errp)
{
    NeXTPC *s = NEXT_PC(dev);

    if (!s->rtc || !s->dma) {
        error_setg(errp, "next-pc needs the rtc and dma links");
        return;
    }
    s->hardclock_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      next_hardclock_tick, s);
    s->vbl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, next_vbl_tick, s);
    qdev_init_gpio_out_named(dev, &s->vbl_out, "vbl", 1);
}

static const Property next_pc_properties[] = {
    DEFINE_PROP_LINK("cpu", NeXTPC, cpu, TYPE_M68K_CPU, M68kCPU *),
    DEFINE_PROP_LINK("rtc", NeXTPC, rtc, TYPE_NEXT_RTC, NeXTRTC *),
    DEFINE_PROP_LINK("dma", NeXTPC, dma, TYPE_NEXT_DMA, NeXTDMAState *),
    DEFINE_PROP_UINT8("model", NeXTPC, model, NEXT_MODEL_CUBE),
    DEFINE_PROP_BOOL("turbo", NeXTPC, turbo, false),
    DEFINE_PROP_BOOL("color", NeXTPC, color, false),
    DEFINE_PROP_BOOL("new-rtc", NeXTPC, new_rtc, false),
};

static int next_pc_post_load(void *opaque, int version_id)
{
    NeXTPC *s = opaque;

    if (s->cpu) {
        m68k_set_irq_level(s->cpu, s->int_level, 24 + s->int_level);
    }
    return 0;
}

static const VMStateDescription next_pc_vmstate = {
    .name = "next-pc",
    .version_id = 7,
    .minimum_version_id = 7,
    .post_load = next_pc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(scr1, NeXTPC),
        VMSTATE_UINT8_ARRAY(scr2, NeXTPC, 4),
        VMSTATE_UINT32(int_status, NeXTPC),
        VMSTATE_UINT32(int_mask, NeXTPC),
        VMSTATE_INT32(int_level, NeXTPC),
        VMSTATE_UINT8(bright, NeXTPC),
        VMSTATE_UINT8(col_vid, NeXTPC),
        VMSTATE_UINT32(scc_clock, NeXTPC),
        VMSTATE_UINT32(scc_irq, NeXTPC),
        VMSTATE_UINT8(hardclock_csr, NeXTPC),
        VMSTATE_UINT16(hardclock_counter, NeXTPC),
        VMSTATE_UINT16(hardclock_latch, NeXTPC),
        VMSTATE_INT64(hardclock_last, NeXTPC),
        VMSTATE_TIMER_PTR(hardclock_timer, NeXTPC),
        VMSTATE_INT64(eventc_offset, NeXTPC),
        VMSTATE_UINT32(eventc_latch, NeXTPC),
        VMSTATE_TIMER_PTR(vbl_timer, NeXTPC),
        VMSTATE_UINT32_ARRAY(bmap, NeXTPC, 16),
        VMSTATE_BOOL(dsp_hreq, NeXTPC),
        VMSTATE_BOOL(dsp_txd, NeXTPC),
        VMSTATE_END_OF_LIST()
    },
};

static void next_pc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT system control";
    dc->realize = next_pc_realize;
    device_class_set_props(dc, next_pc_properties);
    dc->vmsd = &next_pc_vmstate;
    rc->phases.hold = next_pc_reset_hold;
}

static const TypeInfo next_pc_info = {
    .name = TYPE_NEXT_PC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = next_pc_init,
    .instance_size = sizeof(NeXTPC),
    .class_init = next_pc_class_init,
};

/* ------------------------------------------------------------------ */
/* Memory: empty banks and memory write functions                      */
/* ------------------------------------------------------------------ */

/* An empty memory bank reads back its address and ignores writes */
static uint64_t next_empty_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t base = (uintptr_t)opaque;
    uint32_t val = base + addr;

    return size == 4 ? val : val & ((1u << (8 * size)) - 1);
}

static void next_empty_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
}

static const MemoryRegionOps next_empty_ops = {
    .read = next_empty_read,
    .write = next_empty_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,  /* MOVE16 */
    .impl.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Memory write functions of the monochrome machines: writes through these
 * mirrors combine the new 2-bit pixels with the old ones.
 */
static const uint8_t next_mwf[4][4][4] = {
    { { 0, 0, 0, 0 }, { 0, 0, 1, 1 }, { 0, 1, 1, 2 }, { 0, 1, 2, 3 } },
    { { 0, 1, 2, 3 }, { 1, 2, 3, 3 }, { 2, 3, 3, 3 }, { 3, 3, 3, 3 } },
    { { 0, 0, 0, 0 }, { 1, 1, 0, 0 }, { 2, 1, 1, 0 }, { 3, 2, 1, 0 } },
    { { 0, 1, 2, 3 }, { 1, 2, 2, 3 }, { 2, 2, 3, 3 }, { 3, 3, 3, 3 } },
};

static uint32_t next_mwf_apply(uint32_t old, uint32_t new, int f, int size)
{
    uint32_t v = 0;

    for (int i = 0; i < size * 4; i++) {
        int a = (old >> (i * 2)) & 3;
        int b = (new >> (i * 2)) & 3;

        v |= (uint32_t)next_mwf[f][a][b] << (i * 2);
    }
    return v;
}

typedef struct NeXTMWF {
    hwaddr target;      /* where the plain memory is */
    hwaddr mask;        /* which address bits go there */
    int shift;          /* address bit that selects the function */
} NeXTMWF;

static uint64_t next_mwf_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTMWF *m = opaque;
    int f = (addr >> m->shift) & 3;
    uint64_t ones = size == 4 ? 0xffffffff : (1u << (8 * size)) - 1;

    return f == 0 ? ones : 0;
}

static void next_mwf_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    NeXTMWF *m = opaque;
    int f = (addr >> m->shift) & 3;
    hwaddr a = m->target | (addr & m->mask);
    uint32_t old = 0;
    uint8_t buf[4];

    address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                       buf, size);
    for (unsigned i = 0; i < size; i++) {
        old = (old << 8) | buf[i];
    }
    val = next_mwf_apply(old, val, f, size);
    for (unsigned i = 0; i < size; i++) {
        buf[i] = val >> (8 * (size - 1 - i));
    }
    address_space_write(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                        buf, size);
}

static const MemoryRegionOps next_mwf_ops = {
    .read = next_mwf_read,
    .write = next_mwf_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,  /* MOVE16 */
    .impl.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* Machines                                                            */
/* ------------------------------------------------------------------ */

#define TYPE_NEXT_MACHINE MACHINE_TYPE_NAME("next")
OBJECT_DECLARE_TYPE(NeXTState, NeXTMachineClass, NEXT_MACHINE)

struct NeXTMachineClass {
    MachineClass parent_class;

    NeXTModel model;
    bool turbo;
    bool color;
    bool cube;
    const char *rom;
};

struct NeXTState {
    MachineState parent;

    MemoryRegion rom;
    MemoryRegion rom_alias[2];
    MemoryRegion io;
    MemoryRegion io_alias;
    MemoryRegion bmap_alias;
    MemoryRegion ram_alias[4];
    MemoryRegion ram_empty[4];
    MemoryRegion ram_mwf;
    MemoryRegion vram_mwf;
    MemoryRegion vram_alias;
    NeXTMWF mwf_ram;
    NeXTMWF mwf_vram;

    NeXTDMAState dma;
    NeXTRTC rtc;
    NeXTSCSIState scsi;
    MB8795State enet;
    NeXTKMSState kms;
    NeXTMOState mo;
    NeXTFDCState fdc;
    MemoryRegion fdc_ctrl;      /* 68030: control register only */
    NeXTDSPState dsp;
    NeXTSoundState sound;
    BT463State ramdac;
    NeXTTMCState tmc;
    NeXTNCCState ncc;
    OrIRQState nmi_or;
    MemoryRegion mo_dummy;
    NeXTPrinterState printer;
    ESCCState escc;
    NeXTPC pc;

    uint32_t bank_size[4];
    bool new_rtc;
    bool net_services;
    char *nfs_share;
    bool nitro;
    bool dsp_ram_expansion;
    bool nextstep_idle;
};

/*
 * On the user network, the servers NeXTSTEP looks for when it configures
 * itself: bootparam, NetInfo and the time, as Previous gives them, and
 * NFS when there is a folder to share.
 */
static void next_net_services(NeXTState *m, NeXTMachineClass *nc)
{
#ifdef CONFIG_SLIRP
    NetClientState *peer = qemu_get_queue(m->enet.nic)->peer;
    g_autofree char *type = NULL;

    if (!m->net_services || !peer ||
        peer->info->type != NET_CLIENT_DRIVER_USER) {
        if (m->nfs_share) {
            error_report("nfs-share needs the user network (-nic user)");
            exit(1);
        }
        return;
    }
    type = g_strdup_printf("NeXT%s%s%s", nc->cube ? "cube" : "station",
                           nc->turbo ? " Turbo" : "",
                           nc->color ? " Color" : "");
    object_new_with_props(TYPE_FILTER_NEXT, object_get_objects_root(),
                          "next-net-services", &error_fatal,
                          "netdev", peer->name,
                          "share", m->nfs_share ? m->nfs_share : "",
                          "system-type", type,
                          NULL);
#else
    if (m->nfs_share) {
        error_report("nfs-share needs QEMU built with the user network");
        exit(1);
    }
#endif
}

/* Split the RAM into the four banks with the SIMM sizes the model takes */
static uint64_t next_fdc_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void next_fdc_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
}

static const MemoryRegionOps next_fdc_ctrl_ops = {
    .read = next_fdc_ctrl_read,
    .write = next_fdc_ctrl_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static bool next_split_ram(NeXTMachineClass *nc, uint64_t ram_size,
                           uint32_t bank[4], Error **errp)
{
    static const uint32_t mono_sizes[] = { 16 * MiB, 4 * MiB, 1 * MiB, 0 };
    static const uint32_t color_sizes[] = { 8 * MiB, 2 * MiB, 0 };
    static const uint32_t turbo_sizes[] = { 32 * MiB, 8 * MiB, 2 * MiB, 0 };
    static const uint32_t computer_sizes[] = { 4 * MiB, 1 * MiB, 0 };
    const uint32_t *sizes = nc->turbo ? turbo_sizes :
                            nc->color ? color_sizes :
                            nc->model == NEXT_MODEL_COMPUTER ? computer_sizes :
                            mono_sizes;
    uint64_t left = ram_size;

    for (int i = 0; i < 4; i++) {
        bank[i] = 0;
        for (int j = 0; sizes[j]; j++) {
            if (sizes[j] <= left) {
                bank[i] = sizes[j];
                break;
            }
        }
        left -= bank[i];
    }
    if (left) {
        g_autofree char *max = size_to_str(4ULL * sizes[0]);

        error_setg(errp, "this NeXT cannot have that much memory");
        error_append_hint(errp, "Give it up to %s, made of banks of ", max);
        for (int j = 0; sizes[j]; j++) {
            error_append_hint(errp, "%s%u MB", j ? ", " : "",
                              (unsigned)(sizes[j] / MiB));
        }
        error_append_hint(errp, ".\n");
        return false;
    }
    return true;
}

static void next_map_ram(NeXTState *m, NeXTMachineClass *nc,
                         MemoryRegion *sysmem)
{
    MachineState *machine = MACHINE(m);
    uint32_t stride = nc->turbo ? 32 * MiB : nc->color ? 8 * MiB : 16 * MiB;
    uint64_t offset = 0;

    for (int i = 0; i < 4; i++) {
        hwaddr base = NEXT_RAM_BASE + (hwaddr)i * stride;
        uint32_t size = m->bank_size[i];

        if (!size) {
            memory_region_init_io(&m->ram_empty[i], OBJECT(m),
                                  &next_empty_ops, (void *)(uintptr_t)base,
                                  "next.ram.empty", stride);
            memory_region_add_subregion(sysmem, base, &m->ram_empty[i]);
            continue;
        }
        /* smaller SIMMs show up again and again in the bank */
        memory_region_init_alias(&m->ram_alias[i], OBJECT(m), "next.ram.bank",
                                 machine->ram, offset, size);
        memory_region_add_subregion(sysmem, base, &m->ram_alias[i]);
        for (uint32_t a = size; a < stride; a += size) {
            MemoryRegion *mirror = g_new(MemoryRegion, 1);

            memory_region_init_alias(mirror, OBJECT(m), "next.ram.mirror",
                                     machine->ram, offset, size);
            memory_region_add_subregion(sysmem, base + a, mirror);
        }
        offset += size;
    }
}

static uint16_t next_simm_config(NeXTState *m, NeXTMachineClass *nc)
{
    uint16_t parity = 0xF0;
    uint8_t simm[4];

    for (int i = 0; i < 4; i++) {
        uint32_t mb = m->bank_size[i] / MiB;

        if (nc->turbo) {
            parity = 0;
            simm[i] = mb == 32 ? 1 : mb == 8 ? 2 : mb == 2 ? 3 : 0;
        } else if (nc->color) {
            simm[i] = mb == 8 ? 1 : mb == 2 ? 2 : 0;
            if (!mb) {
                parity &= ~(0x10 << i);
            }
        } else {
            /* page mode SIMMs */
            simm[i] = (mb == 16 ? 1 : mb == 4 ? 2 : mb == 1 ? 3 : 0) | 4;
            if (!mb) {
                simm[i] = 0;
                parity &= ~(0x10 << i);
            }
        }
    }
    return ((parity & 0xF0) << 8) | (simm[3] << 9) | (simm[2] << 6) |
           (simm[1] << 3) | simm[0];
}

static void next_power_key(void *opaque, bool down)
{
    if (down) {
        next_rtc_request_power_down(opaque);
    } else {
        next_rtc_stop_power_down(opaque);
    }
}

/* The ADB power key goes to the clock chip */
static void next_tmc_power_key(void *opaque, int n, int level)
{
    next_power_key(opaque, level);
}

/* Registers that read 0 and ignore writes */
static uint64_t next_dummy_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void next_dummy_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
}

static const MemoryRegionOps next_dummy_ops = {
    .read = next_dummy_read,
    .write = next_dummy_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void next_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    uint32_t dregs[8], aregs[7];

    /*
     * A reset through the BMAP leaves the data and address registers as
     * they were, as on the real CPU: the 2.2 ROMs (v63 and older) set D1
     * to a magic value, reset, and go on when they find it after.
     */
    memcpy(dregs, cpu->env.dregs, sizeof(dregs));
    memcpy(aregs, cpu->env.aregs, sizeof(aregs));
    cpu_reset(cs);
    if (next_warm_reset) {
        memcpy(cpu->env.dregs, dregs, sizeof(dregs));
        memcpy(cpu->env.aregs, aregs, sizeof(aregs));
        next_warm_reset = false;
    }
    /* the ROM is at 0: stack pointer and start address */
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
    cpu->env.vbr = 0;
    cpu->env.sr = 0x2700;
}

static void next_machine_init(MachineState *machine)
{
    NeXTState *m = NEXT_MACHINE(machine);
    NeXTMachineClass *nc = NEXT_MACHINE_GET_CLASS(machine);
    MemoryRegion *sysmem = get_system_memory();
    const char *bios_name = machine->firmware ?: nc->rom;
    bool is_030 = nc->model == NEXT_MODEL_COMPUTER;
    DeviceState *dev;
    SysBusDevice *sbd;
    M68kCPU *cpu;
    DriveInfo *dinfo;
    uint8_t enet[6] = { 0 };
    hwaddr vram_base;

    if (!next_split_ram(nc, machine->ram_size, m->bank_size, &error_fatal)) {
        return;
    }

    cpu = M68K_CPU(object_new(machine->cpu_type));
    /*
     * NeXTSTEP's printf uses FMOVE.P, which the 68040 traps to the FPSP:
     * the NeXT FPSP needs FSAVE frames QEMU does not make, and loops on
     * the trap for ever. Convert packed decimal as the 68882 does.
     */
    object_property_set_str(OBJECT(cpu), "packed-decimal", "on",
                            &error_fatal);
    qdev_realize_and_unref(DEVICE(cpu), NULL, &error_fatal);
    cpu->env.nextstep_idle = m->nextstep_idle;
    qemu_register_reset(next_cpu_reset, cpu);

    /* Device space */
    memory_region_init(&m->io, OBJECT(m), "next.io", NEXT_IO_SIZE);
    memory_region_add_subregion(sysmem, NEXT_IO_BASE, &m->io);
    if (!is_030) {
        memory_region_init_alias(&m->io_alias, OBJECT(m), "next.io.bmap",
                                 &m->io, 0, NEXT_IO_SIZE);
        memory_region_add_subregion(sysmem, NEXT_IO_BMAP_BASE, &m->io_alias);
    }

    /* DMA */
    object_initialize_child(OBJECT(m), "dma", &m->dma, TYPE_NEXT_DMA);
    qdev_prop_set_bit(DEVICE(&m->dma), "turbo", nc->turbo);
    sysbus_realize(SYS_BUS_DEVICE(&m->dma), &error_fatal);
    memory_region_add_subregion(&m->io, NEXT_IO_DMA_CSR,
                        sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->dma), 0));
    memory_region_add_subregion(&m->io, NEXT_IO_DMA_REGS,
                        sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->dma), 1));

    /* RTC and NVRAM */
    object_initialize_child(OBJECT(m), "rtc", &m->rtc, TYPE_NEXT_RTC);
    qdev_prop_set_bit(DEVICE(&m->rtc), "new-chip", m->new_rtc);
    qdev_prop_set_bit(DEVICE(&m->rtc), "turbo", nc->turbo);
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(DEVICE(&m->rtc), "drive",
                                blk_by_legacy_dinfo(dinfo), &error_fatal);
    }
    sysbus_realize(SYS_BUS_DEVICE(&m->rtc), &error_fatal);

    /* SCC */
    object_initialize_child(OBJECT(m), "escc", &m->escc, TYPE_ESCC);
    dev = DEVICE(&m->escc);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", 9600 * 384);
    qdev_prop_set_uint32(dev, "it_shift", 0);
    qdev_prop_set_bit(dev, "bit_swap", true);
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_uint32(dev, "chnBtype", escc_serial);
    qdev_prop_set_uint32(dev, "chnAtype", escc_serial);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);

    /* System control */
    object_initialize_child(OBJECT(m), "pc", &m->pc, TYPE_NEXT_PC);
    dev = DEVICE(&m->pc);
    sbd = SYS_BUS_DEVICE(dev);
    object_property_set_link(OBJECT(dev), "cpu", OBJECT(cpu), &error_abort);
    object_property_set_link(OBJECT(dev), "rtc", OBJECT(&m->rtc),
                             &error_abort);
    object_property_set_link(OBJECT(dev), "dma", OBJECT(&m->dma),
                             &error_abort);
    qdev_prop_set_uint8(dev, "model", nc->model);
    qdev_prop_set_bit(dev, "turbo", nc->turbo);
    qdev_prop_set_bit(dev, "color", nc->color);
    qdev_prop_set_bit(dev, "new-rtc", m->new_rtc);
    m->pc.escc_mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->escc), 0);

    /* The colour machines have a Bt463 RAMDAC */
    if (nc->color) {
        object_initialize_child(OBJECT(m), "ramdac", &m->ramdac, TYPE_BT463);
        sysbus_realize(SYS_BUS_DEVICE(&m->ramdac), &error_fatal);
        if (nc->turbo) {
            memory_region_add_subregion(&m->io, NEXT_IO_RAMDAC_TURBO,
                        sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->ramdac), 0));
        } else {
            m->pc.ramdac_mr =
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->ramdac), 0);
        }
    }
    sysbus_realize(sbd, &error_fatal);
    memory_region_add_subregion(&m->io, NEXT_IO_INTSTAT,
                                sysbus_mmio_get_region(sbd, 0));
    memory_region_add_subregion(&m->io, NEXT_IO_SCR1,
                                sysbus_mmio_get_region(sbd, 1));
    memory_region_add_subregion(&m->io, NEXT_IO_SCR2,
                                sysbus_mmio_get_region(sbd, 2));
    memory_region_add_subregion(&m->io, NEXT_IO_BRIGHTNESS,
                                sysbus_mmio_get_region(sbd, 3));
    memory_region_add_subregion(&m->io, NEXT_IO_HARDCLOCK,
                                sysbus_mmio_get_region(sbd, 4));
    memory_region_add_subregion(&m->io, NEXT_IO_EVENTC,
                                sysbus_mmio_get_region(sbd, 5));
    memory_region_add_subregion(&m->io, NEXT_IO_SCC,
                                sysbus_mmio_get_region(sbd, 6));
    if (!is_030 && !nc->turbo) {
        memory_region_add_subregion(sysmem, NEXT_BMAP_BASE,
                                    sysbus_mmio_get_region(sbd, 7));
        memory_region_init_alias(&m->bmap_alias, OBJECT(m), "next.bmap2",
                                 sysbus_mmio_get_region(sbd, 7), 0, 0x10000);
        memory_region_add_subregion(sysmem, NEXT_BMAP_BASE2, &m->bmap_alias);
    }

    /* the DMA channels interrupt through the system control */
    for (int i = 0; i < NEXT_DMA_NUM_CHANNELS; i++) {
        static const int dma_int[NEXT_DMA_NUM_CHANNELS] = {
            [NEXT_DMA_SCSI] = NEXT_INT_SCSI_DMA,
            [NEXT_DMA_SOUNDOUT] = NEXT_INT_SND_OUT_DMA,
            [NEXT_DMA_DISK] = NEXT_INT_DISK_DMA,
            [NEXT_DMA_SOUNDIN] = NEXT_INT_SND_IN_DMA,
            [NEXT_DMA_PRINTER] = NEXT_INT_PRINTER_DMA,
            [NEXT_DMA_SCC] = NEXT_INT_SCC_DMA,
            [NEXT_DMA_DSP] = NEXT_INT_DSP_DMA,
            [NEXT_DMA_EN_TX] = NEXT_INT_EN_TX_DMA,
            [NEXT_DMA_EN_RX] = NEXT_INT_EN_RX_DMA,
            [NEXT_DMA_VIDEO] = NEXT_INT_VIDEO,
            [NEXT_DMA_M2R] = NEXT_INT_M2R_DMA,
            [NEXT_DMA_R2M] = NEXT_INT_R2M_DMA,
        };
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->dma), i,
                           qdev_get_gpio_in(dev, dma_int[i]));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->rtc), 0,
                       qdev_get_gpio_in(dev, NEXT_INT_POWER));
    for (int i = 0; i < 2; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->escc), i,
                           qdev_get_gpio_in_named(dev, "scc", i));
    }

    /* SCSI */
    object_initialize_child(OBJECT(m), "scsi", &m->scsi, TYPE_NEXT_SCSI);
    object_property_set_link(OBJECT(&m->scsi), "dma", OBJECT(&m->dma),
                             &error_abort);
    qdev_prop_set_bit(DEVICE(&m->scsi), "53c90a", !is_030);
    sysbus_realize(SYS_BUS_DEVICE(&m->scsi), &error_fatal);
    memory_region_add_subregion(&m->io, NEXT_IO_SCSI,
                        sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->scsi), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->scsi), 0,
                       qdev_get_gpio_in(dev, NEXT_INT_SCSI));

    /* Ethernet */
    {
        NICInfo *nd = qemu_find_nic_info(TYPE_MB8795, true, NULL);

        object_initialize_child(OBJECT(m), "enet", &m->enet, TYPE_MB8795);
        if (nd) {
            qdev_set_nic_properties(DEVICE(&m->enet), nd);
        }
        object_property_set_link(OBJECT(&m->enet), "dma", OBJECT(&m->dma),
                                 &error_abort);
        qdev_prop_set_bit(DEVICE(&m->enet), "turbo", nc->turbo);
        /* NeXTSTEP looks for twisted pair first on the Turbo machines */
        qdev_prop_set_bit(DEVICE(&m->enet), "twisted-pair", nc->turbo);
        qdev_prop_set_bit(DEVICE(&m->enet), "no-tpe", is_030);
        sysbus_realize(SYS_BUS_DEVICE(&m->enet), &error_fatal);
        memory_region_add_subregion(&m->io, NEXT_IO_ENET,
                        sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->enet), 0));
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->enet), 0,
                           qdev_get_gpio_in(dev, NEXT_INT_EN_TX));
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->enet), 1,
                           qdev_get_gpio_in(dev, NEXT_INT_EN_RX));
        m->pc.enet = &m->enet;
        next_net_services(m, nc);
    }

    /* MO controller: on all machines but the Turbo ones */
    if (nc->turbo) {
        /* four registers that read 0, as in Previous */
        memory_region_init_io(&m->mo_dummy, OBJECT(m), &next_dummy_ops, NULL,
                              "next.mo-dummy", 0x2000);
        memory_region_add_subregion(&m->io, NEXT_IO_MO, &m->mo_dummy);
    } else {
        object_initialize_child(OBJECT(m), "mo", &m->mo, TYPE_NEXT_MO);
        object_property_set_link(OBJECT(&m->mo), "dma", OBJECT(&m->dma),
                                 &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&m->mo), &error_fatal);
        memory_region_add_subregion(&m->io, NEXT_IO_MO,
                        sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->mo), 0));
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->mo), 0,
                           qdev_get_gpio_in(dev, NEXT_INT_DISK));
    }

    /*
     * Floppy: on the 68040 machines. The 68030 board has no 82077, but
     * still has the external control register, which says no drive.
     */
    if (is_030) {
        memory_region_init_io(&m->fdc_ctrl, OBJECT(m), &next_fdc_ctrl_ops,
                              NULL, "next.floppy.ctrl", 1);
        memory_region_add_subregion(&m->io, NEXT_IO_FLOPPY + 8, &m->fdc_ctrl);
    } else {
        DriveInfo *fd = drive_get(IF_FLOPPY, 0, 0);

        object_initialize_child(OBJECT(m), "fdc", &m->fdc, TYPE_NEXT_FDC);
        if (fd) {
            qdev_prop_set_drive_err(DEVICE(&m->fdc), "drive",
                                    blk_by_legacy_dinfo(fd), &error_fatal);
        }
        object_property_set_link(OBJECT(&m->fdc), "scsi", OBJECT(&m->scsi),
                                 &error_abort);
        qdev_prop_set_bit(DEVICE(&m->fdc), "turbo", nc->turbo);
        qdev_prop_set_bit(DEVICE(&m->fdc), "station", !nc->cube);
        qdev_prop_set_bit(DEVICE(&m->fdc), "color", nc->color);
        sysbus_realize(SYS_BUS_DEVICE(&m->fdc), &error_fatal);
        memory_region_add_subregion(&m->io, NEXT_IO_FLOPPY,
                        sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->fdc), 0));
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->fdc), 0,
                           qdev_get_gpio_in(dev, NEXT_INT_PHONE));
    }

    /* DSP */
    object_initialize_child(OBJECT(m), "dsp", &m->dsp, TYPE_NEXT_DSP);
    object_property_set_link(OBJECT(&m->dsp), "dma", OBJECT(&m->dma),
                             &error_abort);
    qdev_prop_set_bit(DEVICE(&m->dsp), "turbo", nc->turbo);
    qdev_prop_set_bit(DEVICE(&m->dsp), "ram-expansion", m->dsp_ram_expansion);
    sysbus_realize(SYS_BUS_DEVICE(&m->dsp), &error_fatal);
    for (int i = 0; i < NEXT_DSP_NUM_IRQS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->dsp), i,
                           qdev_get_gpio_in_named(dev, "dsp", i));
    }
    m->pc.dsp = &m->dsp;
    memory_region_add_subregion(&m->io, NEXT_IO_DSP,
                        sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->dsp), 0));

    /* Laser printer port */
    object_initialize_child(OBJECT(m), "printer", &m->printer,
                            TYPE_NEXT_PRINTER);
    object_property_set_link(OBJECT(&m->printer), "dma", OBJECT(&m->dma),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&m->printer), &error_fatal);
    memory_region_add_subregion(&m->io, NEXT_IO_PRINTER,
                    sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->printer), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->printer), 0,
                       qdev_get_gpio_in(dev, NEXT_INT_PRINTER));

    /* Keyboard, mouse and sound */
    object_initialize_child(OBJECT(m), "kms", &m->kms, TYPE_NEXT_KMS);
    qdev_prop_set_bit(DEVICE(&m->kms), "turbo", nc->turbo);
    qdev_prop_set_bit(DEVICE(&m->kms), "input", !nc->turbo);
    sysbus_realize(SYS_BUS_DEVICE(&m->kms), &error_fatal);
    memory_region_add_subregion(&m->io, NEXT_IO_KMS,
                        sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->kms), 0));
    {
        static const int kms_int[NEXT_KMS_NUM_IRQS] = {
            [NEXT_KMS_IRQ_KEYMOUSE] = NEXT_INT_KEYMOUSE,
            [NEXT_KMS_IRQ_MONITOR] = NEXT_INT_MONITOR,
            [NEXT_KMS_IRQ_SOUND_OVRUN] = NEXT_INT_SOUND_OVRUN,
            [NEXT_KMS_IRQ_NMI] = NEXT_INT_NMI,
        };
        /* the KMS and the TMC both give NMIs */
        object_initialize_child(OBJECT(m), "nmi-or", &m->nmi_or, TYPE_OR_IRQ);
        object_property_set_int(OBJECT(&m->nmi_or), "num-lines", 2,
                                &error_abort);
        qdev_realize(DEVICE(&m->nmi_or), NULL, &error_fatal);
        qdev_connect_gpio_out(DEVICE(&m->nmi_or), 0,
                              qdev_get_gpio_in(dev, NEXT_INT_NMI));
        for (int i = 0; i < NEXT_KMS_NUM_IRQS; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&m->kms), i,
                               i == NEXT_KMS_IRQ_NMI ?
                               qdev_get_gpio_in(DEVICE(&m->nmi_or), 0) :
                               qdev_get_gpio_in(dev, kms_int[i]));
        }
    }
    next_kms_set_power_key(&m->kms, next_power_key, &m->rtc);

    /* Turbo memory controller, with the ADB keyboard and mouse */
    if (nc->turbo) {
        BusState *adb;

        object_initialize_child(OBJECT(m), "tmc", &m->tmc, TYPE_NEXT_TMC);
        qdev_prop_set_uint8(DEVICE(&m->tmc), "machine-type",
                            nc->cube ? NEXT_TMC_CUBE :
                            nc->color ? NEXT_TMC_STATION_COLOR :
                            NEXT_TMC_STATION);
        qdev_prop_set_bit(DEVICE(&m->tmc), "station", !nc->cube);
        sysbus_realize(SYS_BUS_DEVICE(&m->tmc), &error_fatal);
        memory_region_add_subregion(sysmem, NEXT_TMC_BASE,
                        sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->tmc), 0));
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->tmc), NEXT_TMC_IRQ_DISK,
                           qdev_get_gpio_in(dev, NEXT_INT_DISK));
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->tmc), NEXT_TMC_IRQ_NMI,
                           qdev_get_gpio_in(DEVICE(&m->nmi_or), 1));
        sysbus_connect_irq(SYS_BUS_DEVICE(&m->tmc), NEXT_TMC_IRQ_POWER,
                           qemu_allocate_irq(next_tmc_power_key, &m->rtc, 0));
        qdev_connect_gpio_out_named(dev, "vbl", 0,
                                    qdev_get_gpio_in_named(DEVICE(&m->tmc),
                                                           "vbl", 0));
        /* the 40 MHz boards have a second level cache */
        if (m->nitro) {
            SysBusDevice *ncc;

            object_initialize_child(OBJECT(m), "ncc", &m->ncc, TYPE_NEXT_NCC);
            ncc = SYS_BUS_DEVICE(&m->ncc);
            sysbus_realize(ncc, &error_fatal);
            memory_region_add_subregion(sysmem, NEXT_NCC_BASE,
                                        sysbus_mmio_get_region(ncc, 0));
            memory_region_add_subregion(sysmem, NEXT_NCC_TAG_BASE,
                                        sysbus_mmio_get_region(ncc, 1));
            memory_region_add_subregion(sysmem, NEXT_NCC_CACHE_BASE,
                                        sysbus_mmio_get_region(ncc, 2));
        }
        adb = qdev_get_child_bus(DEVICE(&m->tmc), "adb.0");
        qdev_realize_and_unref(qdev_new(TYPE_ADB_KEYBOARD), adb,
                               &error_fatal);
        qdev_realize_and_unref(qdev_new(TYPE_ADB_MOUSE), adb, &error_fatal);
    }

    /* Sound, in the monitor at the other end of the KMS link */
    object_initialize_child(OBJECT(m), "sound", &m->sound, TYPE_NEXT_SOUND);
    object_property_set_link(OBJECT(&m->sound), "dma", OBJECT(&m->dma),
                             &error_abort);
    object_property_set_link(OBJECT(&m->sound), "kms", OBJECT(&m->kms),
                             &error_abort);
    if (MACHINE(m)->audiodev) {
        qdev_prop_set_string(DEVICE(&m->sound), "audiodev",
                             MACHINE(m)->audiodev);
    }
    sysbus_realize(SYS_BUS_DEVICE(&m->sound), &error_fatal);

    /* RAM */
    next_map_ram(m, nc, sysmem);
    if (!nc->color && !nc->turbo) {
        m->mwf_ram = (NeXTMWF) { NEXT_RAM_BASE, 0x03FFFFFF, 26 };
        memory_region_init_io(&m->ram_mwf, OBJECT(m), &next_mwf_ops,
                              &m->mwf_ram, "next.ram.mwf", 4 * 64 * MiB);
        memory_region_add_subregion(sysmem, NEXT_RAM_MWF_BASE, &m->ram_mwf);
    }

    /* Frame buffer */
    dev = qdev_new(TYPE_NEXTFB);
    qdev_prop_set_bit(dev, "color", nc->color);
    qdev_prop_set_bit(dev, "turbo", nc->turbo);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    vram_base = nc->turbo ? NEXT_VRAM_TURBO_BASE :
                nc->color ? NEXT_VRAM_COLOR_BASE : NEXT_VRAM_BASE;
    {
        MemoryRegion *vram = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
        uint64_t vsize = memory_region_size(vram);

        /* the VRAM repeats through a 16 MB window */
        memory_region_add_subregion(sysmem, vram_base, vram);
        for (uint64_t a = vsize; a < 16 * MiB; a += vsize) {
            MemoryRegion *mr = g_new(MemoryRegion, 1);

            memory_region_init_alias(mr, OBJECT(m), "next.vram", vram, 0,
                                     vsize);
            memory_region_add_subregion(sysmem, vram_base + a, mr);
        }
        if (!nc->color && !nc->turbo) {
            m->mwf_vram = (NeXTMWF) { NEXT_VRAM_BASE, vsize - 1, 24 };
            memory_region_init_io(&m->vram_mwf, OBJECT(m), &next_mwf_ops,
                                  &m->mwf_vram, "next.vram.mwf",
                                  4 * 16 * MiB);
            memory_region_add_subregion(sysmem, NEXT_VRAM_MWF_BASE,
                                        &m->vram_mwf);
        }
    }

    /* ROM: 128 kB, repeating through 16 MB at 0, and at 16 MB but on 030 */
    memory_region_init_rom(&m->rom, NULL, "next.rom", 128 * KiB,
                           &error_fatal);
    for (int w = 0; w < (is_030 ? 1 : 2); w++) {
        hwaddr base = w ? NEXT_ROM_DIAG_BASE : NEXT_ROM_BASE;

        for (hwaddr a = 0; a < 16 * MiB; a += 128 * KiB) {
            MemoryRegion *mr = a ? g_new(MemoryRegion, 1) : &m->rom_alias[w];

            memory_region_init_alias(mr, OBJECT(m), "next.rom.alias", &m->rom,
                                     0, 128 * KiB);
            memory_region_add_subregion(sysmem, base + a, mr);
        }
    }
    if (bios_name) {
        g_autofree char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS,
                                               bios_name);
        g_autofree char *data = NULL;
        gsize size = 0;

        if (path && g_file_get_contents(path, &data, &size, NULL) &&
            size >= 8 && size <= 128 * KiB) {
            uint8_t *rom = memory_region_get_ram_ptr(&m->rom);

            memcpy(rom, data, size);
            /* the Ethernet address is in the ROM */
            memcpy(enet, rom + 8, 6);
        } else if (!qtest_enabled()) {
            error_report("Cannot load the NeXT ROM '%s'", bios_name);
            error_printf("Give a ROM image of 64 or 128 kB with -bios.\n");
            exit(1);
        }
    }
    next_rtc_set_defaults(&m->rtc, enet, next_simm_config(m, nc));
}

static bool next_get_new_rtc(Object *obj, Error **errp)
{
    return NEXT_MACHINE(obj)->new_rtc;
}

static void next_set_new_rtc(Object *obj, bool value, Error **errp)
{
    NEXT_MACHINE(obj)->new_rtc = value;
}

static bool next_get_net_services(Object *obj, Error **errp)
{
    return NEXT_MACHINE(obj)->net_services;
}

static void next_set_net_services(Object *obj, bool value, Error **errp)
{
    NEXT_MACHINE(obj)->net_services = value;
}

static char *next_get_nfs_share(Object *obj, Error **errp)
{
    return g_strdup(NEXT_MACHINE(obj)->nfs_share);
}

static void next_set_nfs_share(Object *obj, const char *value, Error **errp)
{
    NeXTState *m = NEXT_MACHINE(obj);

    g_free(m->nfs_share);
    m->nfs_share = value && *value ? g_strdup(value) : NULL;
}

static bool next_get_nitro(Object *obj, Error **errp)
{
    return NEXT_MACHINE(obj)->nitro;
}

static void next_set_nitro(Object *obj, bool value, Error **errp)
{
    if (value && !NEXT_MACHINE_GET_CLASS(obj)->turbo) {
        error_setg(errp, "only the Turbo machines have Nitro boards");
        return;
    }
    NEXT_MACHINE(obj)->nitro = value;
}

static bool next_get_dsp_ram(Object *obj, Error **errp)
{
    return NEXT_MACHINE(obj)->dsp_ram_expansion;
}

static void next_set_dsp_ram(Object *obj, bool value, Error **errp)
{
    NEXT_MACHINE(obj)->dsp_ram_expansion = value;
}

static bool next_get_nextstep_idle(Object *obj, Error **errp)
{
    return NEXT_MACHINE(obj)->nextstep_idle;
}

static void next_set_nextstep_idle(Object *obj, bool value, Error **errp)
{
    NEXT_MACHINE(obj)->nextstep_idle = value;
}

static void next_machine_instance_init(Object *obj)
{
    NeXTMachineClass *nc = NEXT_MACHINE_GET_CLASS(obj);

    /* Turbo machines have the newer MCCS1850 clock */
    NEXT_MACHINE(obj)->new_rtc = nc->turbo;
    NEXT_MACHINE(obj)->net_services = true;
    NEXT_MACHINE(obj)->nextstep_idle = true;
}

static void next_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = next_machine_init;
    mc->block_default_type = IF_SCSI;
    mc->default_ram_id = "next.ram";
    machine_add_audiodev_property(mc);

    object_class_property_add_bool(oc, "new-rtc", next_get_new_rtc,
                                   next_set_new_rtc);
    object_class_property_set_description(oc, "new-rtc",
        "Use the MCCS1850 clock chip instead of the MC68HC68T1");
    object_class_property_add_bool(oc, "net-services", next_get_net_services,
                                   next_set_net_services);
    object_class_property_set_description(oc, "net-services",
        "On the user network, answer NeXTSTEP's bootparam, NetInfo and "
        "time requests (default: on)");
    object_class_property_add_str(oc, "nfs-share", next_get_nfs_share,
                                  next_set_nfs_share);
    object_class_property_add_bool(oc, "nitro", next_get_nitro,
                                   next_set_nitro);
    object_class_property_set_description(oc, "nitro",
        "On the Turbo machines, a 40 MHz Nitro board with its cache "
        "controller");
    object_class_property_add_bool(oc, "dsp-ram-expansion", next_get_dsp_ram,
                                   next_set_dsp_ram);
    object_class_property_set_description(oc, "dsp-ram-expansion",
        "32K words of DSP memory instead of 8K (default: off)");
    object_class_property_add_bool(oc, "nextstep-idle",
                                   next_get_nextstep_idle,
                                   next_set_nextstep_idle);
    object_class_property_set_description(oc, "nextstep-idle",
        "Halt the CPU in Mach's idle loop, which spins on a real NeXT "
        "(default: on)");
    object_class_property_set_description(oc, "nfs-share",
        "A host folder to share by NFS on the user network; NeXTSTEP "
        "mounts it on /Net");
}

typedef struct NeXTModelInfo {
    const char *name;
    const char *desc;
    NeXTModel model;
    bool turbo;
    bool color;
    bool cube;
    const char *cpu;
    const char *rom;
    ram_addr_t ram;
} NeXTModelInfo;

static void next_model_class_init(ObjectClass *oc, const void *data)
{
    const NeXTModelInfo *info = data;
    MachineClass *mc = MACHINE_CLASS(oc);
    NeXTMachineClass *nc = NEXT_MACHINE_CLASS(oc);

    mc->desc = info->desc;
    mc->default_cpu_type = info->cpu;
    mc->default_ram_size = info->ram;
    nc->model = info->model;
    nc->turbo = info->turbo;
    nc->color = info->color;
    nc->cube = info->cube;
    nc->rom = info->rom;
}

static const NeXTModelInfo next_models[] = {
    {
        .name = MACHINE_TYPE_NAME("next-computer"),
        .desc = "NeXT Computer (68030)",
        .model = NEXT_MODEL_COMPUTER,
        .cube = true,
        .cpu = M68K_CPU_TYPE_NAME("m68030"),
        .rom = "Rev_1.2.bin",
        .ram = 16 * MiB,
    }, {
        .name = MACHINE_TYPE_NAME("next-cube"),
        .desc = "NeXTcube (68040)",
        .model = NEXT_MODEL_CUBE,
        .cube = true,
        .cpu = M68K_CPU_TYPE_NAME("m68040"),
        .rom = "Rev_2.5_v66.bin",
        .ram = 64 * MiB,
    }, {
        .name = MACHINE_TYPE_NAME("next-station"),
        .desc = "NeXTstation (68040)",
        .model = NEXT_MODEL_STATION,
        .cpu = M68K_CPU_TYPE_NAME("m68040"),
        .rom = "Rev_2.5_v66.bin",
        .ram = 32 * MiB,
    }, {
        .name = MACHINE_TYPE_NAME("next-station-color"),
        .desc = "NeXTstation Color (68040)",
        .model = NEXT_MODEL_STATION_COLOR,
        .color = true,
        .cpu = M68K_CPU_TYPE_NAME("m68040"),
        .rom = "Rev_2.5_v66.bin",
        .ram = 32 * MiB,
    }, {
        .name = MACHINE_TYPE_NAME("next-cube-turbo"),
        .desc = "NeXTcube Turbo (68040)",
        .model = NEXT_MODEL_CUBE,
        .turbo = true,
        .cube = true,
        .cpu = M68K_CPU_TYPE_NAME("m68040"),
        .rom = "Rev_3.3_v74.bin",
        .ram = 64 * MiB,
    }, {
        .name = MACHINE_TYPE_NAME("next-station-turbo"),
        .desc = "NeXTstation Turbo (68040)",
        .model = NEXT_MODEL_STATION,
        .turbo = true,
        .cpu = M68K_CPU_TYPE_NAME("m68040"),
        .rom = "Rev_3.3_v74.bin",
        .ram = 32 * MiB,
    }, {
        .name = MACHINE_TYPE_NAME("next-station-turbo-color"),
        .desc = "NeXTstation Turbo Color (68040)",
        .model = NEXT_MODEL_STATION_COLOR,
        .turbo = true,
        .color = true,
        .cpu = M68K_CPU_TYPE_NAME("m68040"),
        .rom = "Rev_3.3_v74.bin",
        .ram = 32 * MiB,
    },
};

static const TypeInfo next_machine_info = {
    .name = TYPE_NEXT_MACHINE,
    .parent = TYPE_MACHINE,
    .abstract = true,
    .instance_size = sizeof(NeXTState),
    .instance_init = next_machine_instance_init,
    .class_size = sizeof(NeXTMachineClass),
    .class_init = next_machine_class_init,
};

static void next_register_types(void)
{
    type_register_static(&next_machine_info);
    type_register_static(&next_pc_info);

    for (int i = 0; i < ARRAY_SIZE(next_models); i++) {
        TypeInfo ti = {
            .name = next_models[i].name,
            .parent = TYPE_NEXT_MACHINE,
            .class_init = next_model_class_init,
            .class_data = &next_models[i],
        };
        type_register_static(&ti);
    }
}

type_init(next_register_types)
