/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Western Digital WD8003E: an 8-bit ISA Ethernet card with a DP8390
 * NIC and 8 KB of shared memory, as the SVR4 wdn driver (Dell UNIX)
 * and the other WD/SMC 80x3 drivers know it.
 *
 * The board has no interface chip. Its I/O registers 0-7 read back
 * the station address PROM at 8-15 (register aliasing), which tells
 * the drivers that it is an old jumpered 8-bit board. Register 0,
 * written, is the memory select register: bit 7 resets the board,
 * bit 6 turns the shared memory on, bits 0-5 are address bits A13-A18
 * (A19 is always set). The PROM holds the station address (drivers
 * look for WD's 00:00:c0), the board ID byte (0x03, WD8003E) and a
 * checksum byte that makes PROM bytes 0-7 add up to 0xff.
 *
 * The DP8390 is at registers 0x10-0x1f. The NIC sees the shared memory
 * at local address 0 and the host sees it at the memory select address;
 * there is no remote DMA data port. The register logic follows QEMU's
 * ne2000.c (Fabrice Bellard), with the DP8390 parts NE2000 drivers
 * don't need: the station address filter on PAR0-5, page 2 read back
 * (drivers tell the DP8390 from the 83C690 by TCR), monitor mode,
 * loopback, the received CRC and flow control when the ring is full.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/isa/isa.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "net/eth.h"
#include "system/address-spaces.h"
#include "system/runstate.h"
#include "qom/object.h"
#include "trace.h"
#include <zlib.h>

#define TYPE_WD8003 "wd8003"
OBJECT_DECLARE_SIMPLE_TYPE(WD8003State, WD8003)

#define WD_RAM_SIZE     (8 * KiB)
#define WD_ID_8003E     0x03
#define WD_MAX_FRAME    1518

/* memory select register */
#define WD_MSR_RESET    0x80
#define WD_MSR_MENB     0x40
#define WD_MSR_ADDR     0x3f

/* DP8390 command register */
#define CR_STP          0x01
#define CR_STA          0x02
#define CR_TXP          0x04
#define CR_RD           0x38
#define CR_RD_NONE      0x20
#define CR_PS           0xc0

/* interrupt status and mask */
#define ISR_PRX         0x01
#define ISR_PTX         0x02
#define ISR_RXE         0x04
#define ISR_TXE         0x08
#define ISR_OVW         0x10
#define ISR_CNT         0x20
#define ISR_RDC         0x40
#define ISR_RST         0x80

/* receive configuration */
#define RCR_SEP         0x01
#define RCR_AR          0x02
#define RCR_AB          0x04
#define RCR_AM          0x08
#define RCR_PRO         0x10
#define RCR_MON         0x20

/* receive status */
#define RSR_PRX         0x01
#define RSR_MPA         0x10
#define RSR_PHY         0x20

/* transmit status and configuration */
#define TSR_PTX         0x01
#define TSR_ND          0x02
#define TCR_LB          0x06

struct WD8003State {
    ISADevice parent_obj;

    uint32_t iobase;
    uint32_t isairq;
    QEMUTimer *rx_timer;
    int64_t rx_next;        /* QEMU_CLOCK_VIRTUAL: the wire is free again */
    MemoryRegion io;
    MemoryRegion shmem;
    qemu_irq irq;
    NICState *nic;
    NICConf conf;
    uint8_t prom[8];

    uint8_t msr;
    uint8_t cr;
    uint8_t pstart;
    uint8_t pstop;
    uint8_t bnry;
    uint8_t tpsr;
    uint16_t tbcr;
    uint8_t tsr;
    uint8_t ncr;
    uint8_t isr;
    uint16_t rsar;
    uint16_t rbcr;
    uint8_t rsr;
    uint8_t rcr;
    uint8_t tcr;
    uint8_t dcr;
    uint8_t imr;
    uint8_t par[6];
    uint8_t curr;
    uint8_t mar[8];
    uint8_t cntr[3];
    uint8_t ram[WD_RAM_SIZE];
};

static void wd_update_irq(WD8003State *s)
{
    qemu_set_irq(s->irq, (s->isr & s->imr & 0x7f) != 0);
}

/* The NIC addresses only the 8 KB on the board; higher bits wrap. */
static uint8_t *wd_ram(WD8003State *s, unsigned addr)
{
    return &s->ram[addr & (WD_RAM_SIZE - 1)];
}

static void wd_map_ram(WD8003State *s)
{
    hwaddr base = 0x80000 | (hwaddr)(s->msr & WD_MSR_ADDR) << 13;

    memory_region_transaction_begin();
    memory_region_set_address(&s->shmem, base);
    memory_region_set_enabled(&s->shmem, s->msr & WD_MSR_MENB);
    memory_region_transaction_commit();
}

static void wd_nic_reset(WD8003State *s)
{
    s->cr = CR_RD_NONE | CR_STP;
    s->isr = ISR_RST;
    s->imr = 0;
    s->tsr = 0;
    s->rcr = 0;
    s->tcr = 0;
    s->dcr = 0;
    wd_update_irq(s);
}

/* Free bytes in the receive ring, from the current page up to BNRY. */
static unsigned wd_ring_free(WD8003State *s)
{
    unsigned start = s->pstart << 8, stop = s->pstop << 8;
    unsigned curr = s->curr << 8, bnry = s->bnry << 8;

    if (stop <= start) {
        return 0;
    }
    if (curr < bnry) {
        return bnry - curr;
    }
    return (stop - start) - (curr - bnry);
}

static bool wd_ring_full(WD8003State *s)
{
    /*
     * Room for the largest frame (the header, the frame and its CRC,
     * rounded up to pages) and one page more. The DP8390 never stores
     * into the BNRY page, so a frame never takes CURR up to BNRY, where
     * the ring would look empty.
     */
    return wd_ring_free(s) <= ((4 + WD_MAX_FRAME + 255) & ~255);
}

static bool wd_accept(WD8003State *s, const uint8_t *buf)
{
    unsigned idx;

    if (s->rcr & RCR_PRO) {
        return true;
    }
    if (is_broadcast_ether_addr(buf)) {
        return s->rcr & RCR_AB;
    }
    if (is_multicast_ether_addr(buf)) {
        if (!(s->rcr & RCR_AM)) {
            return false;
        }
        idx = net_crc32(buf, ETH_ALEN) >> 26;
        return s->mar[idx >> 3] & (1 << (idx & 7));
    }
    return !memcmp(buf, s->par, 6);
}

/*
 * Store a frame in the receive ring: 4 header bytes, the frame, its CRC.
 * The byte count in the header is the frame and its CRC, without the
 * header: 1518 for the largest frame. Drivers drop a frame with a
 * larger count: when the count also had the header, the drivers of Dell
 * UNIX (wdn) and SINIX-Z (wd) lost every frame over 1510 bytes, such as
 * full TCP segments and NFS fragments.
 */
static void wd_store(WD8003State *s, const uint8_t *buf, size_t size)
{
    uint8_t frame[WD_MAX_FRAME];
    unsigned start = s->pstart << 8, stop = s->pstop << 8;
    unsigned index, count, next, len, off;
    uint32_t crc;

    size = MIN(size, WD_MAX_FRAME - 4);
    memcpy(frame, buf, size);
    if (size < 60) {
        memset(frame + size, 0, 60 - size);
        size = 60;
    }
    crc = crc32(0, frame, size);
    stl_le_p(frame + size, crc);
    size += 4;

    index = s->curr << 8;
    if (index < start || index >= stop) {
        index = start;
    }
    count = size;
    next = index + ((4 + size + 255) & ~255);
    if (next >= stop) {
        next -= stop - start;
    }

    s->rsr = RSR_PRX | (is_multicast_ether_addr(frame) ? RSR_PHY : 0);
    *wd_ram(s, index) = s->rsr;
    *wd_ram(s, index + 1) = next >> 8;
    *wd_ram(s, index + 2) = count;
    *wd_ram(s, index + 3) = count >> 8;
    index += 4;
    for (off = 0; off < size; off += len) {
        if (index >= stop) {
            index = start;
        }
        len = MIN(size - off, stop - index);
        len = MIN(len, WD_RAM_SIZE - (index & (WD_RAM_SIZE - 1)));
        memcpy(wd_ram(s, index), frame + off, len);
        index += len;
    }
    s->curr = next >> 8;
    s->isr |= ISR_PRX;
    trace_wd8003_receive(size, s->curr, s->bnry);
    wd_update_irq(s);
}

/* A frame's time on a 10 Mbit/s wire: preamble, frame, CRC and gap. */
static int64_t wd_wire_ns(size_t size)
{
    size = MIN(MAX(size, 60), WD_MAX_FRAME - 4);
    return (size + 8 + 4 + 12) * 800;
}

/*
 * QEMU's network backends hand over a burst of frames at once, and each
 * BNRY write lets the next waiting frame into the ring, so the driver got
 * a whole burst in one receive interrupt. Dell UNIX then lost the second
 * of two 8 KB NFS replies (12 frames), although its driver read every
 * frame. So frames come no faster than on the wire: about 1.2 ms for a
 * full frame. A stopped VM gets the waiting frames at once, and a wait
 * longer than one frame time means the clock went back (loadvm).
 */
static bool wd_on_wire(WD8003State *s, int64_t now)
{
    return runstate_is_running() && now < s->rx_next &&
           s->rx_next - now <= wd_wire_ns(WD_MAX_FRAME - 4);
}

static void wd_rx_timer(void *opaque)
{
    WD8003State *s = opaque;

    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

static bool wd_can_receive(NetClientState *nc)
{
    WD8003State *s = qemu_get_nic_opaque(nc);

    /* a stopped NIC drops frames; a full ring holds them back */
    if ((s->cr & CR_STP) || (s->rcr & RCR_MON)) {
        return true;
    }
    return !wd_ring_full(s);
}

static ssize_t wd_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    WD8003State *s = qemu_get_nic_opaque(nc);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if ((s->cr & CR_STP) || (s->tcr & TCR_LB) || size < 6 ||
        !wd_accept(s, buf)) {
        return size;
    }
    if (s->rcr & RCR_MON) {
        return size;
    }
    if (wd_on_wire(s, now)) {
        /* the timer flushes QEMU's queue when the wire is free */
        timer_mod(s->rx_timer, s->rx_next);
        return 0;
    }
    if (wd_ring_full(s)) {
        /*
         * The frame waits in QEMU's queue until the driver frees pages:
         * a BNRY write flushes the queue. Nothing is lost, so no overflow
         * warning and no missed packet. Dell UNIX's wdn driver answers
         * OVW by restarting the NIC without reading the ring, so an OVW
         * for a waiting frame left the frames in the ring unread, and
         * with no room for another frame, no PRX came again.
         */
        return 0;
    }
    wd_store(s, buf, size);
    s->rx_next = now + wd_wire_ns(size);
    return size;
}

static void wd_transmit(WD8003State *s)
{
    uint8_t frame[WD_MAX_FRAME];
    unsigned len = MIN(s->tbcr, WD_MAX_FRAME), base = s->tpsr << 8, i;

    for (i = 0; i < len; i++) {
        frame[i] = *wd_ram(s, base + i);
    }
    trace_wd8003_transmit(len, s->tpsr);
    if (s->tcr & TCR_LB) {
        /* loopback: the frame comes back to this NIC, not to the wire */
        if (len >= 6 && !(s->cr & CR_STP) && !(s->rcr & RCR_MON) &&
            wd_accept(s, frame) && !wd_ring_full(s)) {
            wd_store(s, frame, len);
        }
    } else {
        qemu_send_packet(qemu_get_queue(s->nic), frame, len);
    }
    s->tsr = TSR_PTX | TSR_ND;
    s->ncr = 0;
    s->isr |= ISR_PTX;
    wd_update_irq(s);
}

static void wd_write_cr(WD8003State *s, uint8_t val)
{
    bool was_stopped = s->cr & CR_STP;

    s->cr = (val & ~CR_TXP) | (s->cr & CR_TXP);
    if (val & CR_STP) {
        s->cr = (s->cr & ~CR_STA) | CR_STP;
        s->isr |= ISR_RST;
    } else if (val & CR_STA) {
        s->cr &= ~CR_STP;
        s->isr &= ~ISR_RST;
    }
    /* remote DMA with a zero byte count completes at once */
    if ((val & CR_RD) && (val & CR_RD) != CR_RD_NONE && s->rbcr == 0) {
        s->isr |= ISR_RDC;
    }
    if ((val & CR_TXP) && !(s->cr & CR_STP)) {
        wd_transmit(s);
    }
    s->cr &= ~CR_TXP;
    wd_update_irq(s);
    if (was_stopped && !(s->cr & CR_STP)) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void wd_nic_write(WD8003State *s, unsigned reg, uint8_t val)
{
    if (reg == 0) {
        wd_write_cr(s, val);
        return;
    }
    switch ((s->cr & CR_PS) >> 2 | reg) {
    case 0x01:
        s->pstart = val;
        break;
    case 0x02:
        s->pstop = val;
        break;
    case 0x03:
        s->bnry = val;
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        break;
    case 0x04:
        s->tpsr = val;
        break;
    case 0x05:
        s->tbcr = (s->tbcr & 0xff00) | val;
        break;
    case 0x06:
        s->tbcr = (s->tbcr & 0x00ff) | val << 8;
        break;
    case 0x07:
        s->isr &= ~(val & 0x7f);
        wd_update_irq(s);
        break;
    case 0x08:
        s->rsar = (s->rsar & 0xff00) | val;
        break;
    case 0x09:
        s->rsar = (s->rsar & 0x00ff) | val << 8;
        break;
    case 0x0a:
        s->rbcr = (s->rbcr & 0xff00) | val;
        break;
    case 0x0b:
        s->rbcr = (s->rbcr & 0x00ff) | val << 8;
        break;
    case 0x0c:
        s->rcr = val & 0x3f;
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        break;
    case 0x0d:
        s->tcr = val & 0x1f;
        break;
    case 0x0e:
        s->dcr = val & 0x7f;
        break;
    case 0x0f:
        s->imr = val & 0x7f;
        wd_update_irq(s);
        break;
    case 0x11 ... 0x16:
        s->par[reg - 1] = val;
        break;
    case 0x17:
        s->curr = val;
        break;
    case 0x18 ... 0x1f:
        s->mar[reg - 8] = val;
        break;
    default:
        /* page 2 local DMA and next packet pointers: tests only */
        break;
    }
}

static uint8_t wd_nic_read(WD8003State *s, unsigned reg)
{
    uint8_t val;

    if (reg == 0) {
        return s->cr;
    }
    switch ((s->cr & CR_PS) >> 2 | reg) {
    case 0x01:
        return 0;
    case 0x02:
        return s->curr;
    case 0x03:
        return s->bnry;
    case 0x04:
        return s->tsr;
    case 0x05:
        return s->ncr;
    case 0x06:
        return 0;
    case 0x07:
        return s->isr;
    case 0x08:
        return s->rsar;
    case 0x09:
        return s->rsar >> 8;
    case 0x0c:
        return s->rsr;
    case 0x0d ... 0x0f:
        /* tally counters clear when read */
        val = s->cntr[reg - 0x0d];
        s->cntr[reg - 0x0d] = 0;
        return val;
    case 0x11 ... 0x16:
        return s->par[reg - 1];
    case 0x17:
        return s->curr;
    case 0x18 ... 0x1f:
        return s->mar[reg - 8];
    case 0x21:
        return s->pstart;
    case 0x22:
        return s->pstop;
    case 0x23:
        return s->bnry;
    case 0x24:
        return s->tpsr;
    case 0x25:
        return s->curr;
    case 0x26:
    case 0x27:
        return 0;
    case 0x2c:
        return s->rcr | 0xc0;
    case 0x2d:
        return s->tcr | 0xe0;
    case 0x2e:
        return s->dcr | 0x80;
    case 0x2f:
        return s->imr | 0x80;
    default:
        return 0xff;
    }
}

static uint64_t wd_io_read(void *opaque, hwaddr addr, unsigned size)
{
    WD8003State *s = opaque;
    uint8_t val;

    if (addr < 0x10) {
        val = s->prom[addr & 7];
    } else {
        val = wd_nic_read(s, addr & 0xf);
    }
    trace_wd8003_io_read(addr, val);
    return val;
}

static void wd_io_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    WD8003State *s = opaque;

    trace_wd8003_io_write(addr, val);
    if (addr >= 0x10) {
        wd_nic_write(s, addr & 0xf, val);
    } else if (addr == 0) {
        if (val & WD_MSR_RESET) {
            wd_nic_reset(s);
        }
        s->msr = val;
        wd_map_ram(s);
    }
}

static const MemoryRegionOps wd_io_ops = {
    .read = wd_io_read,
    .write = wd_io_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t wd_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    WD8003State *s = opaque;

    return ldn_le_p(&s->ram[addr], size);
}

static void wd_mem_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    WD8003State *s = opaque;

    stn_le_p(&s->ram[addr], size, val);
}

