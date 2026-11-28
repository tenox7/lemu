/*
 * NeXT network services on QEMU's user network
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * NeXTSTEP set to configure itself (-AUTOMATIC-) wants more than an
 * address: it asks a bootparam server for its name and binds to a parent
 * NetInfo server. The Previous emulator serves these from its own slirp;
 * this filter does the same for QEMU's user network, with the servers
 * ported from Previous:
 *
 *   10.0.2.254  "nfs": portmap, bootparam, NetInfo bind and NetInfo for
 *               the "network" domain, and with a shared folder also mount
 *               and NFS, which NeXTSTEP mounts on /Net
 *   10.0.2.3    names of these machines, in the domain "home"
 *   10.0.2.2    the time, by NTP
 *
 * All of it is answered here, in the filter: the guest's packets for
 * 10.0.2.254 never reach the user network, and there are no host
 * sockets, so nothing but the guest reaches the servers. The filter
 * answers ARP and ping for 10.0.2.254, puts the guest's fragmented
 * packets for it together, hands UDP calls to the servers and TCP to a
 * small TCP server of its own (tcp.c), and fragments long answers.
 * Broadcasts to the port mapper go to it too.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "nextnet.h"
#include "qapi/error.h"
#include "qemu/iov.h"
#include "qom/object.h"
#include "net/filter.h"
#include "net/net.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "qemu/timer.h"
#include "rpc.h"
#include "tcp.h"
#include "net/filter-next.h"

OBJECT_DECLARE_SIMPLE_TYPE(FilterNeXTState, FILTER_NEXT)

#define ETH_HLEN        14
#define ETHERTYPE_IP    0x0800
#define ETHERTYPE_ARP   0x0806

#define IPPROTO_ICMP_   1

#define PORT_DNS        53
#define PORT_NTP        123

#define ADDR_HOST       (CTL_NET | CTL_GATEWAY)
#define ADDR_DNS        (CTL_NET | CTL_DNS)
#define ADDR_SERVER     (CTL_NET | CTL_NFSD)

#define NN_IP_MTU          1500
/* fragmented packets for the servers being put together */
#define REASM_SLOTS     4
#define REASM_MAX       65535
#define REASM_TIMEOUT_MS 30000

typedef struct FilterNeXTReasm {
    uint8_t *data;              /* the IP payload, REASM_MAX bytes */
    uint8_t blocks[REASM_MAX / 8 / 8 + 1];    /* which 8 bytes are here */
    uint8_t head[ETH_HLEN + 60];  /* of the first fragment */
    size_t head_len;
    uint32_t src;
    uint16_t id;
    uint8_t prot;
    bool used;
    size_t total;               /* the payload, once the last one is here */
    int64_t time;
} FilterNeXTReasm;

struct FilterNeXTState {
    NetFilterState parent_obj;

    char *share;
    char *guest_name;
    char *system_type;
    bool network_time;
    bool debug;

    /* the user network's own Ethernet address */
    uint8_t net_mac[6];
    uint16_t ip_id;
    NNTcp *tcp;
    FilterNeXTReasm reasm[REASM_SLOTS];
};

static bool filter_next_in_use;

/* ---- packets ---- */

static void ip_header_csum(uint8_t *ip)
{
    size_t ihl = (ip[0] & 0xF) * 4;

    stw_be_p(ip + 10, 0);
    stw_be_p(ip + 10, net_raw_checksum(ip, ihl));
}

static void filter_next_send(FilterNeXTState *s, bool to_guest,
                             uint8_t *pkt, size_t len)
{
    NetFilterState *nf = NETFILTER(s);
    struct iovec iov = { .iov_base = pkt, .iov_len = len };
    NetClientState *sender = to_guest ? nf->netdev : nf->netdev->peer;

    if (sender) {
        qemu_netfilter_pass_to_next(sender, 0, &iov, 1, nf);
    }
}

