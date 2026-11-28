/*
 * VIA VT6102 Rhine-II 10/100 Ethernet controller
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

/*
 * Pegasos II has this controller on board, next to the VT8231. The chip
 * fetches frames through two lists of 16 byte descriptors in host memory,
 * which the driver usually links into rings. A frame may span several
 * descriptors, chained with the STP and EDP bits. The registers follow
 * the VT6102 data sheet; the PHY is a simple one that always has a
 * 100 Mbit/s full duplex link.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/nvram/eeprom93xx.h"
#include "migration/vmstate.h"
#include "system/system.h"
#include "net/net.h"
#include "net/eth.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_VIA_RHINE "via-rhine"
OBJECT_DECLARE_SIMPLE_TYPE(ViaRhineState, VIA_RHINE)

#define RHINE_REGS_SIZE         0x100

/* registers */
#define R_PAR                   0x00
#define R_RCR                   0x06
#define R_TCR                   0x07
#define R_CR0                   0x08
#define R_CR1                   0x09
#define R_ISR                   0x0c
#define R_IMR                   0x0e
#define R_MAR                   0x10
#define R_CRDA                  0x18
#define R_CTDA                  0x1c
#define R_PHYADR                0x6c
#define R_MIISR                 0x6d
#define R_MIICR                 0x70
#define R_MIIADR                0x71
#define R_MIIDATA               0x72
#define R_EECSR                 0x74
#define R_CFGD                  0x7b
#define R_TALLY_MPA             0x7c
#define R_TALLY_CRC             0x7e
#define R_MISCCR1               0x81
#define R_MISR                  0x84
#define R_MIMR                  0x86

/* RCR */
#define RCR_AM                  BIT(2)
#define RCR_AB                  BIT(3)
#define RCR_PROM                BIT(4)

/* CR0 and CR1 */
#define CR0_INIT                BIT(0)
#define CR0_STRT                BIT(1)
#define CR0_STOP                BIT(2)
#define CR0_RXON                BIT(3)
#define CR0_TXON                BIT(4)
#define CR0_TDMD                BIT(5)
#define CR0_RDMD                BIT(6)
#define CR1_TDMD1               BIT(5)
#define CR1_RDMD1               BIT(6)
#define CR1_SFRST               BIT(7)

/* ISR */
#define ISR_PRX                 BIT(0)
#define ISR_PTX                 BIT(1)
#define ISR_RU                  BIT(5)
#define ISR_NORBF               BIT(12)
#define ISR_SRCI                BIT(14)

/* MIICR */
#define MIICR_WCMD              BIT(5)
#define MIICR_RCMD              BIT(6)
#define MIICR_MAUTO             BIT(7)

/* MIIADR */
#define MIIADR_MDONE            BIT(5)
#define MIIADR_MIDLE            BIT(7)

/* EECSR */
#define EECSR_EDO               BIT(0)
#define EECSR_EDI               BIT(1)
#define EECSR_ECK               BIT(2)
#define EECSR_ECS               BIT(3)
#define EECSR_DPM               BIT(4)
#define EECSR_AUTOLD            BIT(5)
#define EECSR_EEPR              BIT(7)

/* MISCCR1 */
#define MISCCR1_FORSRST         BIT(6)

/* descriptors */
#define DESC_OWN                BIT(31)
#define RDES0_RXOK              BIT(15)
#define RDES0_MAR               BIT(13)
#define RDES0_BAR               BIT(12)
#define RDES0_PHY               BIT(11)
#define RDES0_CHN               BIT(10)
#define RDES0_STP               BIT(9)
#define RDES0_EDP               BIT(8)
#define DES1_SIZE(d)            ((d) & 0x7ff)
#define TDES1_STP               BIT(21)
#define TDES1_EDP               BIT(22)
#define TDES1_IC                BIT(23)
#define TDES3_NO_IRQ            BIT(0)

#define RHINE_MAX_FRAME         2048
#define RHINE_MAX_DESCS         256

struct ViaRhineState {
    PCIDevice dev;
    MemoryRegion io;
    MemoryRegion mmio;
    NICConf conf;
    NICState *nic;
    eeprom_t *eeprom;
    uint8_t regs[RHINE_REGS_SIZE];
    uint16_t phy[32];
    uint8_t tx_frame[RHINE_MAX_FRAME];
    uint32_t tx_len;
};

static uint16_t rhine_isr(ViaRhineState *s)
{
    return lduw_le_p(&s->regs[R_ISR]);
}

