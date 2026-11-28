/*
 * QEMU PowerPC 4xx embedded processors shared devices emulation
 *
 * Copyright (c) 2007 Jocelyn Mayer
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "target/ppc/cpu.h"
#include "hw/ppc/ppc4xx.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "qapi/error.h"

/*****************************************************************************/
/* MAL */

enum {
    MAL0_CFG      = 0x180,
    MAL0_ESR      = 0x181,
    MAL0_IER      = 0x182,
    MAL0_TXCASR   = 0x184,
    MAL0_TXCARR   = 0x185,
    MAL0_TXEOBISR = 0x186,
    MAL0_TXDEIR   = 0x187,
    MAL0_RXCASR   = 0x190,
    MAL0_RXCARR   = 0x191,
    MAL0_RXEOBISR = 0x192,
    MAL0_RXDEIR   = 0x193,
    MAL0_TXCTP0R  = 0x1A0,
    MAL0_RXCTP0R  = 0x1C0,
    MAL0_RCBS0    = 0x1E0,
    MAL0_RCBS1    = 0x1E1,
};

static void ppc4xx_mal_reset(DeviceState *dev)
{
    Ppc4xxMalState *mal = PPC4xx_MAL(dev);

    mal->cfg = 0x0007C000;
    mal->esr = 0x00000000;
    mal->ier = 0x00000000;
    mal->rxcasr = 0x00000000;
    mal->rxdeir = 0x00000000;
    mal->rxeobisr = 0x00000000;
    mal->txcasr = 0x00000000;
    mal->txdeir = 0x00000000;
    mal->txeobisr = 0x00000000;
}

/* descriptors: control and status, length, buffer address, big endian */
#define MAL_DESC_SIZE       8
#define MAL_TX_READY        0x8000
#define MAL_RX_EMPTY        0x8000
#define MAL_WRAP            0x4000
#define MAL_LAST            0x1000
#define MAL_RX_FIRST        0x0800
#define MAL_INTR            0x0400
#define MAL_CFG_EOPIE       0x00000004  /* EOB at the end of every packet */
#define MAL_EMAC_BITS       0x03ff  /* EMAC control in, status out */
#define MAL_IER_DE          0x00000010
#define MAL_MAX_FRAME       16384
#define MAL_MAX_DESCS       256

static uint32_t mal_chan_bit(int chan)
{
    return 0x80000000u >> chan;
}

/*
 * The 460EX's MAL gives each EMAC a group of receive channels (8 for EMAC1);
 * the first channel of a group has the group's bit in RXCASR and the status
 * registers.
 */
static uint32_t mal_rx_bit(Ppc4xxMalState *mal, int chan)
{
    if (mal->rx_group && !(chan % mal->rx_group)) {
        return mal_chan_bit(chan / mal->rx_group);
    }
    return mal_chan_bit(chan);
}

/* irqs: TXEOB, RXEOB, TXDE, RXDE */
static void ppc4xx_mal_update_irq(Ppc4xxMalState *mal)
{
    bool de = mal->ier & MAL_IER_DE;

    qemu_set_irq(mal->irqs[0], !!mal->txeobisr);
    qemu_set_irq(mal->irqs[1], !!mal->rxeobisr);
    qemu_set_irq(mal->irqs[2], de && mal->txdeir);
    qemu_set_irq(mal->irqs[3], de && mal->rxdeir);
}

static void mal_desc_read(uint32_t addr, uint16_t *ctrl, uint16_t *len,
                          uint32_t *buf)
{
    uint8_t d[MAL_DESC_SIZE];

    address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                       d, sizeof(d));
    *ctrl = lduw_be_p(d);
    *len = lduw_be_p(d + 2);
    *buf = ldl_be_p(d + 4);
}

static void mal_desc_write(uint32_t addr, uint16_t ctrl, uint16_t len)
{
    uint8_t d[4];

    stw_be_p(d, ctrl);
    stw_be_p(d + 2, len);
    address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                        d, sizeof(d));
}

