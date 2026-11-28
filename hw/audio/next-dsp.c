/*
 * NeXT DSP56001 and its host interface
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Every NeXT has a Motorola DSP56001 at 25 MHz with 8K words of its own
 * RAM (32K with the expansion), reached through its host port at
 * 0x02008000, with a DMA channel to memory. The DSP core in
 * hw/audio/dsp56k is ARAnyM's, as the Previous emulator has it; this is
 * the NeXT's side, from Previous's dsp.c (GPL-2.0-or-later).
 *
 * The DSP runs behind the CPU: a timer, and every access to the host
 * port, let it catch up with the virtual clock. NeXTSTEP leaves it running
 * a program that waits for the host in a loop of jclr and jset
 * instructions. Once the DSP comes back to the same instruction with all
 * its registers as they were, twice, only the host port or the DMA can
 * end that wait, so it stops there until one of them is used, instead of
 * spinning at 25 MHz.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/audio/next-dsp.h"
#include "hw/dma/next-dma.h"
#include "migration/vmstate.h"
#include "dsp56k/dsp56k_glue.h"
#include "dsp56k/dsp56k_core.h"
#include "dsp56k/dsp56k_cpu.h"

/* DSP clocks per second: the instruction cycle is two of them */
#define DSP_CLOCK_HZ        25000000
/* how often the timer lets the DSP catch up */
#define DSP_SLICE_NS        (100 * SCALE_US)
/* the most a DSP catches up at once, after the VM was stopped */
#define DSP_MAX_CLOCKS      (DSP_CLOCK_HZ / 100)

/* the core works on one DSP, this one */
static NeXTDSPState *next_dsp;

static void next_dsp_set_irq(NeXTDSPState *s, int n, bool level)
{
    qemu_set_irq(s->irq[n], level);
}

/* ---- the core's hooks ---- */

/* HREQ: an interrupt to the CPU, or a request to the DMA */
static void next_dsp_hreq(int set)
{
    NeXTDSPState *s = next_dsp;

    if (dsp_core.dma_mode) {
        dsp_core.dma_request = set;
        next_dsp_set_irq(s, NEXT_DSP_HREQ, false);
    } else {
        dsp_core.dma_request = 0;
        next_dsp_set_irq(s, NEXT_DSP_HREQ, set);
    }
}

void DSP_HandleTXD(int set)
{
    next_dsp_set_irq(next_dsp, NEXT_DSP_TXD, set);
}

void DSP_InitSSI(uint32_t value)
{
    /* the SSI goes to the sound input at 44.1 kHz, which is not here */
    if (value) {
        qemu_log_mask(LOG_UNIMP, "next-dsp: SSI sound input\n");
    }
}

void DSP_SsiTransmit_SC1(void)
{
}

void DSP_SsiTransmit_SC2(uint32_t frame)
{
}

/* ---- DMA ---- */

static bool next_dsp_dma_ready(NeXTDSPState *s)
{
    NeXTDMAChan *c = next_dma_chan(s->dma, NEXT_DMA_DSP);

    return next_dma_enabled(s->dma, NEXT_DMA_DSP) && c->next < c->limit;
}

/* At the end of a DMA buffer: IRQB to the DSP, if SCR2 asks for it */
static void next_dsp_dma_done(NeXTDSPState *s)
{
    NeXTDMAChan *c = next_dma_chan(s->dma, NEXT_DMA_DSP);

    if (c->next == c->limit) {
        if (s->block_end_irq) {
            dsp_set_interrupt(DSP_INTER_IRQB, 1);
        }
        next_dma_update(s->dma, NEXT_DMA_DSP);
    }
}

static uint8_t next_dsp_dma_read(NeXTDSPState *s)
{
    uint8_t val = 0;

    if (next_dsp_dma_ready(s)) {
        next_dma_read_mem(s->dma, NEXT_DMA_DSP, &val, 1);
        next_dsp_dma_done(s);
    }
    return val;
}

static void next_dsp_dma_write(NeXTDSPState *s, uint8_t val)
{
    if (next_dsp_dma_ready(s)) {
        next_dma_write_mem(s->dma, NEXT_DMA_DSP, &val, 1);
        next_dsp_dma_done(s);
    }
}

