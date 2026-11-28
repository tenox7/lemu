/*
 * 3Com 3C905C / 3C920 "Tornado" Fast EtherLink XL PCI Ethernet
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

/*
 * The AmigaOne SE and XE have a 3C920 on board, the 3C905C core without a
 * boot ROM, and the AmigaOS 4 driver (eth3com.device) and the AmigaOne
 * firmware drive it.
 *
 * Registers 0x00-0x0d are in eight windows chosen with SelectWindow; the
 * command and interrupt status register is at 0x0e in all of them and the
 * bus master registers from 0x10 on are not windowed. Frames move through
 * descriptor lists in host memory: the download engine sends the frames of
 * the DPD list at DnListPtr, the upload engine writes received frames into
 * the UPD list at UpListPtr. Each descriptor has a next pointer, a frame
 * status word and address/length pairs, the last one with bit 31 set.
 * The PHY is the internal one at MII address 24 and always has a
 * 100 Mbit/s full duplex link.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/system.h"
#include "net/net.h"
#include "net/eth.h"
#include "net/checksum.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_TC90X "3c905c"

#define PCI_VENDOR_ID_3COM      0x10b7
OBJECT_DECLARE_SIMPLE_TYPE(TC90XState, TC90X)

#define TC_IO_SIZE              0x80
#define TC_EEPROM_WORDS         0x40
#define TC_PHY_ADDR             24
#define TC_MAX_FRAME            1536
#define TC_MAX_FRAGS            63
#define TC_POLL_NS              (100 * SCALE_US)

/* commands, the opcode is in bits 15-11 and the argument below */
#define CMD_TOTAL_RESET         0
#define CMD_SELECT_WINDOW       1
#define CMD_RX_DISABLE          3
#define CMD_RX_ENABLE           4
#define CMD_RX_RESET            5
#define CMD_STALL               6
#define CMD_RX_DISCARD          8
#define CMD_TX_ENABLE           9
#define CMD_TX_DISABLE          10
#define CMD_TX_RESET            11
#define CMD_FAKE_INTR           12
#define CMD_ACK_INTR            13
#define CMD_SET_INTR_ENB        14
#define CMD_SET_STATUS_ENB      15
#define CMD_SET_RX_FILTER       16
#define CMD_SET_RX_THRESHOLD    17
#define CMD_SET_TX_THRESHOLD    18
#define CMD_SET_TX_START        19
#define CMD_DMA                 20
#define CMD_STATS_ENABLE        21
#define CMD_STATS_DISABLE       22
#define CMD_STOP_COAX           23
#define CMD_SET_HASH_BIT        25

#define STALL_UP                0
#define STALL_UP_UNSTALL        1
#define STALL_DOWN              2
#define STALL_DOWN_UNSTALL      3

/* interrupt status */
#define INT_LATCH               BIT(0)
#define INT_HOST_ERROR          BIT(1)
#define INT_TX_COMPLETE         BIT(2)
#define INT_RX_COMPLETE         BIT(4)
#define INT_RX_EARLY            BIT(5)
#define INT_REQUESTED           BIT(6)
#define INT_UPDATE_STATS        BIT(7)
#define INT_LINK_EVENT          BIT(8)
#define INT_DN_COMPLETE         BIT(9)
#define INT_UP_COMPLETE         BIT(10)
#define INT_ACK_MASK            (INT_LATCH | INT_RX_EARLY | INT_REQUESTED | \
                                 INT_LINK_EVENT | INT_DN_COMPLETE | \
                                 INT_UP_COMPLETE)
#define INT_SOURCES             0x07fe

/* receive filter */
#define FILTER_STATION          BIT(0)
#define FILTER_MULTICAST        BIT(1)
#define FILTER_BROADCAST        BIT(2)
#define FILTER_PROMISC          BIT(3)
#define FILTER_HASH             BIT(4)

/* bus master registers */
#define R_TIMER                 0x1a
#define R_TX_STATUS             0x1b
#define R_INT_STATUS_AUTO       0x1e
#define R_DMA_CTRL              0x20
#define R_DN_LIST_PTR           0x24
#define R_DN_POLL               0x2d
#define R_UP_PKT_STATUS         0x30
#define R_FREE_TIMER            0x34
#define R_COUNTDOWN             0x36
#define R_UP_LIST_PTR           0x38
#define R_UP_POLL               0x3d

/* DmaCtrl */
#define DMA_DN_STALLED          BIT(2)
#define DMA_UP_COMPLETE         BIT(3)
#define DMA_DN_COMPLETE         BIT(4)
#define DMA_DN_IN_PROG          BIT(7)