static uint32_t mal_next(uint32_t desc, uint16_t ctrl, uint32_t base)
{
    return (ctrl & MAL_WRAP) ? base : desc + MAL_DESC_SIZE;
}

static void mal_rx_notify(Ppc4xxMalState *mal, int chan)
{
    if (chan < mal->rxcnum && mal->rx_notify[chan]) {
        mal->rx_notify[chan](mal->rx_notify_opaque[chan]);
    }
}

void ppc4xx_mal_set_rx_notify(Ppc4xxMalState *mal, int chan,
                              void (*notify)(void *opaque), void *opaque)
{
    assert(chan < mal->rxcnum);
    mal->rx_notify[chan] = notify;
    mal->rx_notify_opaque[chan] = opaque;
}

void ppc4xx_mal_transmit(Ppc4xxMalState *mal, int chan,
                         void (*send)(void *opaque, const uint8_t *buf,
                                      size_t len),
                         void *opaque)
{
    g_autofree uint8_t *frame = NULL;
    size_t len = 0;
    int n;

    if (chan >= mal->txcnum || !(mal->txcasr & mal_chan_bit(chan))) {
        return;
    }
    frame = g_malloc(MAL_MAX_FRAME);
    for (n = 0; n < MAL_MAX_DESCS; n++) {
        uint32_t desc = mal->txcur[chan], buf;
        uint16_t ctrl, dlen;

        mal_desc_read(desc, &ctrl, &dlen, &buf);
        if (!(ctrl & MAL_TX_READY)) {
            break;
        }
        dlen = MIN(dlen, MAL_MAX_FRAME - len);
        address_space_read(&address_space_memory, buf,
                           MEMTXATTRS_UNSPECIFIED, frame + len, dlen);
        len += dlen;
        if (ctrl & MAL_LAST) {
            send(opaque, frame, len);
            len = 0;
        }
        /* sent, and no errors in the EMAC status bits */
        mal_desc_write(desc, ctrl & ~(MAL_TX_READY | MAL_EMAC_BITS), dlen);
        if ((ctrl & MAL_INTR) ||
            ((mal->cfg & MAL_CFG_EOPIE) && (ctrl & MAL_LAST))) {
            mal->txeobisr |= mal_chan_bit(chan);
        }
        mal->txcur[chan] = mal_next(desc, ctrl, mal->txctpr[chan]);
    }
    ppc4xx_mal_update_irq(mal);
}

bool ppc4xx_mal_can_receive(Ppc4xxMalState *mal, int chan)
{
    uint32_t buf;
    uint16_t ctrl, len;

    if (chan >= mal->rxcnum || !(mal->rxcasr & mal_rx_bit(mal, chan)) ||
        !mal->rcbs[chan]) {
        return false;
    }
    mal_desc_read(mal->rxcur[chan], &ctrl, &len, &buf);
    return ctrl & MAL_RX_EMPTY;
}

bool ppc4xx_mal_receive(Ppc4xxMalState *mal, int chan, const uint8_t *data,
                        size_t size)
{
    uint32_t bufsize = mal->rcbs[chan] * 16;
    size_t done = 0;
    int n;

    if (!ppc4xx_mal_can_receive(mal, chan)) {
        return false;
    }
    for (n = 0; n < MAL_MAX_DESCS && done < size; n++) {
        uint32_t desc = mal->rxcur[chan], buf;
        uint16_t ctrl, len, chunk;

        mal_desc_read(desc, &ctrl, &len, &buf);
        if (!(ctrl & MAL_RX_EMPTY)) {
            /* out of descriptors in the middle of a frame */
            mal->rxdeir |= mal_rx_bit(mal, chan);
            break;
        }
        chunk = MIN(bufsize, size - done);
        address_space_write(&address_space_memory, buf,
                            MEMTXATTRS_UNSPECIFIED, data + done, chunk);
        ctrl &= ~(MAL_RX_EMPTY | MAL_RX_FIRST | MAL_LAST | MAL_EMAC_BITS);
        if (!done) {
            ctrl |= MAL_RX_FIRST;
        }
        done += chunk;
        if (done >= size) {
            ctrl |= MAL_LAST;
        }
        mal_desc_write(desc, ctrl, chunk);
        if ((ctrl & MAL_INTR) ||
            ((mal->cfg & MAL_CFG_EOPIE) && (ctrl & MAL_LAST))) {
            mal->rxeobisr |= mal_rx_bit(mal, chan);
        }
        mal->rxcur[chan] = mal_next(desc, ctrl, mal->rxctpr[chan]);
    }
    ppc4xx_mal_update_irq(mal);
    return true;
}

