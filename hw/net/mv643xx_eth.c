/*
 * Marvell Discovery II (MV64360/MV64361) Gigabit Ethernet controller
 *
 * The Ethernet unit of the Discovery II system controllers, as on the
 * Genesi/bPlan Pegasos II, where port 1 is the board's Gigabit port.
 * The registers are at 0x2000-0x3fff of the system controller's register
 * space. Each port has eight receive and eight transmit queues of 16 byte
 * descriptors, a unicast and two multicast filter tables and MIB counters.
 * The PHY is a Marvell 88E1111 that always has a 1000 Mbit/s full duplex
 * link while the network does. The DMA reads and writes the CPU's address
 * space directly, without the unit's address decoding windows.
 *
 * Interrupt coalescing, the transmit bandwidth limits and the receive
 * priority rules (VLAN, DSCP, TCP and UDP capture) are not modelled: every
 * frame goes to the queue the address filter gives.
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/net/mv643xx_eth.h"
#include "hw/net/mii.h"
#include "migration/vmstate.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "trace.h"
#include <zlib.h>

/* Registers shared by the ports */
#define R_PHY_ADDR      0x000
#define R_SMI           0x004
#define R_EUICR         0x080
#define R_EUIMR         0x084

#define SMI_DATA        0x0000ffff
#define SMI_READ        BIT(26)
#define SMI_READ_VALID  BIT(27)
#define EUICR_SMI_DONE  BIT(4)

/* Port registers, from 0x400 for port 0 and 0x800 for port 1 */
#define PORT(p)         (0x400 + (p) * 0x400)
#define R_PXCR          0x000
#define R_MACAL         0x014
#define R_MACAH         0x018
#define R_SDCR          0x01c
#define R_PSCR          0x03c
#define R_PSR           0x044
#define R_TQCR          0x048
#define R_ICR           0x060
#define R_ICER          0x064
#define R_PIMR          0x068
#define R_PEIMR         0x06c
#define R_DFC           0x084
#define R_OFC           0x088
#define R_CRDP(q)       (0x20c + (q) * 0x10)
#define R_RQCR          0x280
#define R_TCSDP         0x284
#define R_TCQDP(q)      (0x2c0 + (q) * 4)

/* Per port blocks */
#define MIB(p)          (0x1000 + (p) * 0x80)
#define DFSMT(p)        (0x1400 + (p) * 0x400)
#define DFOMT(p)        (0x1500 + (p) * 0x400)
#define DFUT(p)         (0x1600 + (p) * 0x400)

#define PXCR_UPM        BIT(0)
#define PXCR_RXQ(v)     (((v) >> 1) & 7)
#define PXCR_RXQARP(v)  (((v) >> 4) & 7)
#define PXCR_RB         BIT(7)
#define PXCR_RBIP       BIT(8)
#define PXCR_RBARP      BIT(9)

#define SDCR_BLMR       BIT(4)
#define SDCR_BLMT       BIT(5)
#define SDCR_SWAP       BIT(6)

#define PSCR_PORT_EN    BIT(0)

#define PSR_LINK_UP     BIT(1)
#define PSR_FULL_DX     BIT(2)
#define PSR_GMII_SPEED  BIT(4)
#define PSR_TX_FIFO_EMP BIT(10)

#define ICR_RX_BUFFER   BIT(0)
#define ICR_EXTEND      BIT(1)
#define ICR_RX_Q(q)     BIT(2 + (q))
#define ICR_RX_ERROR    BIT(10)
#define ICR_RX_ERR_Q(q) BIT(11 + (q))
#define ICR_TX_END(q)   BIT(19 + (q))
#define ICR_SUM         BIT(31)

#define ICER_TX_BUF(q)  BIT(q)
#define ICER_PHY_STC    BIT(16)
#define ICER_LINK_CHG   BIT(20)

/* Descriptors: the first 64-bit word, then buffer and next pointers */
#define DESC_OWN        BIT(31)
#define DESC_ES         BIT(0)

#define TX_AM           BIT(30)
#define TX_EI           BIT(23)
#define TX_GC           BIT(22)
#define TX_F            BIT(21)
#define TX_L            BIT(20)
#define TX_P            BIT(19)
#define TX_GIPCHK       BIT(18)
#define TX_GL4CHK       BIT(17)

#define RX_L4CHK_OK     BIT(30)
#define RX_EI           BIT(29)
#define RX_U            BIT(28)
#define RX_F            BIT(27)
#define RX_L            BIT(26)
#define RX_IP_HEAD_OK   BIT(25)
#define RX_L3IP         BIT(24)
#define RX_L2EV2        BIT(23)
#define RX_L4_TCP       (0 << 21)
#define RX_L4_UDP       (1 << 21)
#define RX_L4_OTHER     (2 << 21)
#define RX_EC_RE        (3 << 1)
#define RX_IPV4_FRG     BIT(2)