/* Send a whole IP packet to the guest, in fragments if it is long */
static void filter_next_send_ip(FilterNeXTState *s, uint8_t *pkt, size_t len)
{
    uint8_t *ip = pkt + ETH_HLEN;
    size_t payload = len - ETH_HLEN - 20;
    uint8_t frag[ETH_HLEN + NN_IP_MTU];

    stw_be_p(ip + 4, s->ip_id++);
    if (len - ETH_HLEN <= NN_IP_MTU) {
        ip_header_csum(ip);
        filter_next_send(s, true, pkt, len);
        return;
    }
    for (size_t off = 0; off < payload; off += NN_IP_MTU - 20) {
        size_t n = MIN(payload - off, NN_IP_MTU - 20);
        uint8_t *fip = frag + ETH_HLEN;

        memcpy(frag, pkt, ETH_HLEN + 20);
        memcpy(fip + 20, ip + 20 + off, n);
        stw_be_p(fip + 2, 20 + n);
        stw_be_p(fip + 6, (off / 8) | (off + n < payload ? 0x2000 : 0));
        ip_header_csum(fip);
        filter_next_send(s, true, frag, ETH_HLEN + 20 + n);
    }
}

/* Send a UDP packet of ours to the guest */
static void filter_next_udp_reply(FilterNeXTState *s, const uint8_t *req,
                                  uint32_t src, uint16_t sport,
                                  const uint8_t *data, size_t len)
{
    size_t total = ETH_HLEN + 20 + 8 + len;
    g_autofree uint8_t *pkt = NULL;
    const uint8_t *rip = req + ETH_HLEN;
    const uint8_t *rudp = rip + (rip[0] & 0xF) * 4;
    uint8_t *ip, *udp;

    if (20 + 8 + len > 0xFFFF) {
        nn_log("the answer is too long for UDP\n");
        return;
    }
    pkt = g_malloc0(total);
    ip = pkt + ETH_HLEN;
    udp = ip + 20;
    memcpy(pkt, req + 6, 6);
    memcpy(pkt + 6, s->net_mac, 6);
    stw_be_p(pkt + 12, ETHERTYPE_IP);
    ip[0] = 0x45;
    stw_be_p(ip + 2, 20 + 8 + len);
    ip[8] = 64;
    ip[9] = IPPROTO_UDP;
    stl_be_p(ip + 12, src);
    memcpy(ip + 16, rip + 12, 4);
    stw_be_p(udp, sport);
    memcpy(udp + 2, rudp, 2);
    stw_be_p(udp + 4, 8 + len);
    memcpy(udp + 8, data, len);
    net_checksum_calculate(pkt, total, CSUM_UDP);
    filter_next_send_ip(s, pkt, total);
}

/* ---- ARP and ping for the servers ---- */

static bool filter_next_arp(FilterNeXTState *s, const uint8_t *pkt,
                            size_t len)
{
    const uint8_t *arp = pkt + ETH_HLEN;
    uint8_t reply[ETH_HLEN + 28];

    if (len < ETH_HLEN + 28 || lduw_be_p(arp + 6) != 1 ||
        ldl_be_p(arp + 24) != ADDR_SERVER) {
        return false;
    }
    memcpy(reply, pkt + 6, 6);
    memcpy(reply + 6, s->net_mac, 6);
    stw_be_p(reply + 12, ETHERTYPE_ARP);
    memcpy(reply + ETH_HLEN, arp, 6);           /* hardware and protocol */
    stw_be_p(reply + ETH_HLEN + 6, 2);          /* a reply */
    memcpy(reply + ETH_HLEN + 8, s->net_mac, 6);
    stl_be_p(reply + ETH_HLEN + 14, ADDR_SERVER);
    memcpy(reply + ETH_HLEN + 18, arp + 8, 10); /* the asker */
    filter_next_send(s, true, reply, sizeof(reply));
    return true;
}