static uint32_t dcr_read_mal(void *opaque, int dcrn)
{
    Ppc4xxMalState *mal = opaque;
    uint32_t ret;

    switch (dcrn) {
    case MAL0_CFG:
        ret = mal->cfg;
        break;
    case MAL0_ESR:
        ret = mal->esr;
        break;
    case MAL0_IER:
        ret = mal->ier;
        break;
    case MAL0_TXCASR:
        ret = mal->txcasr;
        break;
    case MAL0_TXCARR:
        ret = mal->txcarr;
        break;
    case MAL0_TXEOBISR:
        ret = mal->txeobisr;
        break;
    case MAL0_TXDEIR:
        ret = mal->txdeir;
        break;
    case MAL0_RXCASR:
        ret = mal->rxcasr;
        break;
    case MAL0_RXCARR:
        ret = mal->rxcarr;
        break;
    case MAL0_RXEOBISR:
        ret = mal->rxeobisr;
        break;
    case MAL0_RXDEIR:
        ret = mal->rxdeir;
        break;
    default:
        ret = 0;
        break;
    }
    if (dcrn >= MAL0_TXCTP0R && dcrn < MAL0_TXCTP0R + mal->txcnum) {
        ret = mal->txctpr[dcrn - MAL0_TXCTP0R];
    }
    if (dcrn >= MAL0_RXCTP0R && dcrn < MAL0_RXCTP0R + mal->rxcnum) {
        ret = mal->rxctpr[dcrn - MAL0_RXCTP0R];
    }
    if (dcrn >= MAL0_RCBS0 && dcrn < MAL0_RCBS0 + mal->rxcnum) {
        ret = mal->rcbs[dcrn - MAL0_RCBS0];
    }

    return ret;
}

static void dcr_write_mal(void *opaque, int dcrn, uint32_t val)
{
    Ppc4xxMalState *mal = opaque;
    int i;

    switch (dcrn) {
    case MAL0_CFG:
        if (val & 0x80000000) {
            ppc4xx_mal_reset(DEVICE(mal));
        }
        mal->cfg = val & 0x00FFC087;
        break;
    case MAL0_ESR:
        /* Read/clear */
        mal->esr &= ~val;
        break;
    case MAL0_IER:
        mal->ier = val & 0x0000001F;
        break;
    case MAL0_TXCASR:
        for (i = 0; i < mal->txcnum; i++) {
            if (val & ~mal->txcasr & mal_chan_bit(i)) {
                mal->txcur[i] = mal->txctpr[i];
            }
        }
        mal->txcasr |= val & 0xF0000000;
        break;
    case MAL0_TXCARR:
        /* reset the channels: they stop until set active again */
        mal->txcasr &= ~(val & 0xF0000000);
        break;
    case MAL0_TXEOBISR:
        /* Read/clear */
        mal->txeobisr &= ~val;
        break;
    case MAL0_TXDEIR:
        /* Read/clear */
        mal->txdeir &= ~val;
        break;
    case MAL0_RXCASR:
        for (i = 0; i < mal->rxcnum; i++) {
            if (val & ~mal->rxcasr & mal_rx_bit(mal, i)) {
                mal->rxcur[i] = mal->rxctpr[i];
            }
        }
        mal->rxcasr |= val & 0xC0000000;
        for (i = 0; i < mal->rxcnum; i++) {
            if (val & mal_rx_bit(mal, i)) {
                mal_rx_notify(mal, i);
            }
        }
        break;
    case MAL0_RXCARR:
        mal->rxcasr &= ~(val & 0xC0000000);
        break;
    case MAL0_RXEOBISR:
        /* Read/clear */
        mal->rxeobisr &= ~val;
        break;
    case MAL0_RXDEIR:
        /* Read/clear */
        mal->rxdeir &= ~val;
        for (i = 0; i < mal->rxcnum; i++) {
            if (val & mal_rx_bit(mal, i)) {
                mal_rx_notify(mal, i);
            }
        }
        break;
    }
    if (dcrn >= MAL0_TXCTP0R && dcrn < MAL0_TXCTP0R + mal->txcnum) {
        mal->txctpr[dcrn - MAL0_TXCTP0R] = val;
        mal->txcur[dcrn - MAL0_TXCTP0R] = val;
    }
    if (dcrn >= MAL0_RXCTP0R && dcrn < MAL0_RXCTP0R + mal->rxcnum) {
        mal->rxctpr[dcrn - MAL0_RXCTP0R] = val;
        mal->rxcur[dcrn - MAL0_RXCTP0R] = val;
    }
    if (dcrn >= MAL0_RCBS0 && dcrn < MAL0_RCBS0 + mal->rxcnum) {
        mal->rcbs[dcrn - MAL0_RCBS0] = val & 0x000000FF;
        mal_rx_notify(mal, dcrn - MAL0_RCBS0);
    }
    ppc4xx_mal_update_irq(mal);
}