/* descriptors */
#define DPD_DN_COMPLETE         BIT(16)
#define DPD_TX_INDICATE         BIT(15)
#define DPD_ADD_IP_CKSUM        BIT(25)
#define DPD_ADD_TCP_CKSUM       BIT(26)
#define DPD_ADD_UDP_CKSUM       BIT(27)
#define DPD_DN_INDICATE         BIT(31)
#define FRAG_LAST               BIT(31)
#define UPD_UP_COMPLETE         BIT(15)
#define UPD_UP_ERROR            BIT(14)

/* TxStatus */
#define TX_STATUS_COMPLETE      BIT(7)

struct TC90XState {
    PCIDevice dev;
    MemoryRegion io;
    MemoryRegion mmio;
    NICConf conf;
    NICState *nic;
    QEMUTimer *poll_timer;

    uint8_t window;
    uint16_t int_status;
    uint16_t int_enable;
    uint16_t status_enable;
    uint8_t rx_filter;
    bool rx_enabled;
    bool tx_enabled;
    bool up_stalled;
    bool dn_stalled;
    uint8_t tx_status;
    uint32_t dma_ctrl;
    uint32_t dn_list_ptr;
    uint32_t dn_last;           /* last DPD sent, for polling */
    uint8_t dn_poll;
    uint32_t up_list_ptr;
    uint32_t up_pkt_status;
    uint8_t up_poll;
    uint8_t hash[32];           /* 256 bit multicast hash filter */

    uint8_t station[6];
    uint8_t station_mask[6];
    uint32_t internal_config;
    uint16_t max_pkt_size;
    uint16_t mac_control;
    uint16_t media_status;
    uint16_t rx_early;
    uint16_t tx_start;
    uint16_t tx_reclaim;

    uint16_t eeprom[TC_EEPROM_WORDS];
    uint16_t eeprom_cmd;
    uint16_t eeprom_data;

    /* MII management interface, bit-banged through PhysicalMgmt */
    uint16_t phys_mgmt;
    uint32_t mii_shift;
    int mii_bits;
    int mii_state;
    uint8_t mii_phy;
    uint8_t mii_reg;
    bool mii_read;
    uint16_t mii_out;
    uint16_t phy[32];
};

static void tc_update_irq(TC90XState *s)
{
    if (s->int_status & INT_SOURCES & s->int_enable) {
        s->int_status |= INT_LATCH;
    }
    pci_set_irq(&s->dev, !!(s->int_status & s->int_enable & INT_SOURCES));
}

static void tc_raise(TC90XState *s, uint16_t bits)
{
    s->int_status |= bits & (s->status_enable | INT_LATCH);
    tc_update_irq(s);
}

/* PHY and MII management */

static void tc_phy_reset(TC90XState *s)
{
    memset(s->phy, 0, sizeof(s->phy));
    s->phy[0] = 0x3100;     /* auto negotiation, 100 Mbit/s, full duplex */
    s->phy[1] = 0x782d;     /* link up, negotiation done */
    s->phy[2] = 0x0180;     /* 3Com internal PHY */
    s->phy[3] = 0x7900;
    s->phy[4] = 0x01e1;
    s->phy[5] = 0x45e1;
    s->phy[6] = 0x0001;
}

static uint16_t tc_phy_read(TC90XState *s, int phy, int reg)
{
    if (phy != TC_PHY_ADDR) {
        return 0xffff;
    }
    return s->phy[reg];
}

static void tc_phy_write(TC90XState *s, int phy, int reg, uint16_t val)
{
    if (phy != TC_PHY_ADDR) {
        return;
    }
    switch (reg) {
    case 0:
        if (val & 0x8000) {
            tc_phy_reset(s);
        } else {
            s->phy[0] = val & ~0x0200;
        }
        break;
    case 4:
        s->phy[4] = val;
        break;
    }
}

/*
 * PhysicalMgmt: bit 0 clock, bit 1 data, bit 2 drive the data line.
 * Frames are clocked in on the rising edge: 32 bits of preamble, 01 start,
 * 10 read or 01 write, 5 bits PHY, 5 bits register, turnaround, 16 data.
 */
enum { MII_IDLE, MII_HEADER, MII_DATA_OUT, MII_DATA_IN };

