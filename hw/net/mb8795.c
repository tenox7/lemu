/*
 * Fujitsu MB8795 Ethernet controller of the NeXT computers, and the
 * AT&T 7213 based controller of the Turbo machines
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the Previous emulator's ethernet.c and dma.c
 * (GPL-2.0-or-later).
 *
 * The controller has no buffers of its own worth speaking of: a timer
 * moves a received packet into memory through the receive DMA channel,
 * and takes a packet to send from the transmit channel, whose Limit has
 * the end-of-packet flag on the last buffer.
 *
 * On thin wire Ethernet the machine hears its own packets; with
 * loopback set, or with no network, packets come straight back.
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
#include "hw/dma/next-dma.h"
#include "hw/net/mb8795.h"
#include "migration/vmstate.h"
#include "net/net.h"

#define TXSTAT_READY        0x80
#define TXSTAT_NET_BUSY     0x40
#define TXSTAT_TX_RECVD     0x20
#define TXSTAT_SHORTED      0x10
#define TXSTAT_UNDERFLOW    0x08
#define TXSTAT_COLL         0x04
#define TXSTAT_16COLLS      0x02
#define TXSTAT_PAR_ERR      0x01

#define RXSTAT_PKT_OK       0x80
#define RXSTAT_RESET_PKT    0x10
#define RXSTAT_SHORT_PKT    0x08
#define RXSTAT_ALIGN_ERR    0x04
#define RXSTAT_CRC_ERR      0x02
#define RXSTAT_OVERFLOW     0x01

#define TXMODE_DIS_LOOP     0x02

#define RXMODE_ADDR_SIZE    0x10
#define RXMODE_ENA_SHORT    0x08
#define RXMODE_MATCH_MODE   0x03

#define EN_RESET            0x80

/* MB8795 address match modes */
#define RX_NOPACKETS        0
#define RX_LIMITED          1
#define RX_NORMAL           2
#define RX_PROMISCUOUS      3

/* Turbo */
#define RX_ENABLED          0x80
#define RX_ANY              0x01
#define RX_OWN              0x02
#define TXMODE_ENABLE       0x80
#define RXMODE_ENABLE       0x80
#define TXMODE_LOOP         0x02
#define TXMODE_TPE          0x04
#define ENCTRL_BADTPE       0x40

#define ENET_FRAMESIZE_MIN  64

#define ENET_IO_DELAY       500     /* us */
#define ENET_IO_SHORT       40
#define ENET_NS_PER_BYTE    800     /* 10 Mbit/s */
/*
 * QEMU runs the CPU faster than a 25 MHz 68040, and the ROM's self test
 * polls for its looped back packet only so many times: the controller
 * starts as soon as the CPU gives a DMA channel work.
 */

enum {
    EN_DISCONNECTED,
    EN_LOOPBACK,
    EN_THINWIRE,
    EN_TWISTEDPAIR,
};

enum {
    RECV_STATE_WAITING,
    RECV_STATE_RECEIVING,
};

static bool mb8795_connected(MB8795State *s)
{
    NetClientState *nc = qemu_get_queue(s->nic);

    return nc->peer != NULL && !nc->link_down;
}

static void mb8795_update_irqs(MB8795State *s)
{
    qemu_set_irq(s->tx_irq, (s->tx_status & s->tx_mask) != 0);
    qemu_set_irq(s->rx_irq, (s->rx_status & s->rx_mask) != 0);
}

static void mb8795_tx_interrupt(MB8795State *s, uint8_t intr)
{
    s->tx_status |= intr;
    mb8795_update_irqs(s);
}

static void mb8795_tx_release(MB8795State *s, uint8_t intr)
{
    s->tx_status &= ~intr;
    mb8795_update_irqs(s);
}

static void mb8795_rx_interrupt(MB8795State *s, uint8_t intr)
{
    s->rx_status |= intr;
    mb8795_update_irqs(s);
}

static void mb8795_dma_kick(void *opaque, NeXTDMAChannel ch);

/* ---- address matching ---- */

static bool recv_multicast(const uint8_t *p)
{
    return p[0] & 0x01;
}