/* MIB counters, by byte offset */
#define MIB_RX_OCTETS   0x00
#define MIB_RX_FRAMES   0x10
#define MIB_RX_BCAST    0x18
#define MIB_RX_MCAST    0x1c
#define MIB_TX_OCTETS   0x38
#define MIB_TX_FRAMES   0x40
#define MIB_TX_MCAST    0x48
#define MIB_TX_BCAST    0x4c

#define MAX_FRAME       9728

/* Marvell 88E1111 */
#define PHY_ID1         0x0141
#define PHY_ID2         0x0cc2
#define PHY_SPEC_STATUS 17
#define PHY_INT_STATUS  19

#define REG(s, off)     ((s)->regs[(off) >> 2])
#define PREG(s, p, off) REG(s, PORT(p) + (off))

/*
 * The interrupt lines. A line goes down at once, but up only some time
 * later, as the interrupt takes its time on the way to the CPU on real
 * boards. MorphOS's driver needs that: it unmasks the port, reads the mask
 * back, then marks its interrupt as on, and its handler ignores the port
 * while it is off. Raised before that store, the level-triggered interrupt
 * would come back for ever. Time alone does not do it, as the read back
 * may wait long for the main loop: an interrupt that the guest unmasks
 * comes 50 microseconds after its next access to the registers, or after a
 * millisecond without one. Any other comes 50 microseconds after the last
 * access.
 */
#define IRQ_LATENCY_NS  50000
#define IRQ_UNMASK_NS   1000000

enum {
    IRQ_EVENT,      /* the device's own doing */
    IRQ_UNMASK,     /* the guest wrote a mask */
    IRQ_TIMER,      /* the wait is over */
};

static void mv643xx_eth_set_irqs(MV643xxEthState *s, int why)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool pending = false, unit;

    for (int p = 0; p < MV643XX_ETH_PORTS; p++) {
        uint32_t icer = PREG(s, p, R_ICER) & PREG(s, p, R_PEIMR) &
                        ~ICR_SUM;
        uint32_t icr = PREG(s, p, R_ICR) & ~(ICR_EXTEND | ICR_SUM);
        bool level[4];

        if (icer) {
            icr |= ICR_EXTEND;
        }
        icr &= PREG(s, p, R_PIMR);
        level[MV643XX_ETH_IRQ_SUM] = icr != 0;
        level[MV643XX_ETH_IRQ_RX] = (icr & 0x0007fffd) ||
                                    (icer & (BIT(17) | BIT(18)));
        level[MV643XX_ETH_IRQ_TX] = (icr & 0x7ff80000) ||
                                    (icer & (0xffff | BIT(19)));
        level[MV643XX_ETH_IRQ_MISC] = icer & (ICER_PHY_STC | 0x00f00000);
        for (int n = 0; n < 4; n++) {
            int i = MV643XX_ETH_IRQ(p, n);

            if (level[n] && !s->irq_level[i] && why != IRQ_TIMER) {
                pending = true;
                continue;
            }
            if (level[n] != s->irq_level[i]) {
                trace_mv643xx_eth_irq(i, level[n]);
                s->irq_level[i] = level[n];
                qemu_set_irq(s->irq[i], level[n]);
            }
        }
    }
    unit = (REG(s, R_EUICR) & REG(s, R_EUIMR)) != 0;
    if (unit != s->irq_level[MV643XX_ETH_IRQ_UNIT]) {
        s->irq_level[MV643XX_ETH_IRQ_UNIT] = unit;
        qemu_set_irq(s->irq[MV643XX_ETH_IRQ_UNIT], unit);
    }

    if (why == IRQ_TIMER) {
        s->irq_wait_access = false;
    } else if (!pending) {
        timer_del(s->irq_timer);
        s->irq_wait_access = false;
    } else if (why == IRQ_UNMASK) {
        s->irq_wait_access = true;
        timer_mod(s->irq_timer, now + IRQ_UNMASK_NS);
    } else if (!s->irq_wait_access) {
        timer_mod(s->irq_timer, now + IRQ_LATENCY_NS);
    }
}

/* An access to the registers: a rise that is waiting waits again */
static void mv643xx_eth_access(MV643xxEthState *s)
{
    if (timer_pending(s->irq_timer)) {
        s->irq_wait_access = false;
        timer_mod(s->irq_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + IRQ_LATENCY_NS);
    }
}

/*
 * A handler that reads the cause again without a write, as MorphOS's does
 * when its interrupt is marked off, gets the lines down and up again after
 * the wait. Kept up, they would take the CPU back to the handler for ever.
 */