static void tc_mii_write(TC90XState *s, uint16_t val)
{
    bool clk_rise = (val & 1) && !(s->phys_mgmt & 1);
    bool data = val & 2;

    s->phys_mgmt = val & 7;
    if (!clk_rise) {
        return;
    }
    switch (s->mii_state) {
    case MII_IDLE:
        s->mii_shift = (s->mii_shift << 1) | data;
        s->mii_bits++;
        /* a start after at least some preamble */
        if (s->mii_bits >= 2 && (s->mii_shift & 3) == 1 &&
            (s->mii_shift & ~3u) == ~3u) {
            s->mii_state = MII_HEADER;
            s->mii_shift = 0;
            s->mii_bits = 0;
        }
        break;
    case MII_HEADER:
        s->mii_shift = (s->mii_shift << 1) | data;
        if (++s->mii_bits == 12) {
            /* op(2) phy(5) reg(5) */
            s->mii_read = ((s->mii_shift >> 10) & 3) == 2;
            s->mii_phy = (s->mii_shift >> 5) & 0x1f;
            s->mii_reg = s->mii_shift & 0x1f;
            s->mii_bits = 0;
            s->mii_shift = 0;
            if (s->mii_read) {
                s->mii_out = tc_phy_read(s, s->mii_phy, s->mii_reg);
                s->mii_state = MII_DATA_OUT;
            } else {
                s->mii_state = MII_DATA_IN;
            }
        }
        break;
    case MII_DATA_OUT:
        /* two turnaround bits, then 16 data bits */
        if (++s->mii_bits >= 18) {
            s->mii_state = MII_IDLE;
            s->mii_bits = 0;
            s->mii_shift = 0;
        }
        break;
    case MII_DATA_IN:
        s->mii_shift = (s->mii_shift << 1) | data;
        if (++s->mii_bits == 18) {
            tc_phy_write(s, s->mii_phy, s->mii_reg, s->mii_shift & 0xffff);
            s->mii_state = MII_IDLE;
            s->mii_bits = 0;
            s->mii_shift = 0;
        }
        break;
    }
}

static uint16_t tc_mii_read(TC90XState *s)
{
    uint16_t val = s->phys_mgmt;

    if (s->mii_state == MII_DATA_OUT && !(s->phys_mgmt & 4)) {
        /*
         * The driver samples the data line while the clock is low, before
         * each rising edge: after the two turnaround bits come the 16 data
         * bits, most significant first.
         */
        int bit = s->mii_bits - 2;

        val &= ~2;
        if (bit >= 0 && bit < 16 && (s->mii_out & BIT(15 - bit))) {
            val |= 2;
        }
    }
    return val;
}

/* EEPROM */

static void tc_fill_eeprom(TC90XState *s)
{
    const uint8_t *mac = s->conf.macaddr.a;
    uint16_t sum = 0;
    int i;

    memset(s->eeprom, 0, sizeof(s->eeprom));
    for (i = 0; i < 3; i++) {
        /* the 3Com node address, in big endian words */
        s->eeprom[i] = mac[i * 2] << 8 | mac[i * 2 + 1];
        s->eeprom[10 + i] = s->eeprom[i];
    }
    s->eeprom[3] = 0x9200;          /* product ID */
    s->eeprom[7] = 0x6d50;          /* 3Com */
    s->eeprom[8] = 0x2940;          /* PCI parameters */
    s->eeprom[13] = 0x0010;         /* software information */
    s->eeprom[14] = 0x0000;
    s->eeprom[16] = 0x0010;         /* software information 2 */
    s->eeprom[18] = 0x2000;         /* capabilities */
    /*
     * Linux wants words 0x00-0x17 to XOR to 0; the AmigaOne firmware
     * wants words 0x00-0x20 to XOR to 0xbb, folded to a byte.
     */
    for (i = 0; i < 0x17; i++) {
        sum ^= s->eeprom[i];
    }
    s->eeprom[0x17] = sum;
    s->eeprom[0x20] = 0x00bb;
}

static void tc_eeprom_cmd(TC90XState *s, uint16_t val)
{
    int addr = val & 0x3f;

    s->eeprom_cmd = val & ~BIT(15);     /* never busy */
    switch ((val >> 6) & 3) {
    case 2: /* read */
        s->eeprom_data = s->eeprom[addr];
        break;
    default:
        /* writes and erases are accepted and ignored */
        break;
    }
}

/* Resets */

static void tc_rx_reset(TC90XState *s)
{
    s->rx_enabled = false;
    s->up_list_ptr = 0;
    s->up_pkt_status = 0;
    s->up_stalled = false;
    s->rx_filter = 0;
}

static void tc_tx_reset(TC90XState *s)
{
    s->tx_enabled = false;
    s->tx_status = 0;
    s->dn_list_ptr = 0;
    s->dn_last = 0;
    s->dn_stalled = false;
}