/* One byte between the host port and memory, if the DSP asks */
static void next_dsp_handle_dma(NeXTDSPState *s)
{
    if (!dsp_core.dma_mode || !dsp_core.dma_request ||
        !next_dsp_dma_ready(s)) {
        return;
    }
    if (dsp_core.dma_address_counter == 0) {
        dsp_core.dma_address_counter = 4 - dsp_core.dma_mode;
        /* unpacked: the Turbo moves all four bytes */
        if (s->unpacked && s->turbo) {
            dsp_core.dma_address_counter = 4;
        }
    }
    dsp_core.dma_address_counter--;

    if (dsp_core.dma_direction == (1 << CPU_HOST_ICR_TREQ)) {
        dsp_core_write_host(CPU_HOST_TRXL - dsp_core.dma_address_counter,
                            next_dsp_dma_read(s));
    } else {
        next_dsp_dma_write(s, dsp_core_read_host(CPU_HOST_TRXL -
                                                 dsp_core.dma_address_counter));
    }

    /* unpacked, but not Turbo: the high byte goes too */
    if (s->unpacked && dsp_core.dma_address_counter == 0 && !s->turbo) {
        if (dsp_core.dma_direction == (1 << CPU_HOST_ICR_TREQ)) {
            dsp_core_write_host(CPU_HOST_TRX0, next_dsp_dma_read(s));
        } else {
            next_dsp_dma_write(s, dsp_core_read_host(CPU_HOST_TRX0));
        }
    }
}

/* ---- running ---- */

/* The longest wait loop the DSP is caught in, in instructions */
#define DSP_WAIT_LOOP       8

/*
 * Whether the DSP is back where it was at the last snapshot, with the same
 * registers, and no DMA transfer is ready for it. A DO loop counts in LC,
 * one of the registers; a REP keeps the PC on itself as it counts.
 */
static bool next_dsp_waits(NeXTDSPState *s)
{
    return dsp_core.pc == s->wait_pc && !dsp_core.pc_on_rep &&
           !memcmp(dsp_core.registers, s->wait_regs, sizeof(s->wait_regs)) &&
           !(dsp_core.dma_mode && dsp_core.dma_request &&
             next_dsp_dma_ready(s));
}

static void next_dsp_wait_snapshot(NeXTDSPState *s)
{
    s->wait_pc = dsp_core.pc;
    memcpy(s->wait_regs, dsp_core.registers, sizeof(s->wait_regs));
    s->wait_len = 0;
}

static void next_dsp_run(NeXTDSPState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int same = 0;

    if (!dsp_core.running || s->parked) {
        s->clocks = 0;
        s->last_ns = now;
        return;
    }
    s->clocks += muldiv64(now - s->last_ns, DSP_CLOCK_HZ,
                          NANOSECONDS_PER_SECOND);
    s->last_ns = now;
    s->clocks = MIN(s->clocks, DSP_MAX_CLOCKS);

    next_dsp_wait_snapshot(s);
    while (s->clocks > 0 && dsp_core.running) {
        dsp56k_execute_instruction();
        s->clocks -= dsp_core.instr_cycle;
        next_dsp_handle_dma(s);
        /* twice round the same loop: it goes on until the host or the DMA */
        if (next_dsp_waits(s)) {
            same++;
            s->wait_len = 0;
        } else if (++s->wait_len == DSP_WAIT_LOOP) {
            same = 0;
            next_dsp_wait_snapshot(s);
        }
        if (same == 2) {
            s->parked = true;
            s->clocks = 0;
            timer_del(s->timer);
            break;
        }
    }
}

/* The host or the DMA did something the DSP may wait for */
static void next_dsp_wake(NeXTDSPState *s)
{
    if (s->parked) {
        s->parked = false;
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    if (dsp_core.running && !timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            DSP_SLICE_NS);
    }
}

static void next_dsp_timer(void *opaque)
{
    NeXTDSPState *s = opaque;

    next_dsp_run(s);
    if (dsp_core.running && !s->parked) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            DSP_SLICE_NS);
    }
}

static void next_dsp_dma_kick(void *opaque, NeXTDMAChannel ch)
{
    next_dsp_wake(opaque);
}

void next_dsp_set_running(NeXTDSPState *s, bool run, int mode)
{
    s->parked = false;
    if (run) {
        dsp_core_start(mode, 1);
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->clocks = 0;
        timer_mod(s->timer, s->last_ns + DSP_SLICE_NS);
    } else {
        DSP_HandleTXD(0);
        dsp_core_reset();
        timer_del(s->timer);
    }
}

void next_dsp_set_memory(NeXTDSPState *s, bool on)
{
    s->mem_enabled = on;
    dsp_core_config_ramext(on ? s->ram : NULL,
                           s->ram_expansion ? NEXT_DSP_RAM_WORDS_MAX
                                            : NEXT_DSP_RAM_WORDS);
}