static void mv643xx_eth_cause_read(MV643xxEthState *s, int p)
{
    bool up = false;

    if (s->cause_reads++ < 1) {
        return;
    }
    for (int n = 0; n < 4; n++) {
        int i = MV643XX_ETH_IRQ(p, n);

        if (s->irq_level[i]) {
            trace_mv643xx_eth_irq(i, 0);
            s->irq_level[i] = false;
            qemu_set_irq(s->irq[i], 0);
            up = true;
        }
    }
    if (up) {
        s->cause_reads = 0;
        mv643xx_eth_set_irqs(s, IRQ_EVENT);
    }
}

static void mv643xx_eth_update_irq(MV643xxEthState *s)
{
    mv643xx_eth_set_irqs(s, IRQ_EVENT);
}

static void mv643xx_eth_irq_timer(void *opaque)
{
    mv643xx_eth_set_irqs(opaque, IRQ_TIMER);
}

static bool mv643xx_eth_link_up(MV643xxEthState *s, int p)
{
    return p == s->nic_port && !qemu_get_queue(s->nic)->link_down &&
           (PREG(s, p, R_PSCR) & PSCR_PORT_EN);
}

/* A link change, as when the PHY ends auto-negotiation */
static void mv643xx_eth_link_event(MV643xxEthState *s)
{
    PREG(s, s->nic_port, R_ICER) |= ICER_PHY_STC | ICER_LINK_CHG;
    s->phy[PHY_INT_STATUS] |= 0x0c00;   /* link and auto-negotiation */
    mv643xx_eth_update_irq(s);
}

/* PHY */

static void mv643xx_eth_phy_reset(MV643xxEthState *s)
{
    memset(s->phy, 0, sizeof(s->phy));
    s->phy[MII_BMCR] = MII_BMCR_AUTOEN | MII_BMCR_FD | MII_BMCR_SPEED1000;
    s->phy[MII_PHYID1] = PHY_ID1;
    s->phy[MII_PHYID2] = PHY_ID2;
    s->phy[MII_ANAR] = MII_ANAR_PAUSE | MII_ANAR_TXFD | MII_ANAR_TX |
                       MII_ANAR_10FD | MII_ANAR_10 | MII_ANAR_CSMACD;
    s->phy[MII_CTRL1000] = MII_CTRL1000_FULL | MII_CTRL1000_HALF;
    s->phy[MII_EXTSTAT] = MII_EXTSTAT_1000T_FD | MII_EXTSTAT_1000T_HD;
}

static uint16_t mv643xx_eth_phy_read(MV643xxEthState *s, int reg)
{
    bool link = !qemu_get_queue(s->nic)->link_down;
    uint16_t val;

    switch (reg) {
    case MII_BMSR:
        val = MII_BMSR_100TX_FD | MII_BMSR_100TX_HD | MII_BMSR_10T_FD |
              MII_BMSR_10T_HD | MII_BMSR_EXTSTAT | MII_BMSR_MFPS |
              MII_BMSR_AUTONEG | MII_BMSR_EXTCAP;
        if (link) {
            val |= MII_BMSR_LINK_ST | MII_BMSR_AN_COMP;
        }
        return val;
    case MII_ANLPAR:
        return link ? MII_ANLPAR_ACK | MII_ANLPAR_PAUSE | MII_ANLPAR_TXFD |
                      MII_ANLPAR_TX | MII_ANLPAR_10FD | MII_ANLPAR_10 |
                      MII_ANLPAR_CSMACD : 0;
    case MII_ANER:
        return link ? MII_ANER_NWAY : 0;
    case MII_STAT1000:
        return link ? MII_STAT1000_FULL | MII_STAT1000_HALF |
                      0x3000 /* local and remote receiver OK */ : 0;
    case PHY_SPEC_STATUS:
        /* 1000 Mbit/s, full duplex, resolved, link */
        return link ? 0xac00 : 0x8000;
    case PHY_INT_STATUS:
        val = s->phy[reg];
        s->phy[reg] = 0;
        return val;
    default:
        return s->phy[reg];
    }
}

static void mv643xx_eth_phy_write(MV643xxEthState *s, int reg, uint16_t val)
{
    switch (reg) {
    case MII_BMCR:
        if (val & MII_BMCR_RESET) {
            mv643xx_eth_phy_reset(s);
            val &= ~MII_BMCR_RESET;
        }
        if (val & MII_BMCR_ANRESTART) {
            val &= ~MII_BMCR_ANRESTART;
            if (mv643xx_eth_link_up(s, s->nic_port)) {
                mv643xx_eth_link_event(s);
            }
        }
        s->phy[reg] = val;
        break;
    case MII_BMSR:
    case MII_PHYID1:
    case MII_PHYID2:
    case MII_ANLPAR:
    case MII_ANER:
    case MII_STAT1000:
    case MII_EXTSTAT:
    case PHY_SPEC_STATUS:
    case PHY_INT_STATUS:
        break;
    default:
        s->phy[reg] = val;
        break;
    }
}