static void tc_soft_reset(TC90XState *s)
{
    tc_rx_reset(s);
    tc_tx_reset(s);
    s->window = 0;
    s->int_status = 0;
    s->int_enable = 0;
    s->status_enable = 0;
    s->dma_ctrl = 0;
    s->dn_poll = 0;
    s->up_poll = 0;
    memset(s->hash, 0, sizeof(s->hash));
    memcpy(s->station, s->conf.macaddr.a, 6);
    memset(s->station_mask, 0, 6);
    /* transceiver: autonegotiation, autoselect */
    s->internal_config = 8 << 20 | BIT(24);
    s->max_pkt_size = TC_MAX_FRAME;
    s->mac_control = BIT(5);    /* full duplex */
    s->media_status = 0x0800;   /* link beat */
    s->phys_mgmt = 0;
    s->mii_state = MII_IDLE;
    s->mii_bits = 0;
    s->mii_shift = 0;
    tc_phy_reset(s);
    timer_del(s->poll_timer);
    tc_update_irq(s);
}

static void tc_reset(DeviceState *dev)
{
    tc_soft_reset(TC90X(dev));
}

/* Download engine */

static void tc_poll_arm(TC90XState *s)
{
    if ((s->dn_poll && s->dn_last && !s->dn_list_ptr) ||
        (s->up_poll && s->up_list_ptr)) {
        timer_mod(s->poll_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TC_POLL_NS);
    }
}

static void tc_download(TC90XState *s)
{
    uint8_t frame[TC_MAX_FRAME + 64];
    int n = 0;

    while (s->dn_list_ptr && !s->dn_stalled && n++ < 256) {
        uint32_t desc = s->dn_list_ptr;
        uint32_t next = 0, fsh = 0, len = 0;
        int i;

        ldl_le_pci_dma(&s->dev, desc, &next, MEMTXATTRS_UNSPECIFIED);
        ldl_le_pci_dma(&s->dev, desc + 4, &fsh, MEMTXATTRS_UNSPECIFIED);
        if (fsh & DPD_DN_COMPLETE) {
            /* already sent: the driver has not reused it yet */
            break;
        }
        for (i = 0; i < TC_MAX_FRAGS; i++) {
            uint32_t addr = 0, flen = FRAG_LAST;

            ldl_le_pci_dma(&s->dev, desc + 8 + i * 8, &addr,
                           MEMTXATTRS_UNSPECIFIED);
            ldl_le_pci_dma(&s->dev, desc + 12 + i * 8, &flen,
                           MEMTXATTRS_UNSPECIFIED);
            if ((flen & 0x1fff) && len < sizeof(frame)) {
                uint32_t copy = MIN(flen & 0x1fff, sizeof(frame) - len);

                pci_dma_read(&s->dev, addr, frame + len, copy);
                len += copy;
            }
            if (flen & FRAG_LAST) {
                break;
            }
        }
        if (s->tx_enabled && len) {
            if (fsh & (DPD_ADD_IP_CKSUM | DPD_ADD_TCP_CKSUM |
                       DPD_ADD_UDP_CKSUM)) {
                net_checksum_calculate(frame, len, CSUM_ALL);
            }
            trace_tc90x_tx(len, desc);
            qemu_send_packet(qemu_get_queue(s->nic), frame, len);
        }
        stl_le_pci_dma(&s->dev, desc + 4, fsh | DPD_DN_COMPLETE,
                       MEMTXATTRS_UNSPECIFIED);
        if (fsh & DPD_TX_INDICATE) {
            s->tx_status = TX_STATUS_COMPLETE;
            tc_raise(s, INT_TX_COMPLETE);
        }
        if (fsh & DPD_DN_INDICATE) {
            s->dma_ctrl |= DMA_DN_COMPLETE;
            tc_raise(s, INT_DN_COMPLETE);
        }
        s->dn_last = desc;
        s->dn_list_ptr = next;
    }
    tc_poll_arm(s);
}

/* Upload engine */

static bool tc_rx_ready(TC90XState *s)
{
    uint32_t status = UPD_UP_COMPLETE;

    if (!s->rx_enabled || s->up_stalled || !s->up_list_ptr ||
        !(s->dev.config[PCI_COMMAND] & PCI_COMMAND_MASTER)) {
        return false;
    }
    ldl_le_pci_dma(&s->dev, s->up_list_ptr + 4, &status,
                   MEMTXATTRS_UNSPECIFIED);
    return !(status & UPD_UP_COMPLETE);
}

static bool tc_can_receive(NetClientState *nc)
{
    TC90XState *s = qemu_get_nic_opaque(nc);
    bool ready = tc_rx_ready(s);

    if (!ready && s->rx_enabled) {
        /* the driver may give back descriptors without telling us */
        tc_poll_arm(s);
    }
    return ready;
}