static void ppc4xx_mal_realize(DeviceState *dev, Error **errp)
{
    Ppc4xxMalState *mal = PPC4xx_MAL(dev);
    Ppc4xxDcrDeviceState *dcr = PPC4xx_DCR_DEVICE(dev);
    int i;

    if (mal->txcnum > 32 || mal->rxcnum > 32) {
        error_setg(errp, "invalid TXC/RXC number");
        return;
    }

    mal->txctpr = g_new0(uint32_t, mal->txcnum);
    mal->rxctpr = g_new0(uint32_t, mal->rxcnum);
    mal->rcbs = g_new0(uint32_t, mal->rxcnum);
    mal->txcur = g_new0(uint32_t, mal->txcnum);
    mal->rxcur = g_new0(uint32_t, mal->rxcnum);
    mal->rx_notify = g_new0(typeof(*mal->rx_notify), mal->rxcnum);
    mal->rx_notify_opaque = g_new0(void *, mal->rxcnum);

    for (i = 0; i < ARRAY_SIZE(mal->irqs); i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &mal->irqs[i]);
    }

    ppc4xx_dcr_register(dcr, MAL0_CFG, mal, &dcr_read_mal, &dcr_write_mal);
    ppc4xx_dcr_register(dcr, MAL0_ESR, mal, &dcr_read_mal, &dcr_write_mal);
    ppc4xx_dcr_register(dcr, MAL0_IER, mal, &dcr_read_mal, &dcr_write_mal);
    ppc4xx_dcr_register(dcr, MAL0_TXCASR, mal, &dcr_read_mal, &dcr_write_mal);
    ppc4xx_dcr_register(dcr, MAL0_TXCARR, mal, &dcr_read_mal, &dcr_write_mal);
    ppc4xx_dcr_register(dcr, MAL0_TXEOBISR, mal, &dcr_read_mal, &dcr_write_mal);
    ppc4xx_dcr_register(dcr, MAL0_TXDEIR, mal, &dcr_read_mal, &dcr_write_mal);
    ppc4xx_dcr_register(dcr, MAL0_RXCASR, mal, &dcr_read_mal, &dcr_write_mal);
    ppc4xx_dcr_register(dcr, MAL0_RXCARR, mal, &dcr_read_mal, &dcr_write_mal);
    ppc4xx_dcr_register(dcr, MAL0_RXEOBISR, mal, &dcr_read_mal, &dcr_write_mal);
    ppc4xx_dcr_register(dcr, MAL0_RXDEIR, mal, &dcr_read_mal, &dcr_write_mal);
    for (i = 0; i < mal->txcnum; i++) {
        ppc4xx_dcr_register(dcr, MAL0_TXCTP0R + i,
                            mal, &dcr_read_mal, &dcr_write_mal);
    }
    for (i = 0; i < mal->rxcnum; i++) {
        ppc4xx_dcr_register(dcr, MAL0_RXCTP0R + i,
                            mal, &dcr_read_mal, &dcr_write_mal);
    }
    for (i = 0; i < mal->rxcnum; i++) {
        ppc4xx_dcr_register(dcr, MAL0_RCBS0 + i,
                            mal, &dcr_read_mal, &dcr_write_mal);
    }
}