static void mv643xx_eth_smi(MV643xxEthState *s, uint32_t val)
{
    int addr = (val >> 16) & 0x1f;
    int reg = (val >> 21) & 0x1f;
    int phy = (REG(s, R_PHY_ADDR) >> (s->nic_port * 5)) & 0x1f;
    uint16_t data = val & SMI_DATA;

    if (val & SMI_READ) {
        data = addr == phy ? mv643xx_eth_phy_read(s, reg) : 0xffff;
        REG(s, R_SMI) = (val & ~SMI_DATA) | data | SMI_READ_VALID;
    } else {
        if (addr == phy) {
            mv643xx_eth_phy_write(s, reg, data);
        }
        REG(s, R_SMI) = val & ~SMI_READ_VALID;
    }
    trace_mv643xx_eth_smi(val & SMI_READ ? "read" : "write", addr, reg, data);
    REG(s, R_EUICR) |= EUICR_SMI_DONE;
    mv643xx_eth_update_irq(s);
}

/* Descriptors */

/*
 * The descriptor fields are little endian 32 and 16 bit words in two
 * 64-bit words. With the port's BLMR or BLMT bit clear the DMA swaps the
 * bytes of each 64-bit word, as big endian hosts want; Linux's big endian
 * descriptor layout is that. The Swap mode bit swaps them again.
 */
static bool mv643xx_eth_desc_be(MV643xxEthState *s, int p, bool rx)
{
    uint32_t sdcr = PREG(s, p, R_SDCR);

    return !(sdcr & (rx ? SDCR_BLMR : SDCR_BLMT)) ^ !!(sdcr & SDCR_SWAP);
}

static bool mv643xx_eth_desc_read(hwaddr addr, bool be, uint64_t d[2])
{
    uint8_t b[16];

    if (dma_memory_read(&address_space_memory, addr, b, sizeof(b),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "mv643xx-eth: bad descriptor "
                      "address 0x%" HWADDR_PRIx "\n", addr);
        return false;
    }
    d[0] = be ? ldq_be_p(b) : ldq_le_p(b);
    d[1] = be ? ldq_be_p(b + 8) : ldq_le_p(b + 8);
    return true;
}

static void mv643xx_eth_desc_write(hwaddr addr, bool be, uint64_t d0)
{
    uint8_t b[8];

    if (be) {
        stq_be_p(b, d0);
    } else {
        stq_le_p(b, d0);
    }
    dma_memory_write(&address_space_memory, addr, b, sizeof(b),
                     MEMTXATTRS_UNSPECIFIED);
}

static void mv643xx_eth_mib_add(MV643xxEthState *s, int p, int off,
                                uint32_t n)
{
    uint32_t *c = &s->mib[p][off >> 2];

    if (off == MIB_RX_OCTETS || off == MIB_TX_OCTETS) {
        /* 64-bit counters */
        uint64_t v = deposit64(c[0], 32, 32, c[1]) + n;
        c[0] = v;
        c[1] = v >> 32;
    } else {
        *c += n;
    }
}

/* Transmit */