static const MemoryRegionOps wd_mem_ops = {
    .read = wd_mem_read,
    .write = wd_mem_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
    .valid.unaligned = true,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int wd_post_load(void *opaque, int version_id)
{
    wd_map_ram(opaque);
    return 0;
}

static const VMStateDescription vmstate_wd8003 = {
    .name = TYPE_WD8003,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = wd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(msr, WD8003State),
        VMSTATE_UINT8(cr, WD8003State),
        VMSTATE_UINT8(pstart, WD8003State),
        VMSTATE_UINT8(pstop, WD8003State),
        VMSTATE_UINT8(bnry, WD8003State),
        VMSTATE_UINT8(tpsr, WD8003State),
        VMSTATE_UINT16(tbcr, WD8003State),
        VMSTATE_UINT8(tsr, WD8003State),
        VMSTATE_UINT8(ncr, WD8003State),
        VMSTATE_UINT8(isr, WD8003State),
        VMSTATE_UINT16(rsar, WD8003State),
        VMSTATE_UINT16(rbcr, WD8003State),
        VMSTATE_UINT8(rsr, WD8003State),
        VMSTATE_UINT8(rcr, WD8003State),
        VMSTATE_UINT8(tcr, WD8003State),
        VMSTATE_UINT8(dcr, WD8003State),
        VMSTATE_UINT8(imr, WD8003State),
        VMSTATE_BUFFER(par, WD8003State),
        VMSTATE_UINT8(curr, WD8003State),
        VMSTATE_BUFFER(mar, WD8003State),
        VMSTATE_BUFFER(cntr, WD8003State),
        VMSTATE_BUFFER(ram, WD8003State),
        VMSTATE_END_OF_LIST()
    }
};

