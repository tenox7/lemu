/*
 * Marvell Discovery II (MV64360/MV64361) Gigabit Ethernet controller
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef HW_NET_MV643XX_ETH_H
#define HW_NET_MV643XX_ETH_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_MV643XX_ETH "mv643xx-eth"
OBJECT_DECLARE_SIMPLE_TYPE(MV643xxEthState, MV643XX_ETH)

/* The MV64361 has two ports; registers of a third read as zero */
#define MV643XX_ETH_PORTS       2
#define MV643XX_ETH_REGS_SIZE   0x2000

/*
 * Interrupt lines to the main interrupt controller: the unit error line,
 * then for each port its summary, Rx, Tx and misc lines.
 */
enum {
    MV643XX_ETH_IRQ_UNIT,
    MV643XX_ETH_IRQ_PORT0,
};
#define MV643XX_ETH_IRQ_SUM     0
#define MV643XX_ETH_IRQ_RX      1
#define MV643XX_ETH_IRQ_TX      2
#define MV643XX_ETH_IRQ_MISC    3
#define MV643XX_ETH_IRQ(port, n) (MV643XX_ETH_IRQ_PORT0 + (port) * 4 + (n))
#define MV643XX_ETH_NUM_IRQS    MV643XX_ETH_IRQ(MV643XX_ETH_PORTS, 0)

struct MV643xxEthState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    NICState *nic;
    NICConf conf;
    qemu_irq irq[MV643XX_ETH_NUM_IRQS];
    bool irq_level[MV643XX_ETH_NUM_IRQS];
    QEMUTimer *irq_timer;
    bool irq_wait_access;
    uint32_t cause_reads;
    uint8_t nic_port;

    uint32_t regs[MV643XX_ETH_REGS_SIZE / 4];
    uint32_t mib[MV643XX_ETH_PORTS][32];
    uint16_t phy[32];
};

#endif