static bool recv_local_multicast(MB8795State *s, const uint8_t *p)
{
    return (p[0] & 0x01) && (p[0] & 0xFE) == s->mac[0] &&
           p[1] == s->mac[1] && p[2] == s->mac[2];
}

static bool recv_me(MB8795State *s, const uint8_t *p, bool any_last)
{
    return !memcmp(p, s->mac, 5) && (p[5] == s->mac[5] || any_last);
}

static bool recv_broadcast(const uint8_t *p)
{
    static const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    return !memcmp(p, bc, 6);
}

static bool mb8795_for_me(MB8795State *s, const uint8_t *p)
{
    if (s->turbo) {
        if (!(s->rx_mode & RX_ENABLED)) {
            return false;
        }
        if (s->rx_mode & RX_ANY) {
            return true;
        }
        if (s->rx_mode & RX_OWN) {
            return recv_broadcast(p) || recv_me(s, p, false);
        }
        return recv_broadcast(p);
    }
    switch (s->rx_mode & RXMODE_MATCH_MODE) {
    case RX_LIMITED:
        return recv_broadcast(p) ||
               recv_me(s, p, s->rx_mode & RXMODE_ADDR_SIZE) ||
               recv_local_multicast(s, p);
    case RX_NORMAL:
        return recv_broadcast(p) ||
               recv_me(s, p, s->rx_mode & RXMODE_ADDR_SIZE) ||
               recv_multicast(p);
    case RX_PROMISCUOUS:
        return true;
    default:
        return false;
    }
}

/* Put a packet in the receive buffer, as the wire would */
static void mb8795_take_packet(MB8795State *s, const uint8_t *pkt, int len)
{
    if (len < 6 || !mb8795_for_me(s, pkt)) {
        return;
    }
    len = MIN(len, MB8795_BUF_SIZE - 4);
    memcpy(s->rx_buf, pkt, len);
    len += 4;   /* CRC */
    if (len < ENET_FRAMESIZE_MIN) {
        len = ENET_FRAMESIZE_MIN;
    }
    s->rx_size = s->rx_limit = len;
    s->tx_status |= TXSTAT_NET_BUSY;
    if (!(s->reset & EN_RESET) && !s->in_tick) {
        mb8795_dma_kick(s, NEXT_DMA_EN_RX);
    }
}

/* ---- state of the wire ---- */

static int mb8795_wire(MB8795State *s)
{
    bool up = mb8795_connected(s);

    if (s->turbo) {
        if (s->tx_mode & TXMODE_LOOP) {
            return EN_LOOPBACK;
        }
        if (up) {
            if (s->tx_mode & TXMODE_TPE) {
                if (s->twisted_pair) {
                    return EN_TWISTEDPAIR;
                }
            } else if (!s->twisted_pair) {
                return EN_THINWIRE;
            }
        }
        return EN_DISCONNECTED;
    }
    if (!s->no_tpe && s->tpe_select) {
        if (up && s->twisted_pair) {
            return EN_TWISTEDPAIR;
        }
    } else if (s->tx_mode & TXMODE_DIS_LOOP) {
        if (up && (s->no_tpe || !s->twisted_pair)) {
            return EN_THINWIRE;
        }
    } else {
        return EN_LOOPBACK;
    }
    return EN_DISCONNECTED;
}

/* ---- BOOTP for NeXT clients ---- */

#define BOOTP_SERVER_PORT   67
#define BOOTP_CLIENT_PORT   68
#define BOOTP_VEND          236     /* vendor area in the BOOTP packet */
#define NEXT_VEND_LEN       64

static const uint8_t next_vend_magic[4] = { 'N', 'e', 'X', 'T' };
static const uint8_t rfc1048_magic[4] = { 0x63, 0x82, 0x53, 0x63 };

/*
 * Find the BOOTP packet in an Ethernet frame going from port sport to
 * port dport, and return where it starts, or NULL.
 */