static void rhine_update_irq(ViaRhineState *s)
{
    bool level = (rhine_isr(s) & lduw_le_p(&s->regs[R_IMR])) ||
                 (s->regs[R_MISR] & s->regs[R_MIMR]);

    pci_set_irq(&s->dev, level);
}

static void rhine_raise(ViaRhineState *s, uint16_t bits)
{
    stw_le_p(&s->regs[R_ISR], rhine_isr(s) | bits);
    rhine_update_irq(s);
}

static void rhine_load_mac(ViaRhineState *s)
{
    memcpy(&s->regs[R_PAR], s->conf.macaddr.a, 6);
}

/* The 93C46 holds the MAC address and the configuration bytes */
static void rhine_fill_eeprom(ViaRhineState *s)
{
    uint16_t *w = eeprom93xx_data(s->eeprom);
    const uint8_t *mac = s->conf.macaddr.a;
    uint8_t sum = 0;
    int i;

    memset(w, 0, 64 * 2);
    w[0] = mac[0] | mac[1] << 8;
    w[1] = mac[2] | mac[3] << 8;
    w[2] = mac[4] | mac[5] << 8;
    w[3] = 0x01;                        /* PHY address */
    w[4] = pci_get_word(s->dev.config + PCI_SUBSYSTEM_ID);
    w[5] = pci_get_word(s->dev.config + PCI_SUBSYSTEM_VENDOR_ID);
    w[0xb] = 0x0306;                    /* Min_GNT, Max_LAT */
    w[0xf] = 0x73;                      /* the EEPROM is programmed */
    for (i = 0; i < 0x1f; i++) {
        sum += (w[i / 2] >> ((i & 1) * 8)) & 0xff;
    }
    w[0xf] |= (uint8_t)(0xff - sum) << 8;
}

static void rhine_phy_reset(ViaRhineState *s)
{
    memset(s->phy, 0, sizeof(s->phy));
    s->phy[0] = 0x3100;     /* auto negotiation, 100 Mbit/s, full duplex */
    s->phy[1] = 0x782d;     /* link up, negotiation complete */
    s->phy[2] = 0x0101;     /* VIA VT6103 */
    s->phy[3] = 0x8f20;
    s->phy[4] = 0x05e1;
    s->phy[5] = 0x45e1;
    s->phy[6] = 0x0001;
}

static void rhine_soft_reset(ViaRhineState *s)
{
    uint8_t keep[8];

    /* the configuration loaded from the EEPROM survives a soft reset */
    memcpy(keep, &s->regs[0x78], sizeof(keep));
    memset(s->regs, 0, sizeof(s->regs));
    memcpy(&s->regs[0x78], keep, sizeof(keep));
    rhine_load_mac(s);
    s->regs[R_CR0] = CR0_STOP;
    s->regs[R_TCR] = BIT(3);
    s->regs[R_PHYADR] = 0x01;
    s->regs[R_MIISR] = 0x00; /* 100 Mbit/s, link up */
    s->regs[R_MIIADR] = 0x01;
    s->regs[R_EECSR] = EECSR_EEPR;
    s->tx_len = 0;
    rhine_update_irq(s);
}

static void rhine_reset(DeviceState *dev)
{
    ViaRhineState *s = VIA_RHINE(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_CFGD] = BIT(7); /* memory mapped registers enabled */
    rhine_soft_reset(s);
    rhine_phy_reset(s);
}

/* Receive */

static bool rhine_rx_enabled(ViaRhineState *s)
{
    return (s->regs[R_CR0] & (CR0_STRT | CR0_RXON)) == (CR0_STRT | CR0_RXON)
           && !(s->regs[R_CR0] & CR0_STOP);
}

static bool rhine_can_receive(NetClientState *nc)
{
    ViaRhineState *s = qemu_get_nic_opaque(nc);
    uint32_t status = 0;

    if (!rhine_rx_enabled(s) || !(s->dev.config[PCI_COMMAND] &
                                  PCI_COMMAND_MASTER)) {
        return false;
    }
    ldl_le_pci_dma(&s->dev, ldl_le_p(&s->regs[R_CRDA]), &status,
                   MEMTXATTRS_UNSPECIFIED);
    return status & DESC_OWN;
}

