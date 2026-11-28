/*
 * NeXT network services: TCP for the RPC servers, inside the filter
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * NetInfo, the port mapper and NFS also answer on TCP. So that no host
 * socket is needed, this is a small TCP server of its own: it takes the
 * guest's connections to the servers' address, hands their bytes to the
 * RPC servers, and sends back the replies, within the guest's window and
 * again when they are not acknowledged. The guest is on the same
 * virtual wire, so there is no congestion control and data that comes
 * out of order is dropped, to come again.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "nextnet.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "net/checksum.h"
#include "tcp.h"

#define ETH_HLEN        14
#define IP_HLEN         20
#define TCP_HLEN        20

#define TH_FIN          0x01
#define TH_SYN          0x02
#define TH_RST          0x04
#define TH_PSH          0x08
#define TH_ACK          0x10

#define NN_TCP_MSS         1460
#define TCP_MAX_CONNS   32
#define TCP_RTO_MS      500
#define TCP_MAX_RETRIES 8

#define SEQ_LT(a, b)    ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b)   ((int32_t)((a) - (b)) <= 0)

typedef struct NNTcpConn {
    NNTcp *t;
    uint8_t mac[6];
    uint32_t addr;
    uint16_t port;          /* the guest's */
    uint16_t sport;         /* the server's */

    bool established;       /* our SYN was acknowledged */
    bool fin_in;            /* the guest has closed its side */
    bool fin_out;           /* so have we */

    uint32_t rcv_nxt;
    uint32_t iss;
    uint32_t snd_una;       /* the first byte not yet acknowledged */
    uint32_t snd_nxt;
    uint32_t snd_wnd;
    uint16_t mss;
    GByteArray *queue;      /* the replies, from snd_una on */

    struct nn_rpc_stream *rpc;
    QEMUTimer *timer;
    int retries;
    int64_t last_use;

    QTAILQ_ENTRY(NNTcpConn) next;
} NNTcpConn;

struct NNTcp {
    const uint8_t *our_mac;
    uint32_t our_addr;
    NNTcpSend *send;
    void *opaque;
    int nconns;
    QTAILQ_HEAD(, NNTcpConn) conns;
};

static void tcp_send(NNTcpConn *c, uint32_t seq, uint8_t flags,
                     const uint8_t *data, size_t len)
{
    NNTcp *t = c->t;
    size_t opt = (flags & TH_SYN) ? 4 : 0;
    size_t total = ETH_HLEN + IP_HLEN + TCP_HLEN + opt + len;
    g_autofree uint8_t *pkt = g_malloc0(total);
    uint8_t *ip = pkt + ETH_HLEN, *tcp = ip + IP_HLEN;

    memcpy(pkt, c->mac, 6);
    memcpy(pkt + 6, t->our_mac, 6);
    stw_be_p(pkt + 12, 0x0800);
    ip[0] = 0x45;
    stw_be_p(ip + 2, total - ETH_HLEN);
    stw_be_p(ip + 6, 0x4000);           /* don't fragment */
    ip[8] = 64;
    ip[9] = IPPROTO_TCP;
    stl_be_p(ip + 12, t->our_addr);
    stl_be_p(ip + 16, c->addr);
    stw_be_p(tcp, c->sport);
    stw_be_p(tcp + 2, c->port);
    stl_be_p(tcp + 4, seq);
    stl_be_p(tcp + 8, c->rcv_nxt);
    tcp[12] = ((TCP_HLEN + opt) / 4) << 4;
    tcp[13] = flags;
    stw_be_p(tcp + 14, 0xFFFF);         /* the RPC layer takes it all */
    if (opt) {
        tcp[20] = 2;                    /* maximum segment size */
        tcp[21] = 4;
        stw_be_p(tcp + 22, NN_TCP_MSS);
    }
    if (len) {
        memcpy(tcp + TCP_HLEN + opt, data, len);
    }
    net_checksum_calculate(pkt, total, CSUM_IP | CSUM_TCP);
    t->send(t->opaque, pkt, total);
}

/* An answer to a segment with no connection */
static void tcp_send_reset(NNTcp *t, const uint8_t *frame, uint32_t seq,
                           uint32_t ack, uint8_t flags)
{
    const uint8_t *ip = frame + ETH_HLEN;
    const uint8_t *tcp = ip + (ip[0] & 0xF) * 4;
    NNTcpConn c = {
        .t = t,
        .addr = ldl_be_p(ip + 12),
        .port = lduw_be_p(tcp),
        .sport = lduw_be_p(tcp + 2),
        .rcv_nxt = ack,
    };

    memcpy(c.mac, frame + 6, 6);
    tcp_send(&c, seq, TH_RST | flags, NULL, 0);
}

static void tcp_close(NNTcpConn *c)
{
    NNTcp *t = c->t;

    QTAILQ_REMOVE(&t->conns, c, next);
    t->nconns--;
    timer_free(c->timer);
    nn_rpc_stream_free(c->rpc);
    g_byte_array_unref(c->queue);
    g_free(c);
}