static bool tc_rx_accept(TC90XState *s, const uint8_t *buf)
{
    if (s->rx_filter & FILTER_PROMISC) {
        return true;
    }
    if (is_broadcast_ether_addr(buf)) {
        return s->rx_filter & FILTER_BROADCAST;
    }
    if (is_multicast_ether_addr(buf)) {
        if (s->rx_filter & FILTER_MULTICAST) {
            return true;
        }
        if (s->rx_filter & FILTER_HASH) {
            uint32_t crc = net_crc32(buf, ETH_ALEN) & 0xff;

            return s->hash[crc / 8] & BIT(crc % 8);
        }
        return false;
    }
    if (s->rx_filter & FILTER_STATION) {
        int i;

        for (i = 0; i < 6; i++) {
            if ((buf[i] ^ s->station[i]) & ~s->station_mask[i]) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static ssize_t tc_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    TC90XState *s = qemu_get_nic_opaque(nc);
    uint32_t desc = s->up_list_ptr, done = 0, next = 0;
    int i;

    if (!tc_rx_ready(s)) {
        return 0;
    }
    if (!tc_rx_accept(s, buf)) {
        return size;
    }
    size = MIN(size, TC_MAX_FRAME);
    ldl_le_pci_dma(&s->dev, desc, &next, MEMTXATTRS_UNSPECIFIED);
    for (i = 0; i < TC_MAX_FRAGS && done < size; i++) {
        uint32_t addr = 0, flen = FRAG_LAST, copy;

        ldl_le_pci_dma(&s->dev, desc + 8 + i * 8, &addr,
                       MEMTXATTRS_UNSPECIFIED);
        ldl_le_pci_dma(&s->dev, desc + 12 + i * 8, &flen,
                       MEMTXATTRS_UNSPECIFIED);
        copy = MIN(flen & 0x1fff, size - done);
        pci_dma_write(&s->dev, addr, buf + done, copy);
        done += copy;
        if (flen & FRAG_LAST) {
            break;
        }
    }
    s->up_pkt_status = (done & 0x1fff) | UPD_UP_COMPLETE |
                       (done < size ? UPD_UP_ERROR : 0);
    stl_le_pci_dma(&s->dev, desc + 4, s->up_pkt_status,
                   MEMTXATTRS_UNSPECIFIED);
    s->up_list_ptr = next;
    s->dma_ctrl |= DMA_UP_COMPLETE;
    trace_tc90x_rx(done, desc);
    tc_raise(s, INT_UP_COMPLETE | INT_RX_COMPLETE);
    return size;
}

static void tc_poll(void *opaque)
{
    TC90XState *s = opaque;

    if (s->dn_poll && s->dn_last && !s->dn_list_ptr) {
        uint32_t next = 0;

        ldl_le_pci_dma(&s->dev, s->dn_last, &next, MEMTXATTRS_UNSPECIFIED);
        if (next) {
            s->dn_list_ptr = next;
            tc_download(s);
        }
    }
    if (tc_rx_ready(s)) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    } else {
        tc_poll_arm(s);
    }
}

/* Registers */

static void tc_command(TC90XState *s, uint16_t val)
{
    uint16_t arg = val & 0x7ff;

    trace_tc90x_command(val >> 11, arg);
    switch (val >> 11) {
    case CMD_TOTAL_RESET:
        tc_soft_reset(s);
        break;
    case CMD_SELECT_WINDOW:
        s->window = arg & 7;
        break;
    case CMD_RX_DISABLE:
        s->rx_enabled = false;
        break;
    case CMD_RX_ENABLE:
        s->rx_enabled = true;
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        break;
    case CMD_RX_RESET:
        tc_rx_reset(s);
        break;
    case CMD_STALL:
        switch (arg) {
        case STALL_UP:
            s->up_stalled = true;
            break;
        case STALL_UP_UNSTALL:
            s->up_stalled = false;
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
            break;
        case STALL_DOWN:
            s->dn_stalled = true;
            break;
        case STALL_DOWN_UNSTALL:
            s->dn_stalled = false;
            tc_download(s);
            break;
        }
        break;
    case CMD_RX_DISCARD:
        break;
    case CMD_TX_ENABLE:
        s->tx_enabled = true;
        tc_download(s);
        break;
    case CMD_TX_DISABLE:
        s->tx_enabled = false;
        break;
    case CMD_TX_RESET:
        tc_tx_reset(s);
        break;
    case CMD_FAKE_INTR:
        tc_raise(s, INT_REQUESTED);
        break;
    case CMD_ACK_INTR:
        s->int_status &= ~(arg & INT_ACK_MASK);
        if (arg & INT_DN_COMPLETE) {
            s->dma_ctrl &= ~DMA_DN_COMPLETE;
        }
        if (arg & INT_UP_COMPLETE) {
            s->dma_ctrl &= ~DMA_UP_COMPLETE;
        }
        tc_update_irq(s);
        /* the driver gives buffers back before it acknowledges */
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        break;
    case CMD_SET_INTR_ENB:
        s->int_enable = arg;
        tc_update_irq(s);
        break;
    case CMD_SET_STATUS_ENB:
        s->status_enable = arg;
        s->int_status &= arg | INT_LATCH;
        tc_update_irq(s);
        break;
    case CMD_SET_RX_FILTER:
        s->rx_filter = arg & 0x1f;
        break;
    case CMD_SET_RX_THRESHOLD:
    case CMD_SET_TX_THRESHOLD:
    case CMD_SET_TX_START:
    case CMD_STATS_ENABLE:
    case CMD_STATS_DISABLE:
    case CMD_STOP_COAX:
        break;
    case CMD_SET_HASH_BIT:
    {
        int bit = arg & 0xff;

        if (arg & BIT(10)) {
            s->hash[bit / 8] |= BIT(bit % 8);
        } else {
            s->hash[bit / 8] &= ~BIT(bit % 8);
        }
        break;
    }
    case CMD_DMA:
        qemu_log_mask(LOG_UNIMP, "3c905c: single DMA transfers\n");
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "3c905c: command %d\n", val >> 11);
        break;
    }
}