static void mv643xx_eth_tx_queue(MV643xxEthState *s, int p, int q)
{
    bool be = mv643xx_eth_desc_be(s, p, false);
    g_autofree uint8_t *frame = g_malloc(MAX_FRAME);
    uint32_t first_cmd = 0;
    int len = 0;

    while (PREG(s, p, R_TQCR) & BIT(q)) {
        hwaddr addr = PREG(s, p, R_TCQDP(q));
        uint64_t d[2];
        uint32_t cmd, next;
        int n;

        if (!addr || !mv643xx_eth_desc_read(addr, be, d) ||
            !((cmd = d[0]) & DESC_OWN)) {
            /* the end of the queue */
            PREG(s, p, R_TQCR) &= ~BIT(q);
            PREG(s, p, R_ICR) |= ICR_TX_END(q);
            break;
        }
        n = d[0] >> 48;
        next = d[1] >> 32;
        if (cmd & TX_F) {
            first_cmd = cmd;
            len = 0;
        }
        n = MIN(n, MAX_FRAME - len);
        if (n > 0 && dma_memory_read(&address_space_memory, (uint32_t)d[1],
                                     frame + len, n,
                                     MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
            len += n;
        }
        if (cmd & TX_L) {
            if ((cmd & TX_P) && len < 60) {
                memset(frame + len, 0, 60 - len);
                len = 60;
            }
            if (first_cmd & (TX_GIPCHK | TX_GL4CHK)) {
                net_checksum_calculate(frame, len,
                    (first_cmd & TX_GIPCHK ? CSUM_IP : 0) |
                    (first_cmd & TX_GL4CHK ? CSUM_TCP | CSUM_UDP : 0));
            }
            trace_mv643xx_eth_tx(p, q, len);
            mv643xx_eth_mib_add(s, p, MIB_TX_OCTETS, len + 4);
            mv643xx_eth_mib_add(s, p, MIB_TX_FRAMES, 1);
            if (is_broadcast_ether_addr(frame)) {
                mv643xx_eth_mib_add(s, p, MIB_TX_BCAST, 1);
            } else if (is_multicast_ether_addr(frame)) {
                mv643xx_eth_mib_add(s, p, MIB_TX_MCAST, 1);
            }
            if (p == s->nic_port) {
                qemu_send_packet(qemu_get_queue(s->nic), frame, len);
            }
        }
        /* close the descriptor, with no errors in the last one */
        if (!(cmd & TX_AM)) {
            cmd &= ~DESC_OWN;
        }
        if (cmd & TX_L) {
            cmd &= ~(DESC_ES | 6);
        }
        mv643xx_eth_desc_write(addr, be, (d[0] & ~0xffffffffULL) | cmd);
        if (cmd & TX_EI) {
            PREG(s, p, R_ICER) |= ICER_TX_BUF(q);
        }
        PREG(s, p, R_TCQDP(q)) = next;
        PREG(s, p, R_TCSDP) = addr;
    }
}

static void mv643xx_eth_tx(MV643xxEthState *s, int p)
{
    if (!(PREG(s, p, R_PSCR) & PSCR_PORT_EN)) {
        return;
    }
    /* queue 7 has the highest priority */
    for (int q = 7; q >= 0; q--) {
        mv643xx_eth_tx_queue(s, p, q);
    }
    mv643xx_eth_update_irq(s);
}

/* Receive */

static uint8_t mv643xx_eth_addr_crc(const uint8_t *addr)
{
    int crc = 0;

    for (int i = 0; i < 6; i++) {
        crc = (crc ^ addr[i]) << 8;
        for (int j = 7; j >= 0; j--) {
            if (crc & (0x100 << j)) {
                crc ^= 0x107 << j;
            }
        }
    }
    return crc;
}

static uint8_t mv643xx_eth_table(MV643xxEthState *s, int base, int entry)
{
    return REG(s, base + (entry & ~3)) >> ((entry & 3) * 8);
}

/* The receive queue of a frame, or -1 to drop it */
static int mv643xx_eth_filter(MV643xxEthState *s, int p, const uint8_t *buf,
                              size_t size, uint32_t *status)
{
    uint32_t pxcr = PREG(s, p, R_PXCR);
    uint32_t mach = PREG(s, p, R_MACAH), macl = PREG(s, p, R_MACAL);
    uint16_t type = size >= 14 ? lduw_be_p(buf + 12) : 0;
    uint8_t e;

    if (is_broadcast_ether_addr(buf)) {
        if (type == ETH_P_ARP) {
            return pxcr & PXCR_RBARP ? -1 : PXCR_RXQARP(pxcr);
        }
        if (pxcr & (type == ETH_P_IP ? PXCR_RBIP : PXCR_RB)) {
            return -1;
        }
        return PXCR_RXQ(pxcr);
    }
    if (is_multicast_ether_addr(buf)) {
        if (!memcmp(buf, "\x01\x00\x5e\x00\x00", 5)) {
            e = mv643xx_eth_table(s, DFSMT(p), buf[5]);
        } else {
            e = mv643xx_eth_table(s, DFOMT(p), mv643xx_eth_addr_crc(buf));
        }
        return e & 1 ? (e >> 1) & 7 : -1;
    }
    /* unicast: DA[47:4] against the MAC address, the nibble in the table */
    if (ldl_be_p(buf) == mach &&
        (lduw_be_p(buf + 4) & 0xfff0) == (macl & 0xfff0)) {
        e = mv643xx_eth_table(s, DFUT(p), buf[5] & 0xf);
        if (e & 1) {
            return (e >> 1) & 7;
        }
    }
    if (pxcr & PXCR_UPM) {
        *status |= RX_U;
        return PXCR_RXQ(pxcr);
    }
    return -1;
}

/* The frame type bits of the first descriptor, and its checksum checks */
static void mv643xx_eth_rx_classify(const uint8_t *frame, size_t len,
                                    uint32_t *status, uint32_t *bc)
{
    const uint8_t *ip = frame + 14;
    uint16_t type = lduw_be_p(frame + 12);
    unsigned ihl, tl, off;
    uint8_t proto;

    if (type < 0x600) {
        *status |= RX_L4_OTHER;
        return;
    }
    *status |= RX_L2EV2;
    if (type == ETH_P_VLAN && len >= 18) {
        *status |= BIT(19);
        type = lduw_be_p(frame + 16);
        ip += 4;
    }
    if (type != ETH_P_IP || ip + 20 > frame + len) {
        *status |= RX_L4_OTHER;
        return;
    }
    *status |= RX_L3IP;
    ihl = (ip[0] & 0xf) * 4;
    tl = lduw_be_p(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || ihl > tl ||
        ip + tl > frame + len ||
        net_raw_checksum((uint8_t *)ip, ihl) != 0) {
        *status |= RX_L4_OTHER;
        return;
    }
    *status |= RX_IP_HEAD_OK;
    proto = ip[9];
    if (proto != IP_PROTO_TCP && proto != IP_PROTO_UDP) {
        *status |= RX_L4_OTHER;
        return;
    }
    *status |= proto == IP_PROTO_TCP ? RX_L4_TCP : RX_L4_UDP;
    off = lduw_be_p(ip + 6);
    if (off & 0x3fff) {
        /* more fragments or a fragment offset */
        *bc |= RX_IPV4_FRG;
        return;
    }
    if (proto == IP_PROTO_UDP && !lduw_be_p(ip + ihl + 6)) {
        *status |= RX_L4CHK_OK;     /* no UDP checksum */
    } else {
        uint32_t sum = net_checksum_add(tl - ihl, (uint8_t *)ip + ihl);

        sum += net_checksum_add(8, (uint8_t *)ip + 12);
        sum += proto + tl - ihl;
        if (net_checksum_finish(sum) == 0) {
            *status |= RX_L4CHK_OK;
        }
    }
    *status |= (lduw_be_p(ip + ihl + (proto == IP_PROTO_TCP ? 16 : 6))
                & 0xffff) << 3;
}

static bool mv643xx_eth_can_receive(NetClientState *nc)
{
    MV643xxEthState *s = qemu_get_nic_opaque(nc);
    int p = s->nic_port;

    return mv643xx_eth_link_up(s, p) && (PREG(s, p, R_RQCR) & 0xff);
}

static ssize_t mv643xx_eth_receive(NetClientState *nc, const uint8_t *buf,
                                   size_t size)
{
    MV643xxEthState *s = qemu_get_nic_opaque(nc);
    int p = s->nic_port;
    bool be = mv643xx_eth_desc_be(s, p, true);
    uint8_t frame[2 + MAX_FRAME + 4];
    uint32_t status = 0, bc = 0, fcs, cmd = 0;
    hwaddr addr, first = 0;
    uint64_t d[2], fd0 = 0;
    size_t len, off = 0;
    int q, ndesc = 0;

    if (!mv643xx_eth_link_up(s, p) || size < 14 || size > MAX_FRAME) {
        return size;
    }
    q = mv643xx_eth_filter(s, p, buf, size, &status);
    if (q < 0) {
        return size;
    }
    if (!(PREG(s, p, R_RQCR) & BIT(q))) {
        PREG(s, p, R_DFC)++;
        return size;
    }

    /* two bytes so that the IP header is aligned, the frame, the FCS */
    frame[0] = frame[1] = 0;
    memcpy(frame + 2, buf, size);
    len = 2 + size;
    if (size < 60) {
        memset(frame + len, 0, 60 - size);
        len = 2 + 60;
    }
    fcs = crc32(0, frame + 2, len - 2);
    stl_le_p(frame + len, fcs);
    len += 4;
    mv643xx_eth_rx_classify(frame + 2, len - 6, &status, &bc);

    addr = PREG(s, p, R_CRDP(q));
    while (off < len) {
        size_t n;

        if (!addr || !mv643xx_eth_desc_read(addr, be, d) ||
            !((cmd = d[0]) & DESC_OWN)) {
            break;
        }
        n = MIN((d[0] >> 32) & 0xfff8, len - off);
        if (!n) {
            break;
        }
        dma_memory_write(&address_space_memory, (uint32_t)d[1], frame + off,
                         n, MEMTXATTRS_UNSPECIFIED);
        off += n;
        ndesc++;
        if (!first) {
            first = addr;
            fd0 = d[0];
        } else {
            /* other descriptors give theirs back as they are filled */
            uint32_t c = (cmd & ~(DESC_OWN | RX_F | RX_L)) |
                         (off == len ? RX_L : 0);
            mv643xx_eth_desc_write(addr, be,
                (d[0] & 0x0000ffff00000000ULL) | ((uint64_t)n << 48) | c);
        }
        addr = d[1] >> 32;
    }

    if (off < len) {
        /* out of descriptors */
        PREG(s, p, R_ICR) |= ICR_RX_ERROR | ICR_RX_ERR_Q(q);
        PREG(s, p, R_DFC)++;
        if (first) {
            cmd = (fd0 & ~(DESC_OWN | RX_L | 0x1fffff)) | RX_F | DESC_ES |
                  RX_EC_RE;
            mv643xx_eth_desc_write(first, be,
                (fd0 & 0x0000ffff00000000ULL) | ((uint64_t)off << 48) | cmd);
            PREG(s, p, R_CRDP(q)) = addr;
        }
        trace_mv643xx_eth_rx_error(p, q, size);
        mv643xx_eth_update_irq(s);
        return size;
    }

    /* the status goes to the first descriptor last */
    cmd = (fd0 & RX_EI) | status | RX_F | (ndesc == 1 ? RX_L : 0);
    mv643xx_eth_desc_write(first, be,
        (fd0 & 0x0000fff800000000ULL) | ((uint64_t)bc << 32) |
        ((uint64_t)len << 48) | cmd);
    PREG(s, p, R_CRDP(q)) = addr;
    if (!addr) {
        PREG(s, p, R_RQCR) &= ~BIT(q);
    }
    if (fd0 & RX_EI) {
        PREG(s, p, R_ICR) |= ICR_RX_BUFFER | ICR_RX_Q(q);
    }
    mv643xx_eth_mib_add(s, p, MIB_RX_OCTETS, len - 2);
    mv643xx_eth_mib_add(s, p, MIB_RX_FRAMES, 1);
    if (is_broadcast_ether_addr(buf)) {
        mv643xx_eth_mib_add(s, p, MIB_RX_BCAST, 1);
    } else if (is_multicast_ether_addr(buf)) {
        mv643xx_eth_mib_add(s, p, MIB_RX_MCAST, 1);
    }
    trace_mv643xx_eth_rx(p, q, size);
    mv643xx_eth_update_irq(s);
    return size;
}

static void mv643xx_eth_set_link(NetClientState *nc)
{
    MV643xxEthState *s = qemu_get_nic_opaque(nc);

    if (PREG(s, s->nic_port, R_PSCR) & PSCR_PORT_EN) {
        mv643xx_eth_link_event(s);
    }
}

/* Registers */

static uint64_t mv643xx_eth_read(void *opaque, hwaddr addr, unsigned size)
{
    MV643xxEthState *s = opaque;
    uint32_t val = REG(s, addr);
    int p = (addr - PORT(0)) / 0x400;
    hwaddr r = (addr - PORT(0)) % 0x400;

    if (addr >= MIB(0) && addr < MIB(MV643XX_ETH_PORTS)) {
        /* the counters clear when read */
        p = (addr - MIB(0)) / 0x80;
        val = s->mib[p][(addr & 0x7f) >> 2];
        s->mib[p][(addr & 0x7f) >> 2] = 0;
    } else if (addr >= PORT(0) && addr < PORT(MV643XX_ETH_PORTS)) {
        switch (r) {
        case R_PSR:
            val = PSR_TX_FIFO_EMP;
            if (mv643xx_eth_link_up(s, p)) {
                val |= PSR_LINK_UP | PSR_FULL_DX | PSR_GMII_SPEED;
            }
            break;
        case R_ICR:
        case R_ICER: {
            uint32_t icer = PREG(s, p, R_ICER) & ~ICR_SUM;

            if (r == R_ICR && p == s->nic_port) {
                mv643xx_eth_cause_read(s, p);
            }

            if (r == R_ICER) {
                val = icer;
                if (icer & PREG(s, p, R_PEIMR)) {
                    val |= ICR_SUM;
                }
                break;
            }
            val &= ~(ICR_EXTEND | ICR_SUM);
            if (icer & PREG(s, p, R_PEIMR)) {
                val |= ICR_EXTEND;
            }
            if (val & PREG(s, p, R_PIMR)) {
                val |= ICR_SUM;
            }
            break;
        }
        case R_DFC:
        case R_OFC:
            REG(s, addr) = 0;
            break;
        }
    } else if (addr >= PORT(MV643XX_ETH_PORTS) && addr < 0x1000) {
        val = 0;    /* there is no port 2 */
    }
    trace_mv643xx_eth_read(addr, val);
    mv643xx_eth_access(s);
    return val;
}

static void mv643xx_eth_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MV643xxEthState *s = opaque;
    int p = (addr - PORT(0)) / 0x400;
    hwaddr r = (addr - PORT(0)) % 0x400;
    uint32_t old;

    trace_mv643xx_eth_write(addr, val);
    mv643xx_eth_access(s);
    s->cause_reads = 0;
    switch (addr) {
    case R_SMI:
        mv643xx_eth_smi(s, val);
        return;
    case R_EUICR:
        REG(s, addr) &= val;
        mv643xx_eth_update_irq(s);
        return;
    case R_EUIMR:
        REG(s, addr) = val;
        mv643xx_eth_set_irqs(s, IRQ_UNMASK);
        return;
    }
    if (addr >= MIB(0) && addr < MIB(MV643XX_ETH_PORTS)) {
        return;     /* read only */
    }
    if (addr < PORT(0) || addr >= PORT(MV643XX_ETH_PORTS)) {
        REG(s, addr) = val;
        return;
    }

    old = REG(s, addr);
    switch (r) {
    case R_PSR:
    case R_DFC:
    case R_OFC:
        return;
    case R_PSCR:
        REG(s, addr) = val;
        if ((val ^ old) & PSCR_PORT_EN) {
            if (p == s->nic_port && mv643xx_eth_link_up(s, p)) {
                mv643xx_eth_link_event(s);
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
            mv643xx_eth_tx(s, p);
        }
        return;
    case R_TQCR:
        /* DISQ stops a queue at once; ENQ starts it */
        REG(s, addr) = (old & ~((val >> 8) & 0xff)) | (val & 0xff);
        mv643xx_eth_tx(s, p);
        return;
    case R_RQCR:
        REG(s, addr) = (old & ~((val >> 8) & 0xff)) | (val & 0xff);
        if (p == s->nic_port && (val & 0xff)) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    case R_ICR:
    case R_ICER:
        /* writing 0 clears a cause bit */
        REG(s, addr) &= val;
        mv643xx_eth_update_irq(s);
        return;
    case R_PIMR:
    case R_PEIMR:
        REG(s, addr) = val;
        mv643xx_eth_set_irqs(s, IRQ_UNMASK);
        return;
    default:
        REG(s, addr) = val;
        return;
    }
}

static const MemoryRegionOps mv643xx_eth_ops = {
    .read = mv643xx_eth_read,
    .write = mv643xx_eth_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static NetClientInfo mv643xx_eth_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = mv643xx_eth_can_receive,
    .receive = mv643xx_eth_receive,
    .link_status_changed = mv643xx_eth_set_link,
};

static void mv643xx_eth_reset(DeviceState *dev)
{
    MV643xxEthState *s = MV643XX_ETH(dev);
    const uint8_t *mac = s->conf.macaddr.a;

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->mib, 0, sizeof(s->mib));
    REG(s, R_PHY_ADDR) = 0x8 | (0x9 << 5) | (0xa << 10);
    for (int p = 0; p < MV643XX_ETH_PORTS; p++) {
        PREG(s, p, R_PXCR) = 7 << 22;
        PREG(s, p, R_SDCR) = (4 << 22) | SDCR_BLMT | SDCR_BLMR | (4 << 1);
    }
    /* the firmware leaves the address in the port's registers */
    PREG(s, s->nic_port, R_MACAH) = ldl_be_p(mac);
    PREG(s, s->nic_port, R_MACAL) = lduw_be_p(mac + 4);
    mv643xx_eth_phy_reset(s);
    timer_del(s->irq_timer);
    mv643xx_eth_set_irqs(s, IRQ_TIMER);
}