static void tcp_abort(NNTcpConn *c)
{
    tcp_send(c, c->snd_nxt, TH_RST | TH_ACK, NULL, 0);
    tcp_close(c);
}

static void tcp_arm(NNTcpConn *c)
{
    if (c->snd_una != c->snd_nxt || (c->queue->len && !c->snd_wnd)) {
        timer_mod(c->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  (TCP_RTO_MS << MIN(c->retries, 6)));
    } else {
        timer_del(c->timer);
    }
}

/*
 * Send what the window takes, then FIN once the guest has closed and
 * all is sent; ack: send an ACK even when there is nothing else
 */
static void tcp_output(NNTcpConn *c, bool ack)
{
    uint32_t wnd = MAX(c->snd_wnd, c->retries ? 1 : 0);

    if (!c->established) {
        if (ack) {
            tcp_send(c, c->iss, TH_SYN | TH_ACK, NULL, 0);
        }
        tcp_arm(c);
        return;
    }
    for (;;) {
        uint32_t sent = c->snd_nxt - c->snd_una;
        uint32_t left = c->queue->len - MIN(sent, c->queue->len);
        uint32_t room = wnd > sent ? wnd - sent : 0;
        uint32_t n = MIN(MIN(left, room), c->mss);

        if (!n) {
            break;
        }
        tcp_send(c, c->snd_nxt, TH_ACK | (n == left ? TH_PSH : 0),
                 c->queue->data + sent, n);
        c->snd_nxt += n;
        ack = false;
    }
    if (c->fin_in && !c->fin_out &&
        c->snd_nxt - c->snd_una == c->queue->len) {
        tcp_send(c, c->snd_nxt, TH_FIN | TH_ACK, NULL, 0);
        c->snd_nxt++;
        c->fin_out = true;
        ack = false;
    }
    if (ack) {
        tcp_send(c, c->snd_nxt, TH_ACK, NULL, 0);
    }
    tcp_arm(c);
}

static void tcp_timeout(void *opaque)
{
    NNTcpConn *c = opaque;

    if (++c->retries > TCP_MAX_RETRIES) {
        nn_log("TCP: %d gave no answer\n", c->port);
        tcp_abort(c);
        return;
    }
    /* go back to the first byte not acknowledged */
    c->snd_nxt = c->established ? c->snd_una : c->iss + 1;
    c->fin_out = false;
    tcp_output(c, true);
}

static NNTcpConn *tcp_find(NNTcp *t, uint32_t addr, uint16_t port,
                           uint16_t sport)
{
    NNTcpConn *c;

    QTAILQ_FOREACH(c, &t->conns, next) {
        if (c->addr == addr && c->port == port && c->sport == sport) {
            return c;
        }
    }
    return NULL;
}

/* The maximum segment size the guest takes, from its SYN */
static uint16_t tcp_peer_mss(const uint8_t *tcp, size_t hlen)
{
    size_t i = TCP_HLEN;

    while (i < hlen) {
        uint8_t kind = tcp[i];

        if (kind == 0) {
            break;
        }
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= hlen || tcp[i + 1] < 2 || i + tcp[i + 1] > hlen) {
            break;
        }
        if (kind == 2 && tcp[i + 1] == 4) {
            uint16_t mss = lduw_be_p(tcp + i + 2);

            return MAX(MIN(mss, NN_TCP_MSS), 64);
        }
        i += tcp[i + 1];
    }
    return 536;
}

static NNTcpConn *tcp_accept(NNTcp *t, const uint8_t *frame,
                             const uint8_t *tcp, size_t hlen)
{
    const uint8_t *ip = frame + ETH_HLEN;
    uint16_t sport = lduw_be_p(tcp + 2);
    struct nn_rpc_stream *rpc = nn_rpc_stream_new(sport);
    NNTcpConn *c;

    if (!rpc) {
        return NULL;
    }
    if (t->nconns >= TCP_MAX_CONNS) {
        /* make room: the one used longest ago goes */
        NNTcpConn *old = QTAILQ_FIRST(&t->conns), *o;

        QTAILQ_FOREACH(o, &t->conns, next) {
            if (o->last_use < old->last_use) {
                old = o;
            }
        }
        tcp_abort(old);
    }
    c = g_new0(NNTcpConn, 1);
    c->t = t;
    memcpy(c->mac, frame + 6, 6);
    c->addr = ldl_be_p(ip + 12);
    c->port = lduw_be_p(tcp);
    c->sport = sport;
    c->rcv_nxt = ldl_be_p(tcp + 4) + 1;
    c->iss = g_random_int();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->snd_wnd = lduw_be_p(tcp + 14);
    c->mss = tcp_peer_mss(tcp, hlen);
    c->queue = g_byte_array_new();
    c->rpc = rpc;
    c->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, tcp_timeout, c);
    QTAILQ_INSERT_TAIL(&t->conns, c, next);
    t->nconns++;
    return c;
}