static void ppc4xx_mal_finalize(Object *obj)
{
    Ppc4xxMalState *mal = PPC4xx_MAL(obj);

    g_free(mal->rcbs);
    g_free(mal->txcur);
    g_free(mal->rxcur);
    g_free(mal->rx_notify);
    g_free(mal->rx_notify_opaque);
    g_free(mal->rxctpr);
    g_free(mal->txctpr);
}

static const Property ppc4xx_mal_properties[] = {
    DEFINE_PROP_UINT8("txc-num", Ppc4xxMalState, txcnum, 0),
    DEFINE_PROP_UINT8("rxc-num", Ppc4xxMalState, rxcnum, 0),
    DEFINE_PROP_UINT8("rx-group", Ppc4xxMalState, rx_group, 0),
};

static void ppc4xx_mal_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ppc4xx_mal_realize;
    device_class_set_legacy_reset(dc, ppc4xx_mal_reset);
    /* Reason: only works as function of a ppc4xx SoC */
    dc->user_creatable = false;
    device_class_set_props(dc, ppc4xx_mal_properties);
}

/*****************************************************************************/
/* Peripheral local bus arbitrer */
enum {
    PLB3A0_ACR = 0x077,
    PLB4A0_ACR = 0x081,
    PLB0_BESR  = 0x084,
    PLB0_BEAR  = 0x086,
    PLB0_ACR   = 0x087,
    PLB4A1_ACR = 0x089,
};

static uint32_t dcr_read_plb(void *opaque, int dcrn)
{
    Ppc4xxPlbState *plb = opaque;
    uint32_t ret;

    switch (dcrn) {
    case PLB0_ACR:
        ret = plb->acr;
        break;
    case PLB0_BEAR:
        ret = plb->bear;
        break;
    case PLB0_BESR:
        ret = plb->besr;
        break;
    default:
        /* Avoid gcc warning */
        ret = 0;
        break;
    }

    return ret;
}

static void dcr_write_plb(void *opaque, int dcrn, uint32_t val)
{
    Ppc4xxPlbState *plb = opaque;

    switch (dcrn) {
    case PLB0_ACR:
        /*
         * We don't care about the actual parameters written as
         * we don't manage any priorities on the bus
         */
        plb->acr = val & 0xF8000000;
        break;
    case PLB0_BEAR:
        /* Read only */
        break;
    case PLB0_BESR:
        /* Write-clear */
        plb->besr &= ~val;
        break;
    }
}

static void ppc405_plb_reset(DeviceState *dev)
{
    Ppc4xxPlbState *plb = PPC4xx_PLB(dev);

    plb->acr = 0x00000000;
    plb->bear = 0x00000000;
    plb->besr = 0x00000000;
}