static void filter_next_ping(FilterNeXTState *s, const uint8_t *pkt,
                             size_t len)
{
    g_autofree uint8_t *reply = g_memdup2(pkt, len);
    uint8_t *ip = reply + ETH_HLEN;
    size_t ihl = (ip[0] & 0xF) * 4;
    size_t ip_len = lduw_be_p(ip + 2);
    uint8_t *icmp = ip + ihl;

    if (ip_len > len - ETH_HLEN || ip_len < ihl + 8 || icmp[0] != 8) {
        return;
    }
    memcpy(reply, pkt + 6, 6);
    memcpy(reply + 6, s->net_mac, 6);
    memcpy(ip + 16, pkt + ETH_HLEN + 12, 4);
    stl_be_p(ip + 12, ADDR_SERVER);
    ip[8] = 64;
    ip_header_csum(ip);
    icmp[0] = 0;    /* echo reply */
    stw_be_p(icmp + 2, 0);
    stw_be_p(icmp + 2, net_raw_checksum(icmp, ip_len - ihl));
    filter_next_send(s, true, reply, ETH_HLEN + ip_len);
}

/* ---- DNS: the names of the machines here ---- */

static const uint32_t dns_hosts[] = {
    ADDR_HOST, ADDR_DNS, CTL_NET | CTL_HOST, ADDR_SERVER,
};

static const char *dns_host_name(uint32_t addr)
{
    switch (addr) {
    case ADDR_HOST:
        return nn_cfg.host_name;
    case ADDR_DNS:
        return "dns";
    case CTL_NET | CTL_HOST:
        return nn_cfg.guest_name;
    case ADDR_SERVER:
        return nn_cfg.server_name;
    }
    return NULL;
}

/* Read a name of the question into dotted form; returns its length */
static size_t dns_read_name(const uint8_t *q, size_t len, char *out,
                            size_t outlen)
{
    size_t i = 0, o = 0;

    while (i < len && q[i]) {
        size_t n = q[i];

        if (n > 63 || i + 1 + n > len || o + n + 2 > outlen) {
            return 0;
        }
        if (o) {
            out[o++] = '.';
        }
        for (size_t k = 0; k < n; k++) {
            out[o++] = g_ascii_tolower(q[i + 1 + k]);
        }
        i += 1 + n;
    }
    if (i >= len) {
        return 0;
    }
    out[o] = '\0';
    return i + 1;
}

/*
 * Is the name in our domain? Then give its address, or 0 when there is
 * no such machine.
 */
static bool dns_lookup_a(const char *name, uint32_t *addr)
{
    size_t dlen = strlen(nn_cfg.domain);
    size_t nlen = strlen(name);

    if (nlen <= dlen + 1 || name[nlen - dlen - 1] != '.' ||
        g_ascii_strcasecmp(name + nlen - dlen, nn_cfg.domain)) {
        return false;
    }
    *addr = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(dns_hosts); i++) {
        const char *h = dns_host_name(dns_hosts[i]);

        if (strlen(h) == nlen - dlen - 1 &&
            !g_ascii_strncasecmp(name, h, nlen - dlen - 1)) {
            *addr = dns_hosts[i];
        }
    }
    return true;
}

static bool dns_lookup_ptr(const char *name, char *out, size_t outlen)
{
    unsigned a, b, c, d;
    char tail[32];
    uint32_t addr;
    const char *h;

    if (sscanf(name, "%u.%u.%u.%u.%31s", &d, &c, &b, &a, tail) != 5 ||
        g_ascii_strcasecmp(tail, "in-addr.arpa")) {
        return false;
    }
    addr = (a << 24) | (b << 16) | (c << 8) | d;
    if ((addr & CTL_NET_MASK) != CTL_NET) {
        return false;
    }
    h = dns_host_name(addr);
    if (!h) {
        out[0] = '\0';
    } else {
        snprintf(out, outlen, "%s.%s", h, nn_cfg.domain);
    }
    return true;
}