void next_dsp_set_dma_options(NeXTDSPState *s, bool block_end_irq,
                              bool unpacked)
{
    s->block_end_irq = block_end_irq;
    s->unpacked = unpacked;
}

/* ---- the host port, byte by byte ---- */

static uint64_t next_dsp_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTDSPState *s = opaque;

    uint8_t val;

    next_dsp_run(s);
    val = dsp_core_read_host(addr & 7);
    /* reading the data may let the DSP send more */
    next_dsp_wake(s);
    return val;
}

static void next_dsp_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    NeXTDSPState *s = opaque;

    next_dsp_run(s);
    dsp_core_write_host(addr & 7, val);
    /* the DSP may have been waiting for this */
    next_dsp_wake(s);
}

static const MemoryRegionOps next_dsp_ops = {
    .read = next_dsp_read,
    .write = next_dsp_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ---- device ---- */

static void next_dsp_reset_hold(Object *obj, ResetType type)
{
    NeXTDSPState *s = NEXT_DSP(obj);

    s->block_end_irq = false;
    s->unpacked = false;
    next_dsp_set_running(s, false, 0);
    /* the board sets the memory from SCR2 */
}

static void next_dsp_realize(DeviceState *dev, Error **errp)
{
    NeXTDSPState *s = NEXT_DSP(dev);

    if (!s->dma) {
        error_setg(errp, "next-dsp needs the dma link");
        return;
    }
    if (next_dsp) {
        error_setg(errp, "a machine has only one next-dsp");
        return;
    }
    next_dsp = s;
    dsp56k_core_ptr = s->core;
    dsp_core_init(next_dsp_hreq);
    dsp56k_init_cpu();
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, next_dsp_timer, s);
    next_dma_set_client(s->dma, NEXT_DMA_DSP, next_dsp_dma_kick, NULL, s);
}

