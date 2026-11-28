/*
 * Fujitsu MB8795 Ethernet controller of the NeXT computers
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_NET_MB8795_H
#define HW_NET_MB8795_H

#include "hw/core/sysbus.h"
#include "hw/dma/next-dma.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_MB8795 "mb8795"
OBJECT_DECLARE_SIMPLE_TYPE(MB8795State, MB8795)

#define MB8795_BUF_SIZE 4096

struct MB8795State {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    NICState *nic;
    NICConf conf;
    NeXTDMAState *dma;
    qemu_irq tx_irq;
    qemu_irq rx_irq;
    QEMUTimer *timer;

    bool turbo;
    bool twisted_pair;
    bool no_tpe;

    uint8_t tx_status;
    uint8_t tx_mask;
    uint8_t tx_mode;
    uint8_t rx_status;
    uint8_t rx_mask;
    uint8_t rx_mode;
    uint8_t reset;
    uint8_t mac[6];
    uint8_t mem_timing[5];
    uint8_t saved_nibble;
    bool tpe_select;
    bool rx_chain;
    bool in_tick;
    bool sending;           /* inside qemu_send_packet() */
    int64_t rx_wire_free;   /* the wire is free again: virtual ns */
    bool bootp_pending;     /* a NeXT BOOTP request went out */
    uint32_t bootp_xid;
    int32_t recv_state;
    int32_t wire;

    int32_t rx_size;
    int32_t rx_limit;
    int32_t tx_size;
    uint8_t rx_buf[MB8795_BUF_SIZE];
    uint8_t tx_buf[MB8795_BUF_SIZE];
};

/* The BMAP chip selects the twisted pair transceiver */
void mb8795_set_tpe(MB8795State *s, bool tpe);

#endif /* HW_NET_MB8795_H */