static size_t dns_write_name(uint8_t *p, const char *name)
{
    size_t o = 0;

    while (*name) {
        const char *dot = strchr(name, '.');
        size_t n = dot ? dot - name : strlen(name);

        p[o++] = n;
        memcpy(p + o, name, n);
        o += n;
        name += n + (dot ? 1 : 0);
    }
    p[o++] = 0;
    return o;
}

static bool filter_next_dns(FilterNeXTState *s, const uint8_t *pkt,
                            const uint8_t *udp, size_t udp_len)
{
    const uint8_t *msg = udp + 8;
    size_t len = udp_len - 8, qlen;
    uint8_t ans[512];
    char name[256], ptr[256];
    uint16_t qtype;
    uint32_t addr = 0;
    bool found;
    size_t n;

    if (len < 12 || len > 300 || (msg[2] & 0x80) ||
        lduw_be_p(msg + 4) != 1) {
        return false;
    }
    qlen = dns_read_name(msg + 12, len - 12, name, sizeof(name));
    if (!qlen || 12 + qlen + 4 > len) {
        return false;
    }
    qtype = lduw_be_p(msg + 12 + qlen);
    if (qtype == 12) {
        if (!dns_lookup_ptr(name, ptr, sizeof(ptr))) {
            return false;   /* not one of ours */
        }
        found = ptr[0];
    } else {
        if (!dns_lookup_a(name, &addr)) {
            return false;
        }
        found = addr && qtype == 1;
    }

    n = 12 + qlen + 4;
    memcpy(ans, msg, n);
    stw_be_p(ans + 2, 0x8400 | (msg[2] & 1) << 8 | (found ? 0 : 3));
    stw_be_p(ans + 6, found);   /* answers */
    stw_be_p(ans + 8, 0);
    stw_be_p(ans + 10, 0);
    if (found) {
        stw_be_p(ans + n, 0xC00C);   /* the name in the question */
        stw_be_p(ans + n + 2, qtype);
        stw_be_p(ans + n + 4, 1);
        stl_be_p(ans + n + 6, 60);
        n += 10;
        if (qtype == 12) {
            size_t l = dns_write_name(ans + n + 2, ptr);

            stw_be_p(ans + n, l);
            n += 2 + l;
        } else {
            stw_be_p(ans + n, 4);
            stl_be_p(ans + n + 2, addr);
            n += 6;
        }
    }
    filter_next_udp_reply(s, pkt, ADDR_DNS, PORT_DNS, ans, n);
    return true;
}

/* ---- NTP, as Previous's server ---- */

#define JAN_1970 2208988800u

static void ntp_now(uint8_t *p)
{
    gint64 us = g_get_real_time();

    stl_be_p(p, JAN_1970 + us / G_USEC_PER_SEC);
    stl_be_p(p + 4, (uint32_t)((us % G_USEC_PER_SEC) * 4294.967296));
}

static bool filter_next_ntp(FilterNeXTState *s, const uint8_t *pkt,
                            const uint8_t *udp, size_t udp_len)
{
    const uint8_t *req = udp + 8;
    uint8_t ans[48] = { 0 };
    int version;

    if (!nn_cfg.network_time || udp_len < 8 + 48) {
        return false;
    }
    version = (req[0] >> 3) & 7;
    ans[0] = (version << 3) | 4;    /* no leap second, a server */
    ans[1] = 1;                     /* stratum */
    ans[2] = 3;                     /* poll */
    ans[3] = 0xEE;                  /* precision */
    stl_be_p(ans + 12, 0x4C454D55); /* "LEMU" */
    stw_be_p(ans + 10, 8);          /* dispersion */
    ntp_now(ans + 32);              /* received */
    memcpy(ans + 16, ans + 32, 8);  /* reference */
    stl_be_p(ans + 16, ldl_be_p(ans + 16) - 4);
    memcpy(ans + 24, req + 40, 8);  /* the client's transmit time */
    ntp_now(ans + 40);              /* transmitted */
    filter_next_udp_reply(s, pkt, ADDR_HOST, PORT_NTP, ans, sizeof(ans));
    return true;
}