/* The guest acknowledged up to ack; false when the connection is gone */
static bool tcp_acked(NNTcpConn *c, uint32_t ack)
{
    uint32_t n;

    if (SEQ_LEQ(ack, c->snd_una) || SEQ_LT(c->snd_nxt, ack)) {
        return true;
    }
    n = ack - c->snd_una;
    if (!c->established) {
        c->established = true;      /* our SYN */
        n--;
    }
    if (c->fin_out && ack == c->snd_nxt) {
        n--;                        /* our FIN */
    }
    g_byte_array_remove_range(c->queue, 0, MIN(n, c->queue->len));
    c->snd_una = ack;
    c->retries = 0;
    /* room again: answer the calls that waited */
    if (!c->fin_in && !nn_rpc_stream_input(c->rpc, NULL, 0, c->queue)) {
        tcp_abort(c);
        return false;
    }
    return true;
}

void nn_tcp_input(NNTcp *t, const uint8_t *frame, size_t len)
{
    const uint8_t *ip = frame + ETH_HLEN;
    size_t ihl = (ip[0] & 0xF) * 4;
    size_t ip_len = lduw_be_p(ip + 2);
    const uint8_t *tcp = ip + ihl;
    size_t hlen, dlen;
    uint32_t seq, ack;
    uint8_t flags;
    const uint8_t *data;
    NNTcpConn *c;

    if (ip_len < ihl + TCP_HLEN) {
        return;
    }
    hlen = (tcp[12] >> 4) * 4;
    if (hlen < TCP_HLEN || ip_len < ihl + hlen) {
        return;
    }
    data = tcp + hlen;
    dlen = ip_len - ihl - hlen;
    seq = ldl_be_p(tcp + 4);
    ack = ldl_be_p(tcp + 8);
    flags = tcp[13];

    c = tcp_find(t, ldl_be_p(ip + 12), lduw_be_p(tcp), lduw_be_p(tcp + 2));
    if (flags & TH_RST) {
        if (c) {
            tcp_close(c);
        }
        return;
    }
    if ((flags & (TH_SYN | TH_ACK)) == TH_SYN) {
        if (c && !c->established && seq + 1 == c->rcv_nxt) {
            tcp_output(c, true);    /* our SYN was lost */
            return;
        }
        if (c) {
            tcp_close(c);           /* a new connection on the same ports */
        }
        c = tcp_accept(t, frame, tcp, hlen);
        if (!c) {
            tcp_send_reset(t, frame, 0, seq + 1, TH_ACK);
            return;
        }
        c->last_use = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        tcp_output(c, true);
        return;
    }
    if (!c) {
        if (flags & TH_ACK) {
            tcp_send_reset(t, frame, ack, 0, 0);
        } else {
            tcp_send_reset(t, frame, 0, seq + dlen +
                           ((flags & TH_FIN) ? 1 : 0), TH_ACK);
        }
        return;
    }
    c->last_use = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (!(flags & TH_ACK)) {
        return;
    }
    if (!tcp_acked(c, ack)) {
        return;
    }
    c->snd_wnd = lduw_be_p(tcp + 14);
    if (!c->established) {
        tcp_output(c, false);
        return;
    }

    /* the data, from rcv_nxt on: what came before was had already */
    if (dlen && SEQ_LEQ(seq, c->rcv_nxt) && SEQ_LT(c->rcv_nxt, seq + dlen)) {
        uint32_t skip = c->rcv_nxt - seq;

        if (!c->fin_in &&
            !nn_rpc_stream_input(c->rpc, data + skip, dlen - skip,
                                 c->queue)) {
            tcp_abort(c);
            return;
        }
        c->rcv_nxt += dlen - skip;
    }
    if ((flags & TH_FIN) && !c->fin_in && seq + dlen == c->rcv_nxt) {
        c->rcv_nxt++;
        c->fin_in = true;
    }
    if (c->fin_in && c->fin_out && c->snd_una == c->snd_nxt) {
        tcp_close(c);   /* our FIN was acknowledged */
        return;
    }
    tcp_output(c, dlen || (flags & TH_FIN));
}

NNTcp *nn_tcp_new(const uint8_t *our_mac, uint32_t our_addr,
                  NNTcpSend *send, void *opaque)
{
    NNTcp *t = g_new0(NNTcp, 1);

    t->our_mac = our_mac;
    t->our_addr = our_addr;
    t->send = send;
    t->opaque = opaque;
    QTAILQ_INIT(&t->conns);
    return t;
}

void nn_tcp_free(NNTcp *t)
{
    if (!t) {
        return;
    }
    while (!QTAILQ_EMPTY(&t->conns)) {
        tcp_close(QTAILQ_FIRST(&t->conns));
    }
    g_free(t);
}