static uint8_t *mb8795_bootp(uint8_t *pkt, size_t len, uint16_t sport,
                             uint16_t dport)
{
    size_t ihl;
    uint8_t *udp;

    if (len < 14 + 20 || lduw_be_p(pkt + 12) != 0x0800 ||
        (pkt[14] >> 4) != 4 || pkt[14 + 9] != 17) {
        return NULL;
    }
    ihl = (pkt[14] & 0xF) * 4;
    if (ihl < 20 || len < 14 + ihl + 8 + BOOTP_VEND + NEXT_VEND_LEN) {
        return NULL;
    }
    udp = pkt + 14 + ihl;
    if (lduw_be_p(udp) != sport || lduw_be_p(udp + 2) != dport) {
        return NULL;
    }
    return udp + 8;
}

/*
 * NeXTSTEP asks for its address with BOOTP and the vendor magic "NeXT",
 * and takes only an answer in the same form. QEMU's user network answers
 * with DHCP options instead, so put a NeXT vendor area in their place:
 * version 1, opcode 0 (it worked) and no text. Previous's own BOOTP
 * server answers the same way.
 */
static void mb8795_note_bootp(MB8795State *s, uint8_t *pkt, size_t len)
{
    uint8_t *bp = mb8795_bootp(pkt, len, BOOTP_CLIENT_PORT,
                               BOOTP_SERVER_PORT);

    if (bp && bp[0] == 1 &&
        !memcmp(bp + BOOTP_VEND, next_vend_magic, 4)) {
        s->bootp_pending = true;
        s->bootp_xid = ldl_be_p(bp + 4);
    }
}