/* ---- the servers ---- */

static void filter_next_tcp_send(void *opaque, uint8_t *frame, size_t len)
{
    filter_next_send(opaque, true, frame, len);
}

/* A whole packet for the servers' address, or a broadcast to them */
static void filter_next_server(FilterNeXTState *s, const uint8_t *pkt,
                               size_t len)
{
    const uint8_t *ip = pkt + ETH_HLEN;
    size_t ihl = (ip[0] & 0xF) * 4;
    size_t ip_len = lduw_be_p(ip + 2);
    const uint8_t *udp = ip + ihl;
    const uint8_t *reply;
    size_t reply_len, udp_len;

    switch (ip[9]) {
    case IPPROTO_ICMP_:
        filter_next_ping(s, pkt, len);
        break;
    case IPPROTO_TCP:
        nn_tcp_input(s->tcp, pkt, len);
        break;
    case IPPROTO_UDP:
        if (ip_len < ihl + 8) {
            break;
        }
        udp_len = lduw_be_p(udp + 4);
        if (udp_len < 8 || udp_len > ip_len - ihl) {
            break;
        }
        if (nn_rpc_udp(lduw_be_p(udp + 2), udp + 8, udp_len - 8,
                       &reply, &reply_len)) {
            filter_next_udp_reply(s, pkt, ADDR_SERVER, lduw_be_p(udp + 2),
                                  reply, reply_len);
        }
        break;
    }
}

static bool reasm_block(FilterNeXTReasm *r, size_t b)
{
    return r->blocks[b / 8] & (1 << (b % 8));
}

/*
 * A fragment for the servers: when it completes its packet, hand that on.
 * Fragments that overlap simply overwrite what was there.
 */
static void filter_next_reasm(FilterNeXTState *s, const uint8_t *pkt)
{
    const uint8_t *ip = pkt + ETH_HLEN;
    size_t ihl = (ip[0] & 0xF) * 4;
    size_t n = lduw_be_p(ip + 2) - ihl;
    uint16_t frag = lduw_be_p(ip + 6);
    size_t off = (frag & 0x1FFF) * 8;
    bool more = frag & 0x2000;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    FilterNeXTReasm *r = NULL, *old = &s->reasm[0];
    g_autofree uint8_t *whole = NULL;

    if (off + n > REASM_MAX - 60 || (more && (n & 7)) || !n) {
        return;
    }
    for (int i = 0; i < REASM_SLOTS; i++) {
        FilterNeXTReasm *e = &s->reasm[i];

        if (e->used && now - e->time > REASM_TIMEOUT_MS) {
            e->used = false;
        }
        if (e->used && e->src == ldl_be_p(ip + 12) &&
            e->id == lduw_be_p(ip + 4) && e->prot == ip[9]) {
            r = e;
            break;
        }
        if (!e->used || (old->used && e->time < old->time)) {
            old = e;
        }
    }
    if (!r) {
        r = old;
        if (!r->data) {
            r->data = g_malloc(REASM_MAX);
        }
        memset(r->blocks, 0, sizeof(r->blocks));
        r->used = true;
        r->src = ldl_be_p(ip + 12);
        r->id = lduw_be_p(ip + 4);
        r->prot = ip[9];
        r->total = 0;
        r->head_len = 0;
        r->time = now;
    }
    memcpy(r->data + off, ip + ihl, n);
    for (size_t b = off / 8; b < (off + n + 7) / 8; b++) {
        r->blocks[b / 8] |= 1 << (b % 8);
    }
    if (!more) {
        r->total = off + n;
    }
    if (!off) {
        r->head_len = ETH_HLEN + ihl;
        memcpy(r->head, pkt, r->head_len);
    }
    if (!r->total || !r->head_len) {
        return;
    }
    for (size_t b = 0; b < (r->total + 7) / 8; b++) {
        if (!reasm_block(r, b)) {
            return;
        }
    }

    /* all here: one packet */
    whole = g_malloc(r->head_len + r->total);
    memcpy(whole, r->head, r->head_len);
    memcpy(whole + r->head_len, r->data, r->total);
    stw_be_p(whole + ETH_HLEN + 2, r->head_len - ETH_HLEN + r->total);
    stw_be_p(whole + ETH_HLEN + 6, 0);
    r->used = false;
    filter_next_server(s, whole, r->head_len + r->total);
}