static void mv643xx_eth_realize(DeviceState *dev, Error **errp)
{
    MV643xxEthState *s = MV643XX_ETH(dev);

    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mv643xx_eth_irq_timer, s);
    if (s->nic_port >= MV643XX_ETH_PORTS) {
        error_setg(errp, "nic-port must be below %d", MV643XX_ETH_PORTS);
        return;
    }
    memory_region_init_io(&s->mmio, OBJECT(s), &mv643xx_eth_ops, s,
                          TYPE_MV643XX_ETH, MV643XX_ETH_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    for (int i = 0; i < MV643XX_ETH_NUM_IRQS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&mv643xx_eth_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void mv643xx_eth_unrealize(DeviceState *dev)
{
    MV643xxEthState *s = MV643XX_ETH(dev);

    timer_free(s->irq_timer);
    qemu_del_nic(s->nic);
}

static const VMStateDescription vmstate_mv643xx_eth = {
    .name = TYPE_MV643XX_ETH,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MV643xxEthState,
                             MV643XX_ETH_REGS_SIZE / 4),
        VMSTATE_UINT32_2DARRAY(mib, MV643xxEthState, MV643XX_ETH_PORTS, 32),
        VMSTATE_UINT16_ARRAY(phy, MV643xxEthState, 32),
        VMSTATE_BOOL_ARRAY(irq_level, MV643xxEthState, MV643XX_ETH_NUM_IRQS),
        VMSTATE_TIMER_PTR(irq_timer, MV643xxEthState),
        VMSTATE_BOOL(irq_wait_access, MV643xxEthState),
        VMSTATE_UINT32(cause_reads, MV643xxEthState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property mv643xx_eth_props[] = {
    DEFINE_NIC_PROPERTIES(MV643xxEthState, conf),
    DEFINE_PROP_UINT8("nic-port", MV643xxEthState, nic_port, 1),
};

static void mv643xx_eth_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Marvell Discovery II Gigabit Ethernet";
    dc->realize = mv643xx_eth_realize;
    dc->unrealize = mv643xx_eth_unrealize;
    dc->vmsd = &vmstate_mv643xx_eth;
    device_class_set_legacy_reset(dc, mv643xx_eth_reset);
    device_class_set_props(dc, mv643xx_eth_props);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    /* part of the system controller */
    dc->user_creatable = false;
}

static const TypeInfo mv643xx_eth_info = {
    .name = TYPE_MV643XX_ETH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MV643xxEthState),
    .class_init = mv643xx_eth_class_init,
};

static void mv643xx_eth_register_types(void)
{
    type_register_static(&mv643xx_eth_info);
}

type_init(mv643xx_eth_register_types)