static uint32_t rhine_rx_filter(ViaRhineState *s, const uint8_t *buf)
{
    uint8_t rcr = s->regs[R_RCR];

    if (!memcmp(buf, &s->regs[R_PAR], 6)) {
        return RDES0_PHY;
    }
    if (is_broadcast_ether_addr(buf)) {
        return rcr & (RCR_AB | RCR_PROM) ? RDES0_BAR : 0;
    }
    if (is_multicast_ether_addr(buf)) {
        if (rcr & RCR_PROM) {
            return RDES0_MAR;
        }
        if (rcr & RCR_AM) {
            uint32_t crc = net_crc32(buf, ETH_ALEN) >> 26;

            if (s->regs[R_MAR + crc / 8] & BIT(crc % 8)) {
                return RDES0_MAR;
            }
        }
        return 0;
    }
    return rcr & RCR_PROM ? RDES0_PHY : 0;
}

static ssize_t rhine_receive(NetClientState *nc, const uint8_t *buf,
                             size_t size)
{
    ViaRhineState *s = qemu_get_nic_opaque(nc);
    uint8_t frame[RHINE_MAX_FRAME];
    uint32_t addr_bits, len, done = 0, n = 0;
    uint32_t desc = ldl_le_p(&s->regs[R_CRDA]);

    if (!rhine_rx_enabled(s)) {
        return -1;
    }
    addr_bits = rhine_rx_filter(s, buf);
    if (!addr_bits) {
        return size; /* not for us */
    }
    /* frames come without the CRC, add room for it and pad short ones */
    len = MIN(MAX(size, 60), RHINE_MAX_FRAME - 4);
    memset(frame, 0, sizeof(frame));
    memcpy(frame, buf, MIN(size, len));
    len += 4;

    while (done < len && n++ < RHINE_MAX_DESCS) {
        uint32_t d[4], chunk;

        pci_dma_read(&s->dev, desc, d, sizeof(d));
        d[0] = le32_to_cpu(d[0]);
        d[1] = le32_to_cpu(d[1]);
        d[2] = le32_to_cpu(d[2]);
        d[3] = le32_to_cpu(d[3]);
        if (!(d[0] & DESC_OWN)) {
            /* out of buffers: the rest of the frame is lost */
            rhine_raise(s, ISR_NORBF | ISR_RU);
            trace_via_rhine_rx_nobuf(desc);
            return done ? size : 0;
        }
        chunk = MIN(DES1_SIZE(d[1]), len - done);
        pci_dma_write(&s->dev, d[2], frame + done, chunk);
        d[0] = (done == 0 ? RDES0_STP : 0) | RDES0_CHN;
        done += chunk;
        if (done >= len) {
            d[0] |= RDES0_EDP | RDES0_RXOK | addr_bits | (len << 16);
        }
        stl_le_pci_dma(&s->dev, desc, d[0], MEMTXATTRS_UNSPECIFIED);
        desc = d[3];
        stl_le_p(&s->regs[R_CRDA], desc);
        if (!chunk && done < len) {
            break; /* a zero sized buffer would never finish the frame */
        }
    }
    trace_via_rhine_rx(len, desc);
    rhine_raise(s, ISR_PRX);
    return size;
}

/* Transmit */

static void rhine_transmit(ViaRhineState *s)
{
    uint32_t desc = ldl_le_p(&s->regs[R_CTDA]);
    bool irq = false;
    int n = 0;

    if (!(s->regs[R_CR0] & CR0_TXON) || (s->regs[R_CR0] & CR0_STOP)) {
        return;
    }
    while (n++ < RHINE_MAX_DESCS) {
        uint32_t d[4], size;

        pci_dma_read(&s->dev, desc, d, sizeof(d));
        d[0] = le32_to_cpu(d[0]);
        d[1] = le32_to_cpu(d[1]);
        d[2] = le32_to_cpu(d[2]);
        d[3] = le32_to_cpu(d[3]);
        if (!(d[0] & DESC_OWN)) {
            break;
        }
        if (d[1] & TDES1_STP) {
            s->tx_len = 0;
        }
        size = MIN(DES1_SIZE(d[1]), RHINE_MAX_FRAME - s->tx_len);
        pci_dma_read(&s->dev, d[2], s->tx_frame + s->tx_len, size);
        s->tx_len += size;
        if ((d[1] & TDES1_EDP) || !(d[1] & (TDES1_STP | TDES1_EDP))) {
            trace_via_rhine_tx(s->tx_len, desc);
            qemu_send_packet(qemu_get_queue(s->nic), s->tx_frame, s->tx_len);
            s->tx_len = 0;
            if (!(d[3] & TDES3_NO_IRQ)) {
                irq = true;
            }
        }
        if (d[1] & TDES1_IC) {
            irq = true;
        }
        /* done, no errors, no collisions */
        stl_le_pci_dma(&s->dev, desc, 0, MEMTXATTRS_UNSPECIFIED);
        desc = d[3] & ~0xf;
        stl_le_p(&s->regs[R_CTDA], desc);
    }
    if (irq) {
        rhine_raise(s, ISR_PTX);
    }
}