/* ---- the guest's packets ---- */

static bool is_broadcast(uint32_t addr)
{
    return addr == 0xFFFFFFFFu || addr == (CTL_NET | ~CTL_NET_MASK) ||
           addr == (CTL_NET | ~CTL_CLASS_MASK(CTL_NET));
}

/* Returns true when the packet was taken */
static bool filter_next_from_guest(FilterNeXTState *s, uint8_t *pkt,
                                   size_t len)
{
    uint8_t *ip = pkt + ETH_HLEN;
    size_t ihl, ip_len;
    uint32_t dst;
    bool first;
    int prot;

    if (len < ETH_HLEN + 20) {
        if (len >= ETH_HLEN + 28 &&
            lduw_be_p(pkt + 12) == ETHERTYPE_ARP) {
            return filter_next_arp(s, pkt, len);
        }
        return false;
    }
    if (lduw_be_p(pkt + 12) == ETHERTYPE_ARP) {
        return filter_next_arp(s, pkt, len);
    }
    if (lduw_be_p(pkt + 12) != ETHERTYPE_IP || (ip[0] >> 4) != 4) {
        return false;
    }
    ihl = (ip[0] & 0xF) * 4;
    ip_len = lduw_be_p(ip + 2);
    if (ihl < 20 || ip_len < ihl || ip_len > len - ETH_HLEN) {
        return false;
    }
    dst = ldl_be_p(ip + 16);
    prot = ip[9];
    first = !(lduw_be_p(ip + 6) & 0x1FFF);

    if (dst == ADDR_SERVER) {
        /* all of it stays here */
        if (lduw_be_p(ip + 6) & 0x3FFF) {
            filter_next_reasm(s, pkt);
        } else {
            filter_next_server(s, pkt, ETH_HLEN + ip_len);
        }
        return true;
    }

    if (prot != IPPROTO_UDP || !first || ip_len < ihl + 8) {
        return false;
    }
    if (is_broadcast(dst) && lduw_be_p(ip + ihl + 2) == PORT_RPC &&
        !(lduw_be_p(ip + 6) & 0x2000)) {
        filter_next_server(s, pkt, ETH_HLEN + ip_len);
        return true;
    }
    if (dst == ADDR_DNS && lduw_be_p(ip + ihl + 2) == PORT_DNS) {
        return filter_next_dns(s, pkt, ip + ihl, ip_len - ihl);
    }
    if (dst == ADDR_HOST && lduw_be_p(ip + ihl + 2) == PORT_NTP) {
        return filter_next_ntp(s, pkt, ip + ihl, ip_len - ihl);
    }
    return false;
}

/* ---- the user network's packets ---- */

/* Nothing from the user network is for us: only learn its address */
static bool filter_next_from_net(FilterNeXTState *s, uint8_t *pkt,
                                 size_t len)
{
    const uint8_t *ip = pkt + ETH_HLEN;

    if (len >= ETH_HLEN + 20 && lduw_be_p(pkt + 12) == ETHERTYPE_IP &&
        (ip[0] >> 4) == 4 && ldl_be_p(ip + 12) == ADDR_HOST) {
        memcpy(s->net_mac, pkt + 6, 6);
    }
    return false;
}