static void ppc405_plb_realize(DeviceState *dev, Error **errp)
{
    Ppc4xxPlbState *plb = PPC4xx_PLB(dev);
    Ppc4xxDcrDeviceState *dcr = PPC4xx_DCR_DEVICE(dev);

    ppc4xx_dcr_register(dcr, PLB3A0_ACR, plb, &dcr_read_plb, &dcr_write_plb);
    ppc4xx_dcr_register(dcr, PLB4A0_ACR, plb, &dcr_read_plb, &dcr_write_plb);
    ppc4xx_dcr_register(dcr, PLB0_ACR, plb, &dcr_read_plb, &dcr_write_plb);
    ppc4xx_dcr_register(dcr, PLB0_BEAR, plb, &dcr_read_plb, &dcr_write_plb);
    ppc4xx_dcr_register(dcr, PLB0_BESR, plb, &dcr_read_plb, &dcr_write_plb);
    ppc4xx_dcr_register(dcr, PLB4A1_ACR, plb, &dcr_read_plb, &dcr_write_plb);
}

static void ppc405_plb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ppc405_plb_realize;
    device_class_set_legacy_reset(dc, ppc405_plb_reset);
    /* Reason: only works as function of a ppc4xx SoC */
    dc->user_creatable = false;
}

/*****************************************************************************/
/* Peripheral controller */
enum {
    EBC0_CFGADDR = 0x012,
    EBC0_CFGDATA = 0x013,
};

static uint32_t dcr_read_ebc(void *opaque, int dcrn)
{
    Ppc4xxEbcState *ebc = opaque;
    uint32_t ret;

    switch (dcrn) {
    case EBC0_CFGADDR:
        ret = ebc->addr;
        break;
    case EBC0_CFGDATA:
        switch (ebc->addr) {
        case 0x00: /* B0CR */
            ret = ebc->bcr[0];
            break;
        case 0x01: /* B1CR */
            ret = ebc->bcr[1];
            break;
        case 0x02: /* B2CR */
            ret = ebc->bcr[2];
            break;
        case 0x03: /* B3CR */
            ret = ebc->bcr[3];
            break;
        case 0x04: /* B4CR */
            ret = ebc->bcr[4];
            break;
        case 0x05: /* B5CR */
            ret = ebc->bcr[5];
            break;
        case 0x06: /* B6CR */
            ret = ebc->bcr[6];
            break;
        case 0x07: /* B7CR */
            ret = ebc->bcr[7];
            break;
        case 0x10: /* B0AP */
            ret = ebc->bap[0];
            break;
        case 0x11: /* B1AP */
            ret = ebc->bap[1];
            break;
        case 0x12: /* B2AP */
            ret = ebc->bap[2];
            break;
        case 0x13: /* B3AP */
            ret = ebc->bap[3];
            break;
        case 0x14: /* B4AP */
            ret = ebc->bap[4];
            break;
        case 0x15: /* B5AP */
            ret = ebc->bap[5];
            break;
        case 0x16: /* B6AP */
            ret = ebc->bap[6];
            break;
        case 0x17: /* B7AP */
            ret = ebc->bap[7];
            break;
        case 0x20: /* BEAR */
            ret = ebc->bear;
            break;
        case 0x21: /* BESR0 */
            ret = ebc->besr0;
            break;
        case 0x22: /* BESR1 */
            ret = ebc->besr1;
            break;
        case 0x23: /* CFG */
            ret = ebc->cfg;
            break;
        default:
            ret = 0x00000000;
            break;
        }
        break;
    default:
        ret = 0x00000000;
        break;
    }

    return ret;
}