/* Returns the new length of the packet */
static size_t mb8795_fix_bootp(MB8795State *s, uint8_t *pkt, size_t len)
{
    uint8_t *bp, *ip = pkt + 14;
    size_t ihl = (ip[0] & 0xF) * 4;
    uint32_t sum = 0;

    if (!s->bootp_pending) {
        return len;
    }
    bp = mb8795_bootp(pkt, len, BOOTP_SERVER_PORT, BOOTP_CLIENT_PORT);
    if (!bp || bp[0] != 2 || ldl_be_p(bp + 4) != s->bootp_xid ||
        memcmp(bp + BOOTP_VEND, rfc1048_magic, 4)) {
        return len;
    }
    /* a BOOTP packet of the old size, 300 bytes */
    memset(bp + BOOTP_VEND, 0, NEXT_VEND_LEN);
    memcpy(bp + BOOTP_VEND, next_vend_magic, 4);
    bp[BOOTP_VEND + 4] = 1;     /* version */
    stw_be_p(bp - 8 + 4, 8 + BOOTP_VEND + NEXT_VEND_LEN);   /* UDP length */
    stw_be_p(bp - 8 + 6, 0);    /* no UDP checksum */
    stw_be_p(ip + 2, ihl + 8 + BOOTP_VEND + NEXT_VEND_LEN);
    stw_be_p(ip + 10, 0);
    for (size_t i = 0; i < ihl; i += 2) {
        sum += lduw_be_p(ip + i);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    stw_be_p(ip + 10, ~sum);
    return 14 + ihl + 8 + BOOTP_VEND + NEXT_VEND_LEN;
}

static void mb8795_send(MB8795State *s, int wire)
{
    mb8795_note_bootp(s, s->tx_buf, s->tx_size);
    if (wire == EN_LOOPBACK) {
        mb8795_take_packet(s, s->tx_buf, s->tx_size);
    } else {
        /*
         * On thin wire the machine hears its own packet as it goes out,
         * before any answer: an answer can come back at once, from inside
         * qemu_send_packet(), and then waits in the queue.
         */
        if (wire == EN_THINWIRE && !s->rx_size) {
            mb8795_take_packet(s, s->tx_buf, s->tx_size);
        }
        s->sending = true;
        qemu_send_packet(qemu_get_queue(s->nic), s->tx_buf, s->tx_size);
        s->sending = false;
    }
}

/* ---- DMA: Previous's dma_enet_write_memory() and dma_enet_read_memory() */

static void mb8795_dma_rx(MB8795State *s, bool eop)
{
    NeXTDMAState *dma = s->dma;
    NeXTDMAChan *c = next_dma_chan(dma, NEXT_DMA_EN_RX);

    if (!next_dma_enabled(dma, NEXT_DMA_EN_RX)) {
        return;
    }
    if ((c->limit % NEXT_DMA_BURST) || (c->next % NEXT_DMA_BURST)) {
        qemu_log_mask(LOG_GUEST_ERROR, "mb8795: receive buffer not aligned\n");
    }
    if (s->rx_size == s->rx_limit) {
        c->saved_next = c->next;
    }
    while (c->next < c->limit && s->rx_size > 0) {
        if (!next_dma_write_mem(dma, NEXT_DMA_EN_RX,
                                &s->rx_buf[s->rx_limit - s->rx_size], 1)) {
            break;
        }
        s->rx_size--;
    }
    if (s->rx_size == 0) {
        c->saved_limit = c->next;
        if (s->turbo) {
            s->saved_nibble = c->next % NEXT_DMA_BURST;
            if (!(c->csr & NEXT_DMA_SUPDATE)) {
                c->next -= s->saved_nibble;
                if (c->next < c->limit) {
                    c->next += NEXT_DMA_BURST;
                }
            }
        }
        if (eop) {
            c->next |= NEXT_DMA_EN_BOP;
        }
    }
    next_dma_enet_done(dma, NEXT_DMA_EN_RX);
}

static bool mb8795_dma_tx(MB8795State *s)
{
    NeXTDMAState *dma = s->dma;
    NeXTDMAChan *c = next_dma_chan(dma, NEXT_DMA_EN_TX);

    if (!next_dma_enabled(dma, NEXT_DMA_EN_TX)) {
        return false;
    }
    if (s->tx_size == 0) {
        c->saved_next = c->next;
    }
    while (c->next < NEXT_DMA_EN_ADDR(c->limit) &&
           s->tx_size < MB8795_BUF_SIZE) {
        if (!next_dma_read_mem(dma, NEXT_DMA_EN_TX, &s->tx_buf[s->tx_size],
                               1)) {
            break;
        }
        s->tx_size++;
    }
    if (c->limit & NEXT_DMA_EN_EOP) {
        next_dma_enet_done(dma, NEXT_DMA_EN_TX);
        return true;
    }
    next_dma_enet_done(dma, NEXT_DMA_EN_TX);
    return false;
}

/* ---- the timer ---- */

static void mb8795_receive_step(MB8795State *s)
{
    int old;

    switch (s->recv_state) {
    case RECV_STATE_WAITING:
        if (s->rx_size == 0) {
            return;
        }
        s->rx_chain = false;
        if (s->rx_size < ENET_FRAMESIZE_MIN &&
            !(s->rx_mode & RXMODE_ENA_SHORT)) {
            mb8795_rx_interrupt(s, RXSTAT_SHORT_PKT);
            s->rx_size = 0;
            s->tx_status &= ~TXSTAT_NET_BUSY;
            return;
        }
        s->recv_state = RECV_STATE_RECEIVING;
        /* fall through */
    case RECV_STATE_RECEIVING:
        if (s->rx_size == 0) {
            return;
        }
        old = s->rx_size;
        mb8795_dma_rx(s, s->turbo ? false : s->rx_chain);
        if (s->rx_size == old) {
            mb8795_rx_interrupt(s, RXSTAT_OVERFLOW);
            if (s->turbo) {
                s->rx_mode &= ~RXMODE_ENABLE;
            }
            s->rx_chain = false;
            s->rx_size = 0;
            s->tx_status &= ~TXSTAT_NET_BUSY;
            s->recv_state = RECV_STATE_WAITING;
            return;
        }
        if (s->rx_size > 0) {
            s->rx_chain = true;
            return;
        }
        s->rx_chain = false;
        mb8795_rx_interrupt(s, RXSTAT_PKT_OK);
        if (s->wire == EN_LOOPBACK) {
            mb8795_tx_interrupt(s, TXSTAT_TX_RECVD);
        }
        s->tx_status &= ~TXSTAT_NET_BUSY;
        s->recv_state = RECV_STATE_WAITING;
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        break;
    }
}

static void mb8795_send_step(MB8795State *s)
{
    int old;
    bool done;

    if (s->turbo) {
        if (!(s->tx_mode & TXMODE_ENABLE) ||
            (s->tx_status & TXSTAT_NET_BUSY)) {
            return;
        }
        mb8795_dma_tx(s);
        if (s->tx_size > 0) {
            if (s->wire == EN_DISCONNECTED) {
                mb8795_tx_interrupt(s, TXSTAT_16COLLS);
            } else {
                mb8795_send(s, s->wire);
            }
            s->tx_size = 0;
            mb8795_tx_interrupt(s, TXSTAT_READY);
        }
        return;
    }

    if (!(s->tx_status & TXSTAT_READY) || (s->tx_status & TXSTAT_NET_BUSY)) {
        return;
    }
    old = s->tx_size;
    done = mb8795_dma_tx(s);
    if (s->tx_size > 0) {
        mb8795_tx_release(s, TXSTAT_TX_RECVD | TXSTAT_SHORTED);
        if (s->tx_size == old && !done) {
            mb8795_tx_interrupt(s, TXSTAT_UNDERFLOW);
            s->tx_size = 0;
        } else if (s->wire == EN_DISCONNECTED) {
            mb8795_tx_interrupt(s, TXSTAT_16COLLS);
            s->tx_size = 0;
            done = false;
        } else if (s->tx_size > 15) {
            /*
             * NeXT software sets the limit 15 bytes past the end of the
             * packet, for the DMA bursts (Previous does the same).
             */
            s->tx_size -= 15;
        } else if (done) {
            s->tx_size = 0;
            done = false;
        }
    }
    if (done) {
        mb8795_send(s, s->wire);
        s->tx_size = 0;
    }
}

static void mb8795_tick(void *opaque)
{
    MB8795State *s = opaque;

    if (s->reset & EN_RESET) {
        return;
    }
    s->in_tick = true;
    s->wire = mb8795_wire(s);
    /* packets that came while the receiver was busy, or while sending */
    if (s->recv_state == RECV_STATE_WAITING && !s->rx_size) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
    mb8795_receive_step(s);
    mb8795_send_step(s);
    /* a looped back packet goes into memory at once */
    if (s->recv_state == RECV_STATE_WAITING && s->rx_size) {
        mb8795_receive_step(s);
    }
    s->in_tick = false;

    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (s->recv_state == RECV_STATE_WAITING ? ENET_IO_DELAY
                                                   : ENET_IO_SHORT) *
              SCALE_US);
}