static ssize_t filter_next_receive_iov(NetFilterState *nf,
                                       NetClientState *sender,
                                       unsigned flags,
                                       const struct iovec *iov,
                                       int iovcnt,
                                       NetPacketSent *sent_cb)
{
    FilterNeXTState *s = FILTER_NEXT(nf);
    size_t size = iov_size(iov, iovcnt);
    g_autofree uint8_t *pkt = NULL;
    bool taken;

    if (size < ETH_HLEN || size > 65536 + ETH_HLEN) {
        return 0;
    }
    pkt = g_malloc(size);
    iov_to_buf(iov, iovcnt, 0, pkt, size);
    if (sender == nf->netdev) {
        taken = filter_next_from_net(s, pkt, size);
    } else {
        taken = filter_next_from_guest(s, pkt, size);
    }
    return taken ? size : 0;
}

/* ---- the object ---- */

/* A host name as a NeXT takes it: lower case letters, digits and '-' */
static void copy_host_name(char *dst, const char *src, size_t maxlen)
{
    size_t len = 0;

    for (; *src && *src != '.' && len + 1 < maxlen; src++) {
        if (g_ascii_isalpha(*src) || (len && (g_ascii_isdigit(*src) ||
                                               *src == '-'))) {
            dst[len++] = g_ascii_tolower(*src);
        }
    }
    while (len && dst[len - 1] == '-') {
        len--;
    }
    dst[len] = '\0';
}