static NetClientInfo net_wd8003_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = wd_can_receive,
    .receive = wd_receive,
};

static void wd_reset(DeviceState *dev)
{
    WD8003State *s = WD8003(dev);

    s->msr = 0;
    wd_map_ram(s);
    wd_nic_reset(s);
    timer_del(s->rx_timer);
    s->rx_next = 0;
}

static void wd_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isa = ISA_DEVICE(dev);
    WD8003State *s = WD8003(dev);
    static const uint8_t zero[6];
    bool own_mac = memcmp(s->conf.macaddr.a, zero, 6);
    uint8_t sum = 0;
    int i;

    if (s->iobase & 0x1f || s->iobase > 0x3e0) {
        error_setg(errp, "wd8003: iobase must be a multiple of 0x20");
        return;
    }
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    if (!own_mac) {
        /* WD's OUI: the drivers look for it in the PROM */
        s->conf.macaddr.a[0] = 0x00;
        s->conf.macaddr.a[1] = 0x00;
        s->conf.macaddr.a[2] = 0xc0;
    }
    memcpy(s->prom, s->conf.macaddr.a, 6);
    s->prom[6] = WD_ID_8003E;
    for (i = 0; i < 7; i++) {
        sum += s->prom[i];
    }
    s->prom[7] = 0xff - sum;

    memory_region_init_io(&s->io, OBJECT(dev), &wd_io_ops, s, "wd8003", 0x20);
    isa_register_ioport(isa, &s->io, s->iobase);
    /*
     * The BIOS leaves shadow RAM on over the upper memory blocks it does
     * not use (the i440FX PAM), and the PC's option ROM RAM covers
     * 0xc0000-0xdffff, so the board's memory goes on top of both.
     */
    memory_region_init_io(&s->shmem, OBJECT(dev), &wd_mem_ops, s,
                          "wd8003-ram", WD_RAM_SIZE);
    memory_region_set_enabled(&s->shmem, false);
    memory_region_add_subregion_overlap(get_system_memory(), 0xd0000,
                                        &s->shmem, 2);
    s->irq = isa_get_irq(isa, s->isairq);
    s->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wd_rx_timer, s);

    s->nic = qemu_new_nic(&net_wd8003_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const Property wd_properties[] = {
    DEFINE_PROP_UINT32("iobase", WD8003State, iobase, 0x280),
    DEFINE_PROP_UINT32("irq", WD8003State, isairq, 3),
    DEFINE_NIC_PROPERTIES(WD8003State, conf),
};

static void wd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = wd_realize;
    dc->desc = "Western Digital WD8003E Ethernet (DP8390, 8 KB shared memory)";
    device_class_set_legacy_reset(dc, wd_reset);
    device_class_set_props(dc, wd_properties);
    dc->vmsd = &vmstate_wd8003;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo wd_info = {
    .name = TYPE_WD8003,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(WD8003State),
    .class_init = wd_class_init,
};

static void wd_register_types(void)
{
    type_register_static(&wd_info);
}

type_init(wd_register_types)