/* Windowed registers, 0x00-0x0d */
static uint32_t tc_window_read(TC90XState *s, hwaddr addr)
{
    switch (s->window << 8 | addr) {
    case 0x00a:
        return s->eeprom_cmd;
    case 0x00c:
        return s->eeprom_data;
    case 0x102:
        /* the AmigaOne firmware waits here for negotiation to finish */
        return 0x0004;
    case 0x200 ... 0x205:
        return s->station[addr];
    case 0x206 ... 0x20b:
        return s->station_mask[addr - 6];
    case 0x20c:
    case 0x308:
        /* reset options and media options: MII, 100BASE-TX, 10BASE-T */
        return 0x4a;
    case 0x300:
        return s->internal_config & 0xffff;
    case 0x302:
        return s->internal_config >> 16;
    case 0x304:
        return s->max_pkt_size;
    case 0x306:
        return s->mac_control;
    case 0x30a:
        return 0x2000;      /* free receive bytes */
    case 0x30c:
        return 0x2000;      /* free transmit bytes */
    case 0x406:
        return 0x0800 | (s->mac_control & BIT(5) ? BIT(11) : 0);
    case 0x408:
        return tc_mii_read(s);
    case 0x40a:
        return s->media_status;
    case 0x500:
        return s->tx_start;
    case 0x506:
        return s->rx_early;
    case 0x508:
        return s->rx_filter;
    case 0x509:
        return s->tx_reclaim;
    case 0x50a:
        return s->int_enable;
    case 0x50c:
        return s->status_enable;
    default:
        return 0;   /* statistics and unused registers */
    }
}

static void tc_window_write(TC90XState *s, hwaddr addr, uint32_t val)
{
    switch (s->window << 8 | addr) {
    case 0x00a:
        tc_eeprom_cmd(s, val);
        break;
    case 0x200 ... 0x205:
        s->station[addr] = val;
        break;
    case 0x206 ... 0x20b:
        s->station_mask[addr - 6] = val;
        break;
    case 0x300:
        s->internal_config = (s->internal_config & 0xffff0000) | (val & 0xffff);
        break;
    case 0x302:
        s->internal_config = (s->internal_config & 0xffff) | val << 16;
        break;
    case 0x304:
        s->max_pkt_size = val;
        break;
    case 0x306:
        s->mac_control = val;
        break;
    case 0x408:
        tc_mii_write(s, val);
        break;
    case 0x40a:
        s->media_status = (s->media_status & 0x0800) | (val & ~0x0800);
        break;
    case 0x500:
        s->tx_start = val;
        break;
    case 0x506:
        s->rx_early = val;
        break;
    case 0x509:
        s->tx_reclaim = val;
        break;
    default:
        break;
    }
}

static uint32_t tc_reg_read(TC90XState *s, hwaddr addr)
{
    switch (addr) {
    case 0x0e:
    case R_INT_STATUS_AUTO:
        return s->int_status | s->window << 13;
    case R_TX_STATUS:
        return s->tx_status;
    case R_DMA_CTRL:
        return s->dma_ctrl | (s->dn_stalled ? DMA_DN_STALLED : 0) |
               (s->dn_list_ptr ? DMA_DN_IN_PROG : 0);
    case R_DN_LIST_PTR:
        return s->dn_list_ptr;
    case R_DN_POLL:
        return s->dn_poll;
    case R_UP_PKT_STATUS:
        return s->up_pkt_status;
    case R_UP_LIST_PTR:
        return s->up_list_ptr;
    case R_UP_POLL:
        return s->up_poll;
    case R_TIMER:
    case R_FREE_TIMER:
    case R_COUNTDOWN:
        return 0;
    default:
        if (addr < 0x0e) {
            return tc_window_read(s, addr);
        }
        return 0;
    }
}