static void filter_next_setup(NetFilterState *nf, Error **errp)
{
    FilterNeXTState *s = FILTER_NEXT(nf);
    static const uint8_t slirp_mac[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

    if (filter_next_in_use) {
        error_setg(errp, "filter-next: there can be only one");
        return;
    }
    if (nf->netdev->info->type != NET_CLIENT_DRIVER_USER) {
        error_setg(errp, "filter-next: netdev '%s' is not a user network",
                   nf->netdev_id);
        return;
    }
    memcpy(s->net_mac, slirp_mac, 6);

    memset(&nn_cfg, 0, sizeof(nn_cfg));
    copy_host_name(nn_cfg.guest_name, s->guest_name ? s->guest_name : "next",
                   sizeof(nn_cfg.guest_name));
    if (!nn_cfg.guest_name[0]) {
        pstrcpy(nn_cfg.guest_name, sizeof(nn_cfg.guest_name), "next");
    }
    pstrcpy(nn_cfg.server_name, sizeof(nn_cfg.server_name), "nfs");
    copy_host_name(nn_cfg.host_name, g_get_host_name(),
                   sizeof(nn_cfg.host_name));
    if (!nn_cfg.host_name[0] ||
        !strcmp(nn_cfg.host_name, nn_cfg.guest_name) ||
        !strcmp(nn_cfg.host_name, nn_cfg.server_name)) {
        pstrcpy(nn_cfg.host_name, sizeof(nn_cfg.host_name), "host");
    }
    pstrcpy(nn_cfg.domain, sizeof(nn_cfg.domain), "home");
    pstrcpy(nn_cfg.system_type, sizeof(nn_cfg.system_type),
            s->system_type ? s->system_type : "NeXTstation");
    nn_cfg.network_time = s->network_time;
    nn_cfg.debug = s->debug;

    if (s->share && *s->share && !g_file_test(s->share, G_FILE_TEST_IS_DIR)) {
        error_setg(errp, "filter-next: '%s' is not a folder", s->share);
        return;
    }
    nn_rpc_start(s->share);
    s->tcp = nn_tcp_new(s->net_mac, ADDR_SERVER, filter_next_tcp_send, s);
    filter_next_in_use = true;
}

static void filter_next_cleanup(NetFilterState *nf)
{
    FilterNeXTState *s = FILTER_NEXT(nf);

    if (filter_next_in_use) {
        /* the connections hold the servers' programs */
        nn_tcp_free(s->tcp);
        s->tcp = NULL;
        nn_rpc_stop();
        filter_next_in_use = false;
    }
    for (int i = 0; i < REASM_SLOTS; i++) {
        g_free(s->reasm[i].data);
        s->reasm[i].data = NULL;
        s->reasm[i].used = false;
    }
}

static char *filter_next_get_share(Object *obj, Error **errp)
{
    return g_strdup(FILTER_NEXT(obj)->share);
}

static void filter_next_set_share(Object *obj, const char *v, Error **errp)
{
    FilterNeXTState *s = FILTER_NEXT(obj);

    g_free(s->share);
    s->share = g_strdup(v);
}

static char *filter_next_get_name(Object *obj, Error **errp)
{
    return g_strdup(FILTER_NEXT(obj)->guest_name);
}

static void filter_next_set_name(Object *obj, const char *v, Error **errp)
{
    FilterNeXTState *s = FILTER_NEXT(obj);

    g_free(s->guest_name);
    s->guest_name = g_strdup(v);
}

static char *filter_next_get_type(Object *obj, Error **errp)
{
    return g_strdup(FILTER_NEXT(obj)->system_type);
}

static void filter_next_set_type(Object *obj, const char *v, Error **errp)
{
    FilterNeXTState *s = FILTER_NEXT(obj);

    g_free(s->system_type);
    s->system_type = g_strdup(v);
}

static bool filter_next_get_time(Object *obj, Error **errp)
{
    return FILTER_NEXT(obj)->network_time;
}

static void filter_next_set_time(Object *obj, bool v, Error **errp)
{
    FILTER_NEXT(obj)->network_time = v;
}

static bool filter_next_get_debug(Object *obj, Error **errp)
{
    return FILTER_NEXT(obj)->debug;
}

static void filter_next_set_debug(Object *obj, bool v, Error **errp)
{
    FILTER_NEXT(obj)->debug = v;
}

static void filter_next_init(Object *obj)
{
    FILTER_NEXT(obj)->network_time = true;
}

static void filter_next_finalize(Object *obj)
{
    FilterNeXTState *s = FILTER_NEXT(obj);

    g_free(s->share);
    g_free(s->guest_name);
    g_free(s->system_type);
}

static void filter_next_class_init(ObjectClass *oc, const void *data)
{
    NetFilterClass *nfc = NETFILTER_CLASS(oc);

    object_class_property_add_str(oc, "share", filter_next_get_share,
                                  filter_next_set_share);
    object_class_property_set_description(oc, "share",
        "a host folder to share by NFS, which NeXTSTEP mounts on /Net");
    object_class_property_add_str(oc, "hostname", filter_next_get_name,
                                  filter_next_set_name);
    object_class_property_set_description(oc, "hostname",
        "the NeXT's host name (default: next)");
    object_class_property_add_str(oc, "system-type", filter_next_get_type,
                                  filter_next_set_type);
    object_class_property_add_bool(oc, "network-time", filter_next_get_time,
                                   filter_next_set_time);
    object_class_property_set_description(oc, "network-time",
        "tell NeXTSTEP to take its time from the host (default: on)");
    object_class_property_add_bool(oc, "debug", filter_next_get_debug,
                                   filter_next_set_debug);

    nfc->setup = filter_next_setup;
    nfc->cleanup = filter_next_cleanup;
    nfc->receive_iov = filter_next_receive_iov;
}

static const TypeInfo filter_next_info = {
    .name = TYPE_FILTER_NEXT,
    .parent = TYPE_NETFILTER,
    .class_init = filter_next_class_init,
    .instance_init = filter_next_init,
    .instance_finalize = filter_next_finalize,
    .instance_size = sizeof(FilterNeXTState),
};

static void filter_next_register_types(void)
{
    type_register_static(&filter_next_info);
}

type_init(filter_next_register_types);