static void mb8795_reset_reg(MB8795State *s)
{
    if (s->reset & EN_RESET) {
        s->tx_status = s->turbo ? 0 : TXSTAT_READY;
        timer_del(s->timer);
    } else if (!timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  ENET_IO_DELAY * SCALE_US);
    }
}

static void mb8795_dma_kick(void *opaque, NeXTDMAChannel ch)
{
    MB8795State *s = opaque;

    if ((s->reset & EN_RESET) || s->in_tick) {
        return;
    }
    mb8795_tick(s);
}

/* ---- network ---- */

static bool mb8795_can_receive(NetClientState *nc)
{
    MB8795State *s = qemu_get_nic_opaque(nc);
    int wire = mb8795_wire(s);

    /*
     * Not while sending: an answer that comes back at once waits for the
     * next tick, when the driver has set up the receive DMA again (the
     * Turbo controller turns its receiver off on an overflow).
     */
    return !(s->reset & EN_RESET) && !s->sending && s->rx_size == 0 &&
           s->recv_state == RECV_STATE_WAITING &&
           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->rx_wire_free &&
           (wire == EN_THINWIRE || wire == EN_TWISTEDPAIR);
}

static ssize_t mb8795_receive(NetClientState *nc, const uint8_t *buf,
                              size_t size)
{
    MB8795State *s = qemu_get_nic_opaque(nc);
    uint8_t pkt[MB8795_BUF_SIZE];
    size_t len = MIN(size, sizeof(pkt));

    if (!mb8795_can_receive(nc)) {
        return 0;
    }
    memcpy(pkt, buf, len);
    /*
     * Packets come no faster than 10 Mbit/s carries them, with the
     * preamble, CRC and gap: the driver sets up the receive DMA for the
     * next one in that time, as on the real wire. Without it a burst,
     * such as the fragments of an 8 KB NFS read, overflows the receiver.
     */
    s->rx_wire_free = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (int64_t)(MAX(size, 60) + 24) * ENET_NS_PER_BYTE;
    len = mb8795_fix_bootp(s, pkt, len);
    mb8795_take_packet(s, pkt, len);
    return size;
}