static void tc_reg_write(TC90XState *s, hwaddr addr, uint32_t val)
{
    switch (addr) {
    case 0x0e:
        tc_command(s, val);
        break;
    case R_TX_STATUS:
        /* writing pops the transmit status */
        s->tx_status = 0;
        s->int_status &= ~INT_TX_COMPLETE;
        tc_update_irq(s);
        break;
    case R_DMA_CTRL:
        s->dma_ctrl = (s->dma_ctrl & (DMA_UP_COMPLETE | DMA_DN_COMPLETE)) |
                      (val & ~(DMA_UP_COMPLETE | DMA_DN_COMPLETE |
                               DMA_DN_STALLED | DMA_DN_IN_PROG));
        break;
    case R_DN_LIST_PTR:
        s->dn_list_ptr = val & ~3;
        tc_download(s);
        break;
    case R_DN_POLL:
        s->dn_poll = val;
        tc_poll_arm(s);
        break;
    case R_UP_LIST_PTR:
        s->up_list_ptr = val & ~3;
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        break;
    case R_UP_POLL:
        s->up_poll = val;
        tc_poll_arm(s);
        break;
    default:
        if (addr < 0x0e) {
            tc_window_write(s, addr, val);
        }
        break;
    }
}

/*
 * Registers are read and written in their natural sizes; split other
 * accesses into the registers they cover.
 */
static unsigned tc_reg_size(TC90XState *s, hwaddr addr)
{
    switch (addr) {
    case R_DMA_CTRL:
    case R_DN_LIST_PTR:
    case R_UP_PKT_STATUS:
    case R_UP_LIST_PTR:
        return 4;
    case R_TX_STATUS:
    case R_TIMER:
    case R_DN_POLL:
    case R_UP_POLL:
        return 1;
    case 0x200 ... 0x20b:
        return 1;
    default:
        return 2;
    }
}

static uint64_t tc_read(void *opaque, hwaddr addr, unsigned size)
{
    TC90XState *s = opaque;
    uint64_t val = 0;
    unsigned done = 0;

    while (done < size) {
        hwaddr a = addr + done;
        hwaddr key = a < 0x0e ? (s->window << 8 | a) : a;
        unsigned rs = tc_reg_size(s, key);
        hwaddr base = a & ~(hwaddr)(rs - 1);
        uint32_t r = tc_reg_read(s, base);

        r >>= (a - base) * 8;
        rs = MIN(rs - (a - base), size - done);
        val |= (uint64_t)(r & MAKE_64BIT_MASK(0, rs * 8)) << (done * 8);
        done += rs;
    }
    trace_tc90x_read(s->window, addr, size, val);
    return val;
}

static void tc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TC90XState *s = opaque;
    unsigned done = 0;

    trace_tc90x_write(s->window, addr, size, val);
    while (done < size) {
        hwaddr a = addr + done;
        hwaddr key = a < 0x0e ? (s->window << 8 | a) : a;
        unsigned rs = tc_reg_size(s, key);
        hwaddr base = a & ~(hwaddr)(rs - 1);
        uint32_t cur = 0;

        if (rs > size - done || base != a) {
            /* a partial write: keep the other bytes of the register */
            cur = tc_reg_read(s, base);
        }
        rs = MIN(rs - (a - base), size - done);
        cur = deposit32(cur, (a - base) * 8, rs * 8, val >> (done * 8));
        tc_reg_write(s, base, cur);
        done += rs;
    }
}

static const MemoryRegionOps tc_ops = {
    .read = tc_read,
    .write = tc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void tc_set_link_status(NetClientState *nc)
{
    TC90XState *s = qemu_get_nic_opaque(nc);

    if (nc->link_down) {
        s->media_status &= ~0x0800;
        s->phy[1] &= ~0x0004;
    } else {
        s->media_status |= 0x0800;
        s->phy[1] |= 0x0004;
    }
    tc_raise(s, INT_LINK_EVENT);
}

static NetClientInfo net_tc_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = tc_can_receive,
    .receive = tc_receive,
    .link_status_changed = tc_set_link_status,
};