/* Registers */

static void rhine_mii(ViaRhineState *s, uint8_t cmd)
{
    int reg = s->regs[R_MIIADR] & 0x1f;
    bool ours = (s->regs[R_PHYADR] & 0x1f) == 1;

    if (cmd & MIICR_RCMD) {
        stw_le_p(&s->regs[R_MIIDATA], ours ? s->phy[reg] : 0xffff);
    } else if (cmd & MIICR_WCMD && ours) {
        uint16_t v = lduw_le_p(&s->regs[R_MIIDATA]);

        switch (reg) {
        case 0:
            if (v & 0x8000) {
                rhine_phy_reset(s);
            } else {
                s->phy[0] = v & ~0x0200; /* restart negotiation self clears */
            }
            break;
        case 4:
            s->phy[4] = v;
            break;
        default:
            break;
        }
    }
    s->regs[R_MIICR] = cmd & ~(MIICR_RCMD | MIICR_WCMD);
}

static uint64_t rhine_read(void *opaque, hwaddr addr, unsigned size)
{
    ViaRhineState *s = opaque;
    uint64_t val;

    if (addr + size > RHINE_REGS_SIZE) {
        return 0;
    }
    if (addr <= R_EECSR && addr + size > R_EECSR &&
        (s->regs[R_EECSR] & EECSR_DPM)) {
        s->regs[R_EECSR] = (s->regs[R_EECSR] & ~EECSR_EDO) |
                           (eeprom93xx_read(s->eeprom) ? EECSR_EDO : 0);
    }
    if (addr <= R_MIIADR && addr + size > R_MIIADR) {
        /*
         * With auto polling off the management port is idle. With it on,
         * a polling cycle of the PHY is always just done.
         */
        if (s->regs[R_MIICR] & MIICR_MAUTO) {
            s->regs[R_MIIADR] = (s->regs[R_MIIADR] & ~MIIADR_MIDLE) |
                                MIIADR_MDONE;
        } else {
            s->regs[R_MIIADR] |= MIIADR_MIDLE;
        }
    }
    val = ldn_le_p(&s->regs[addr], size);
    trace_via_rhine_read(addr, size, val);
    return val;
}