static NetClientInfo mb8795_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = mb8795_can_receive,
    .receive = mb8795_receive,
};

/* ---- registers ---- */

static MemTxResult mb8795_read(void *opaque, hwaddr addr, uint64_t *data,
                               unsigned size, MemTxAttrs attrs)
{
    MB8795State *s = opaque;
    unsigned reg = addr & (s->turbo ? 0xf : 0x1f);
    uint8_t val = 0;

    switch (reg) {
    case 0x0:
        val = s->tx_status;
        break;
    case 0x1:
        val = s->tx_mask;
        break;
    case 0x2:
        if (s->turbo && !(s->tx_mode & (TXMODE_TPE | TXMODE_LOOP)) &&
            (!mb8795_connected(s) || s->twisted_pair)) {
            return MEMTX_DECODE_ERROR;
        }
        val = s->rx_status;
        break;
    case 0x3:
        val = s->rx_mask;
        break;
    case 0x4:
        val = s->tx_mode;
        break;
    case 0x5:
        val = s->rx_mode;
        break;
    case 0x6:
        if (s->turbo) {
            val = s->reset & EN_RESET;
            if ((s->tx_mode & TXMODE_TPE) &&
                (!mb8795_connected(s) || !s->twisted_pair)) {
                val |= ENCTRL_BADTPE;
            }
        } else {
            val = s->reset;
        }
        break;
    case 0x7:
        val = s->turbo ? s->saved_nibble & 0x0F : 0;
        break;
    case 0x8 ... 0xd:
        val = s->mac[reg - 8];
        break;
    case 0xe:
    case 0xf:
        val = 0;
        break;
    case 0x10 ... 0x14:
        /* memory timing */
        val = s->mem_timing[reg - 0x10];
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    *data = val;
    return MEMTX_OK;
}

static MemTxResult mb8795_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size, MemTxAttrs attrs)
{
    MB8795State *s = opaque;
    unsigned reg = addr & (s->turbo ? 0xf : 0x1f);
    uint8_t val = data;

    switch (reg) {
    case 0x0:
        s->tx_status &= ~(val & (s->turbo ? 0xBE : 0x0F));
        break;
    case 0x1:
        s->tx_mask = val & (s->turbo ? 0xBE : 0xAF);
        break;
    case 0x2:
        s->rx_status &= ~(val & (s->turbo ? 0xDF : 0x8F));
        break;
    case 0x3:
        s->rx_mask = val & (s->turbo ? 0xDF : 0x9F);
        break;
    case 0x4:
        s->tx_mode = val;
        break;
    case 0x5:
        s->rx_mode = val;
        break;
    case 0x6:
        s->reset = val;
        mb8795_reset_reg(s);
        break;
    case 0x7:
        break;
    case 0x8 ... 0xd:
        s->mac[reg - 8] = val;
        break;
    case 0xe:
    case 0xf:
        break;
    case 0x10 ... 0x14:
        s->mem_timing[reg - 0x10] = val;
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    mb8795_update_irqs(s);
    return MEMTX_OK;
}

static const MemoryRegionOps mb8795_ops = {
    .read_with_attrs = mb8795_read,
    .write_with_attrs = mb8795_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

void mb8795_set_tpe(MB8795State *s, bool tpe)
{
    s->tpe_select = tpe;
}

static void mb8795_reset_hold(Object *obj, ResetType type)
{
    MB8795State *s = MB8795(obj);

    s->reset = EN_RESET;
    s->rx_size = s->tx_size = 0;
    s->rx_limit = 0;
    s->tx_status = s->turbo ? 0 : TXSTAT_READY;
    s->tx_mask = s->rx_mask = s->rx_status = 0;
    s->tx_mode = s->rx_mode = 0;
    s->recv_state = RECV_STATE_WAITING;
    s->rx_chain = false;
    s->tpe_select = false;
    s->saved_nibble = 0;
    timer_del(s->timer);
    mb8795_update_irqs(s);
}

static void mb8795_realize(DeviceState *dev, Error **errp)
{
    MB8795State *s = MB8795(dev);

    if (!s->dma) {
        error_setg(errp, "mb8795 needs the dma link");
        return;
    }
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&mb8795_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mb8795_tick, s);
    next_dma_set_client(s->dma, NEXT_DMA_EN_TX, mb8795_dma_kick, NULL, s);
    next_dma_set_client(s->dma, NEXT_DMA_EN_RX, mb8795_dma_kick, NULL, s);
}

static void mb8795_init(Object *obj)
{
    MB8795State *s = MB8795(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mem, obj, &mb8795_ops, s, "mb8795", 0x1000);
    sysbus_init_mmio(sbd, &s->mem);
    sysbus_init_irq(sbd, &s->tx_irq);
    sysbus_init_irq(sbd, &s->rx_irq);
}

static const VMStateDescription mb8795_vmstate = {
    .name = "mb8795",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(tx_status, MB8795State),
        VMSTATE_UINT8(tx_mask, MB8795State),
        VMSTATE_UINT8(tx_mode, MB8795State),
        VMSTATE_UINT8(rx_status, MB8795State),
        VMSTATE_UINT8(rx_mask, MB8795State),
        VMSTATE_UINT8(rx_mode, MB8795State),
        VMSTATE_UINT8(reset, MB8795State),
        VMSTATE_UINT8_ARRAY(mac, MB8795State, 6),
        VMSTATE_UINT8_ARRAY(mem_timing, MB8795State, 5),
        VMSTATE_UINT8(saved_nibble, MB8795State),
        VMSTATE_BOOL(tpe_select, MB8795State),
        VMSTATE_BOOL(rx_chain, MB8795State),
        VMSTATE_INT32(recv_state, MB8795State),
        VMSTATE_INT32(wire, MB8795State),
        VMSTATE_INT32(rx_size, MB8795State),
        VMSTATE_INT32(rx_limit, MB8795State),
        VMSTATE_INT32(tx_size, MB8795State),
        VMSTATE_UINT8_ARRAY(rx_buf, MB8795State, MB8795_BUF_SIZE),
        VMSTATE_UINT8_ARRAY(tx_buf, MB8795State, MB8795_BUF_SIZE),
        VMSTATE_TIMER_PTR(timer, MB8795State),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mb8795_properties[] = {
    DEFINE_NIC_PROPERTIES(MB8795State, conf),
    DEFINE_PROP_LINK("dma", MB8795State, dma, TYPE_NEXT_DMA, NeXTDMAState *),
    DEFINE_PROP_BOOL("turbo", MB8795State, turbo, false),
    DEFINE_PROP_BOOL("twisted-pair", MB8795State, twisted_pair, false),
    /* the 68030 machine has no BMAP to select twisted pair */
    DEFINE_PROP_BOOL("no-tpe", MB8795State, no_tpe, false),
};

static void mb8795_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Fujitsu MB8795 Ethernet (NeXT)";
    dc->realize = mb8795_realize;
    dc->vmsd = &mb8795_vmstate;
    device_class_set_props(dc, mb8795_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    rc->phases.hold = mb8795_reset_hold;
}

static const TypeInfo mb8795_info = {
    .name = TYPE_MB8795,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MB8795State),
    .instance_init = mb8795_init,
    .class_init = mb8795_class_init,
};

static void mb8795_register_types(void)
{
    type_register_static(&mb8795_info);
}

type_init(mb8795_register_types)