static void tc_realize(PCIDevice *pci_dev, Error **errp)
{
    TC90XState *s = TC90X(pci_dev);

    pci_dev->config[PCI_INTERRUPT_PIN] = 1;
    pci_dev->config[PCI_MIN_GNT] = 0x0a;
    pci_dev->config[PCI_MAX_LAT] = 0x0a;

    memory_region_init_io(&s->io, OBJECT(s), &tc_ops, s, "3c905c-io",
                          TC_IO_SIZE);
    memory_region_init_io(&s->mmio, OBJECT(s), &tc_ops, s, "3c905c-mmio",
                          TC_IO_SIZE);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    s->poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tc_poll, s);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    tc_fill_eeprom(s);
    s->nic = qemu_new_nic(&net_tc_info, &s->conf,
                          object_get_typename(OBJECT(pci_dev)),
                          pci_dev->qdev.id,
                          &pci_dev->qdev.mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void tc_exit(PCIDevice *pci_dev)
{
    TC90XState *s = TC90X(pci_dev);

    timer_free(s->poll_timer);
    qemu_del_nic(s->nic);
}

static const VMStateDescription vmstate_tc90x = {
    .name = "3c905c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, TC90XState),
        VMSTATE_TIMER_PTR(poll_timer, TC90XState),
        VMSTATE_UINT8(window, TC90XState),
        VMSTATE_UINT16(int_status, TC90XState),
        VMSTATE_UINT16(int_enable, TC90XState),
        VMSTATE_UINT16(status_enable, TC90XState),
        VMSTATE_UINT8(rx_filter, TC90XState),
        VMSTATE_BOOL(rx_enabled, TC90XState),
        VMSTATE_BOOL(tx_enabled, TC90XState),
        VMSTATE_BOOL(up_stalled, TC90XState),
        VMSTATE_BOOL(dn_stalled, TC90XState),
        VMSTATE_UINT8(tx_status, TC90XState),
        VMSTATE_UINT32(dma_ctrl, TC90XState),
        VMSTATE_UINT32(dn_list_ptr, TC90XState),
        VMSTATE_UINT32(dn_last, TC90XState),
        VMSTATE_UINT8(dn_poll, TC90XState),
        VMSTATE_UINT32(up_list_ptr, TC90XState),
        VMSTATE_UINT32(up_pkt_status, TC90XState),
        VMSTATE_UINT8(up_poll, TC90XState),
        VMSTATE_UINT8_ARRAY(hash, TC90XState, 32),
        VMSTATE_UINT8_ARRAY(station, TC90XState, 6),
        VMSTATE_UINT8_ARRAY(station_mask, TC90XState, 6),
        VMSTATE_UINT32(internal_config, TC90XState),
        VMSTATE_UINT16(max_pkt_size, TC90XState),
        VMSTATE_UINT16(mac_control, TC90XState),
        VMSTATE_UINT16(media_status, TC90XState),
        VMSTATE_UINT16(rx_early, TC90XState),
        VMSTATE_UINT16(tx_start, TC90XState),
        VMSTATE_UINT16(tx_reclaim, TC90XState),
        VMSTATE_UINT16_ARRAY(eeprom, TC90XState, TC_EEPROM_WORDS),
        VMSTATE_UINT16(eeprom_cmd, TC90XState),
        VMSTATE_UINT16(eeprom_data, TC90XState),
        VMSTATE_UINT16(phys_mgmt, TC90XState),
        VMSTATE_UINT32(mii_shift, TC90XState),
        VMSTATE_INT32(mii_bits, TC90XState),
        VMSTATE_INT32(mii_state, TC90XState),
        VMSTATE_UINT8(mii_phy, TC90XState),
        VMSTATE_UINT8(mii_reg, TC90XState),
        VMSTATE_BOOL(mii_read, TC90XState),
        VMSTATE_UINT16(mii_out, TC90XState),
        VMSTATE_UINT16_ARRAY(phy, TC90XState, 32),
        VMSTATE_END_OF_LIST()
    }
};

static const Property tc_properties[] = {
    DEFINE_NIC_PROPERTIES(TC90XState, conf),
};

static void tc_instance_init(Object *obj)
{
    TC90XState *s = TC90X(obj);

    device_add_bootindex_property(obj, &s->conf.bootindex, "bootindex",
                                  "/ethernet-phy@0", DEVICE(obj));
}

static void tc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = tc_realize;
    k->exit = tc_exit;
    k->vendor_id = PCI_VENDOR_ID_3COM;
    k->device_id = 0x9200;
    k->revision = 0x74;
    k->subsystem_vendor_id = PCI_VENDOR_ID_3COM;
    k->subsystem_id = 0x1000;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    dc->desc = "3Com 3C905C/3C920 Fast EtherLink XL";
    dc->vmsd = &vmstate_tc90x;
    device_class_set_props(dc, tc_properties);
    device_class_set_legacy_reset(dc, tc_reset);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo tc_info = {
    .name = TYPE_TC90X,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(TC90XState),
    .class_init = tc_class_init,
    .instance_init = tc_instance_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void tc_register_types(void)
{
    type_register_static(&tc_info);
}

type_init(tc_register_types)