static void next_dsp_init(Object *obj)
{
    NeXTDSPState *s = NEXT_DSP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->core = g_new0(dsp_core_t, 1);
    memory_region_init_io(&s->mem, obj, &next_dsp_ops, s, "next.dsp",
                          0x2000);
    sysbus_init_mmio(sbd, &s->mem);
    for (int i = 0; i < NEXT_DSP_NUM_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static void next_dsp_finalize(Object *obj)
{
    NeXTDSPState *s = NEXT_DSP(obj);

    if (next_dsp == s) {
        next_dsp = NULL;
    }
    g_free(s->core);
}

static int next_dsp_post_load(void *opaque, int version_id)
{
    NeXTDSPState *s = opaque;

    /* the core keeps a pointer to the RAM, and its size */
    if (s->mem_enabled) {
        DSP_RAMSIZE = s->ram_expansion ? NEXT_DSP_RAM_WORDS_MAX
                                       : NEXT_DSP_RAM_WORDS;
        dsp_core.ramext = s->ram;
    } else {
        DSP_RAMSIZE = 0;
        dsp_core.ramext = NULL;
    }
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return 0;
}

static const VMStateDescription next_dsp_ssi_vmstate = {
    .name = "next-dsp/ssi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(cra_word_length, dsp_core_ssi_t),
        VMSTATE_UINT32(cra_word_mask, dsp_core_ssi_t),
        VMSTATE_UINT16(cra_frame_rate_divider, dsp_core_ssi_t),
        VMSTATE_UINT16(crb_src_clock, dsp_core_ssi_t),
        VMSTATE_UINT16(crb_shifter, dsp_core_ssi_t),
        VMSTATE_UINT16(crb_synchro, dsp_core_ssi_t),
        VMSTATE_UINT16(crb_mode, dsp_core_ssi_t),
        VMSTATE_UINT16(crb_te, dsp_core_ssi_t),
        VMSTATE_UINT16(crb_re, dsp_core_ssi_t),
        VMSTATE_UINT16(crb_tie, dsp_core_ssi_t),
        VMSTATE_UINT16(crb_rie, dsp_core_ssi_t),
        VMSTATE_UINT32(TX, dsp_core_ssi_t),
        VMSTATE_UINT32(RX, dsp_core_ssi_t),
        VMSTATE_UINT32(transmit_value, dsp_core_ssi_t),
        VMSTATE_UINT32(received_value, dsp_core_ssi_t),
        VMSTATE_UINT16(waitFrameTX, dsp_core_ssi_t),
        VMSTATE_UINT16(waitFrameRX, dsp_core_ssi_t),
        VMSTATE_UINT32(dspPlay_handshakeMode_frame, dsp_core_ssi_t),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription next_dsp_core_vmstate = {
    .name = "next-dsp/core",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(running, dsp_core_t),
        VMSTATE_INT32(dma_mode, dsp_core_t),
        VMSTATE_INT32(dma_direction, dsp_core_t),
        VMSTATE_INT32(dma_request, dsp_core_t),
        VMSTATE_INT32(dma_address_counter, dsp_core_t),
        VMSTATE_UINT16(instr_cycle, dsp_core_t),
        VMSTATE_UINT16(pc, dsp_core_t),
        VMSTATE_UINT32_ARRAY(registers, dsp_core_t, 64),
        VMSTATE_UINT16_2DARRAY(stack, dsp_core_t, 2, 16),
        VMSTATE_UINT32_2DARRAY(ramint, dsp_core_t, 3, 512),
        VMSTATE_UINT32_2DARRAY(periph, dsp_core_t, 2, 64),
        VMSTATE_UINT32(dsp_host_htx, dsp_core_t),
        VMSTATE_UINT32(dsp_host_rtx, dsp_core_t),
        VMSTATE_UINT16(dsp_host_isr_HREQ, dsp_core_t),
        VMSTATE_UINT8_ARRAY(hostport, dsp_core_t, 12),
        VMSTATE_STRUCT(ssi, dsp_core_t, 1, next_dsp_ssi_vmstate,
                       dsp_core_ssi_t),
        VMSTATE_UINT32(loop_rep, dsp_core_t),
        VMSTATE_UINT32(pc_on_rep, dsp_core_t),
        VMSTATE_UINT16(bootstrap_pos, dsp_core_t),
        VMSTATE_UINT32(mode, dsp_core_t),
        VMSTATE_UINT32(mode_wait, dsp_core_t),
        VMSTATE_UINT16(interrupt_state, dsp_core_t),
        VMSTATE_UINT16(interrupt_instr_fetch, dsp_core_t),
        VMSTATE_UINT16(interrupt_save_pc, dsp_core_t),
        VMSTATE_UINT16(interrupt_IplToRaise, dsp_core_t),
        VMSTATE_UINT16(interrupt_pipeline_count, dsp_core_t),
        VMSTATE_UINT32(interrupt_status, dsp_core_t),
        VMSTATE_UINT32(interrupt_enable, dsp_core_t),
        VMSTATE_UINT32(interrupt_mask, dsp_core_t),
        VMSTATE_UINT32_ARRAY(interrupt_mask_level, dsp_core_t, 3),
        VMSTATE_UINT32(interrupt_edgetriggered_mask, dsp_core_t),
        VMSTATE_UINT16(agu_move_indirect_instr, dsp_core_t),
        VMSTATE_UINT16_ARRAY(agu_pipeline_reg, dsp_core_t, 2),
        VMSTATE_UINT16_ARRAY(agu_pipeline_val, dsp_core_t, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription next_dsp_vmstate = {
    .name = "next-dsp",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = next_dsp_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(core, NeXTDSPState, next_dsp_core_vmstate,
                               dsp_core_t),
        VMSTATE_UINT32_ARRAY(ram, NeXTDSPState, NEXT_DSP_RAM_WORDS_MAX),
        VMSTATE_BOOL(mem_enabled, NeXTDSPState),
        VMSTATE_BOOL(block_end_irq, NeXTDSPState),
        VMSTATE_BOOL(unpacked, NeXTDSPState),
        VMSTATE_INT64(clocks, NeXTDSPState),
        VMSTATE_TIMER_PTR(timer, NeXTDSPState),
        VMSTATE_BOOL(parked, NeXTDSPState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property next_dsp_properties[] = {
    DEFINE_PROP_LINK("dma", NeXTDSPState, dma, TYPE_NEXT_DMA, NeXTDMAState *),
    DEFINE_PROP_BOOL("turbo", NeXTDSPState, turbo, false),
    DEFINE_PROP_BOOL("ram-expansion", NeXTDSPState, ram_expansion, false),
};

static void next_dsp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT DSP56001";
    dc->realize = next_dsp_realize;
    dc->vmsd = &next_dsp_vmstate;
    device_class_set_props(dc, next_dsp_properties);
    rc->phases.hold = next_dsp_reset_hold;
}

static const TypeInfo next_dsp_info = {
    .name = TYPE_NEXT_DSP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTDSPState),
    .instance_init = next_dsp_init,
    .instance_finalize = next_dsp_finalize,
    .class_init = next_dsp_class_init,
};

static void next_dsp_register_types(void)
{
    type_register_static(&next_dsp_info);
}

type_init(next_dsp_register_types)