static void rhine_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ViaRhineState *s = opaque;
    unsigned i;

    if (addr + size > RHINE_REGS_SIZE) {
        return;
    }
    trace_via_rhine_write(addr, size, val);
    for (i = 0; i < size; i++) {
        uint8_t b = val >> (i * 8);
        hwaddr a = addr + i;

        switch (a) {
        case R_CR0:
            if (b & CR0_STRT) {
                b &= ~CR0_STOP;
            }
            if (b & CR0_STOP) {
                b &= ~(CR0_STRT | CR0_RXON | CR0_TXON);
            }
            s->regs[a] = b & ~(CR0_TDMD | CR0_RDMD | CR0_INIT);
            if (b & (CR0_TDMD | CR0_TXON)) {
                rhine_transmit(s);
            }
            if (b & (CR0_RDMD | CR0_RXON)) {
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
            break;
        case R_CR1:
            if (b & CR1_SFRST) {
                rhine_soft_reset(s);
                break;
            }
            s->regs[a] = b & ~(CR1_TDMD1 | CR1_RDMD1);
            if (b & CR1_TDMD1) {
                rhine_transmit(s);
            }
            if (b & CR1_RDMD1) {
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
            break;
        case R_ISR:
        case R_ISR + 1:
        case R_MISR:
            /* write one to clear */
            s->regs[a] &= ~b;
            break;
        case R_MIICR:
            rhine_mii(s, b);
            break;
        case R_MIIADR:
            s->regs[a] = b & ~MIIADR_MIDLE;
            break;
        case R_MIISR:
            s->regs[a] = (s->regs[a] & 0x03) | (b & ~0x83);
            break;
        case R_EECSR:
            if (b & EECSR_AUTOLD) {
                rhine_load_mac(s);
            }
            if (b & EECSR_DPM) {
                /* direct programming: the driver drives the EEPROM pins */
                eeprom93xx_write(s->eeprom, !!(b & EECSR_ECS),
                                 !!(b & EECSR_ECK), !!(b & EECSR_EDI));
            }
            s->regs[a] = (b & 0x1e) | EECSR_EEPR |
                         (eeprom93xx_read(s->eeprom) ? EECSR_EDO : 0);
            break;
        case R_MISCCR1:
            if (b & MISCCR1_FORSRST) {
                rhine_soft_reset(s);
                break;
            }
            s->regs[a] = b;
            break;
        case R_CRDA ... R_CRDA + 3:
            s->regs[a] = b;
            if (a == R_CRDA + 3) {
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
            break;
        default:
            s->regs[a] = b;
            break;
        }
    }
    rhine_update_irq(s);
}

static const MemoryRegionOps rhine_ops = {
    .read = rhine_read,
    .write = rhine_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void rhine_set_link_status(NetClientState *nc)
{
    ViaRhineState *s = qemu_get_nic_opaque(nc);

    if (nc->link_down) {
        s->regs[R_MIISR] |= BIT(1);
        s->phy[1] &= ~0x0004;
    } else {
        s->regs[R_MIISR] &= ~BIT(1);
        s->phy[1] |= 0x0004;
    }
    rhine_raise(s, ISR_SRCI);
}

static NetClientInfo net_rhine_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = rhine_can_receive,
    .receive = rhine_receive,
    .link_status_changed = rhine_set_link_status,
};

static void rhine_realize(PCIDevice *pci_dev, Error **errp)
{
    ViaRhineState *s = VIA_RHINE(pci_dev);

    pci_dev->config[PCI_INTERRUPT_PIN] = 1;
    pci_set_word(pci_dev->config + PCI_STATUS,
                 PCI_STATUS_FAST_BACK | PCI_STATUS_DEVSEL_MEDIUM);

    memory_region_init_io(&s->io, OBJECT(s), &rhine_ops, s, "via-rhine-io",
                          RHINE_REGS_SIZE);
    memory_region_init_io(&s->mmio, OBJECT(s), &rhine_ops, s,
                          "via-rhine-mmio", RHINE_REGS_SIZE);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->eeprom = eeprom93xx_new(DEVICE(pci_dev), 64);
    rhine_fill_eeprom(s);
    s->nic = qemu_new_nic(&net_rhine_info, &s->conf,
                          object_get_typename(OBJECT(pci_dev)),
                          pci_dev->qdev.id,
                          &pci_dev->qdev.mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void rhine_exit(PCIDevice *pci_dev)
{
    ViaRhineState *s = VIA_RHINE(pci_dev);

    qemu_del_nic(s->nic);
    eeprom93xx_free(DEVICE(pci_dev), s->eeprom);
}

static const VMStateDescription vmstate_via_rhine = {
    .name = "via-rhine",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, ViaRhineState),
        VMSTATE_UINT8_ARRAY(regs, ViaRhineState, RHINE_REGS_SIZE),
        VMSTATE_UINT16_ARRAY(phy, ViaRhineState, 32),
        VMSTATE_UINT8_ARRAY(tx_frame, ViaRhineState, RHINE_MAX_FRAME),
        VMSTATE_UINT32(tx_len, ViaRhineState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property rhine_properties[] = {
    DEFINE_NIC_PROPERTIES(ViaRhineState, conf),
};

static void rhine_instance_init(Object *obj)
{
    ViaRhineState *s = VIA_RHINE(obj);

    device_add_bootindex_property(obj, &s->conf.bootindex, "bootindex",
                                  "/ethernet-phy@0", DEVICE(obj));
}

static void rhine_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = rhine_realize;
    k->exit = rhine_exit;
    k->vendor_id = PCI_VENDOR_ID_VIA;
    k->device_id = 0x3065; /* VT6102 Rhine-II */
    k->revision = 0x74;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    dc->desc = "VIA VT6102 Rhine-II Ethernet";
    dc->vmsd = &vmstate_via_rhine;
    device_class_set_props(dc, rhine_properties);
    device_class_set_legacy_reset(dc, rhine_reset);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo rhine_info = {
    .name = TYPE_VIA_RHINE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ViaRhineState),
    .class_init = rhine_class_init,
    .instance_init = rhine_instance_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void rhine_register_types(void)
{
    type_register_static(&rhine_info);
}

type_init(rhine_register_types)