static void dcr_write_ebc(void *opaque, int dcrn, uint32_t val)
{
    Ppc4xxEbcState *ebc = opaque;

    switch (dcrn) {
    case EBC0_CFGADDR:
        ebc->addr = val;
        break;
    case EBC0_CFGDATA:
        switch (ebc->addr) {
        case 0x00: /* B0CR */
            break;
        case 0x01: /* B1CR */
            break;
        case 0x02: /* B2CR */
            break;
        case 0x03: /* B3CR */
            break;
        case 0x04: /* B4CR */
            break;
        case 0x05: /* B5CR */
            break;
        case 0x06: /* B6CR */
            break;
        case 0x07: /* B7CR */
            break;
        case 0x10: /* B0AP */
            break;
        case 0x11: /* B1AP */
            break;
        case 0x12: /* B2AP */
            break;
        case 0x13: /* B3AP */
            break;
        case 0x14: /* B4AP */
            break;
        case 0x15: /* B5AP */
            break;
        case 0x16: /* B6AP */
            break;
        case 0x17: /* B7AP */
            break;
        case 0x20: /* BEAR */
            break;
        case 0x21: /* BESR0 */
            break;
        case 0x22: /* BESR1 */
            break;
        case 0x23: /* CFG */
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

static void ppc405_ebc_reset(DeviceState *dev)
{
    Ppc4xxEbcState *ebc = PPC4xx_EBC(dev);
    int i;

    ebc->addr = 0x00000000;
    ebc->bap[0] = 0x7F8FFE80;
    ebc->bcr[0] = 0xFFE28000;
    for (i = 0; i < 8; i++) {
        ebc->bap[i] = 0x00000000;
        ebc->bcr[i] = 0x00000000;
    }
    ebc->besr0 = 0x00000000;
    ebc->besr1 = 0x00000000;
    ebc->cfg = 0x80400000;
}

static void ppc405_ebc_realize(DeviceState *dev, Error **errp)
{
    Ppc4xxEbcState *ebc = PPC4xx_EBC(dev);
    Ppc4xxDcrDeviceState *dcr = PPC4xx_DCR_DEVICE(dev);

    ppc4xx_dcr_register(dcr, EBC0_CFGADDR, ebc, &dcr_read_ebc, &dcr_write_ebc);
    ppc4xx_dcr_register(dcr, EBC0_CFGDATA, ebc, &dcr_read_ebc, &dcr_write_ebc);
}

static void ppc405_ebc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ppc405_ebc_realize;
    device_class_set_legacy_reset(dc, ppc405_ebc_reset);
    /* Reason: only works as function of a ppc4xx SoC */
    dc->user_creatable = false;
}

/* PPC4xx_DCR_DEVICE */

void ppc4xx_dcr_register(Ppc4xxDcrDeviceState *dev, int dcrn, void *opaque,
                         dcr_read_cb dcr_read, dcr_write_cb dcr_write)
{
    assert(dev->cpu);
    ppc_dcr_register(&dev->cpu->env, dcrn, opaque, dcr_read, dcr_write);
}

bool ppc4xx_dcr_realize(Ppc4xxDcrDeviceState *dev, PowerPCCPU *cpu,
                        Error **errp)
{
    object_property_set_link(OBJECT(dev), "cpu", OBJECT(cpu), &error_abort);
    return sysbus_realize(SYS_BUS_DEVICE(dev), errp);
}

static const Property ppc4xx_dcr_properties[] = {
    DEFINE_PROP_LINK("cpu", Ppc4xxDcrDeviceState, cpu, TYPE_POWERPC_CPU,
                     PowerPCCPU *),
};

static void ppc4xx_dcr_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, ppc4xx_dcr_properties);
}

static const TypeInfo ppc4xx_types[] = {
    {
        .name           = TYPE_PPC4xx_MAL,
        .parent         = TYPE_PPC4xx_DCR_DEVICE,
        .instance_size  = sizeof(Ppc4xxMalState),
        .instance_finalize = ppc4xx_mal_finalize,
        .class_init     = ppc4xx_mal_class_init,
    }, {
        .name           = TYPE_PPC4xx_PLB,
        .parent         = TYPE_PPC4xx_DCR_DEVICE,
        .instance_size  = sizeof(Ppc4xxPlbState),
        .class_init     = ppc405_plb_class_init,
    }, {
        .name           = TYPE_PPC4xx_EBC,
        .parent         = TYPE_PPC4xx_DCR_DEVICE,
        .instance_size  = sizeof(Ppc4xxEbcState),
        .class_init     = ppc405_ebc_class_init,
    }, {
        .name           = TYPE_PPC4xx_DCR_DEVICE,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Ppc4xxDcrDeviceState),
        .class_init     = ppc4xx_dcr_class_init,
        .abstract       = true,
    }
};

DEFINE_TYPES(ppc4xx_types)
