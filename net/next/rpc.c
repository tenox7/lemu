/*
 * Remote Procedure Call
 *
 * Created by Simon Schubiger
 * Rewritten in C by Andreas Grabher
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
 *
 * In Previous each server has threads blocking on its sockets. Here
 * there are no sockets: the NeXT filter takes the guest's packets for
 * the servers and hands their calls here, so nothing outside the guest
 * reaches them.
 */

#include "nextnet.h"
#include "qemu/log.h"

#include "rpc.h"
#include "portmap.h"
#include "mount.h"
#include "nfs.h"
#include "bootparam.h"
#include "netinfobind.h"

#define MAX_RECORD (1024 * 1024)

NeXTNetConfig nn_cfg;

void nn_log(const char *fmt, ...)
{
    va_list ap;

    if (!nn_cfg.debug) {
        return;
    }
    va_start(ap, fmt);
    fprintf(stderr, "next-net: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static const struct rpc_prog_t rpc_prog_table_template[] = {
    { PORTMAPPROG,   PORTMAPVERS,   IPPROTO_UDP, PORT_RPC, portmap_prog,   1, "PORTMAP" },
    { PORTMAPPROG,   PORTMAPVERS,   IPPROTO_TCP, PORT_RPC, portmap_prog,   1, "PORTMAP" },
    { BOOTPARAMPROG, BOOTPARAMVERS, IPPROTO_UDP, 0,        bootparam_prog, 1, "BOOTPARAM" },
    { NIBINDPROG,    NIBINDVERS,    IPPROTO_UDP, 0,        nibind_prog,    1, "NETINFOBIND" },
    { NIBINDPROG,    NIBINDVERS,    IPPROTO_TCP, 0,        nibind_prog,    1, "NETINFOBIND" },
};

/* Only with a shared folder */
static const struct rpc_prog_t rpc_nfs_table_template[] = {
    { MOUNTPROG,     MOUNTVERS,     IPPROTO_UDP, 0,        mount_prog,     1, "MOUNT" },
    { NFSPROG,       NFSVERS,       IPPROTO_UDP, PORT_NFS, nfs_prog,       1, "NFS" },
    { NFSPROG,       NFSVERS,       IPPROTO_TCP, PORT_NFS, nfs_prog,       1, "NFS" },
};

static struct rpc_t *rpc_server;
static struct xdr_t *rpc_in;
static struct xdr_t *rpc_out;
static uint32_t rpc_out_capacity;

int rpc_match_prog(struct rpc_t *rpc, struct rpc_prog_t *prog)
{
    if (prog->prog == rpc->prog && prog->prot == rpc->prot) {
        rpc->name = prog->name;
        rpc->log  = prog->log;
        if (prog->vers == rpc->vers) {
            return 1;
        }
        return -1;
    }
    return 0;
}

static int rpc_call(struct rpc_t *rpc)
{
    int result, mismatch;
    struct rpc_prog_t *prog = rpc->prog_list;

    mismatch  = 0;
    rpc->low = ~0;
    rpc->high = 0;

    while (prog) {
        if (prog->port == rpc->port) {
            result = rpc_match_prog(rpc, prog);
            if (result > 0) {
                return prog->run(rpc);
            }
            if (result < 0) {
                mismatch = 1;
                if (prog->vers < rpc->low) {
                    rpc->low  = prog->vers;
                }
                if (prog->vers > rpc->high) {
                    rpc->high = prog->vers;
                }
            }
        }
        prog = prog->next;
    }

    return mismatch ? RPC_PROG_MISMATCH : RPC_PROG_UNAVAIL;
}

/* false for credentials that cannot be right */
static bool rpc_read_auth(struct rpc_t *rpc)
{
    struct xdr_t *m_in = rpc->m_in;
    int len = rpc->auth.length;

    if (rpc->auth.flavor == RPC_AUTH_UNIX) {
        uint32_t i;
        struct auth_unix_t *auth = &rpc->auth_unix;
        len -= 5 * 4;
        auth->time = xdr_read_long(m_in);
        len -= xdr_read_string(m_in, auth->machine, sizeof(auth->machine));
        len &= ~3; /* align */
        auth->uid  = xdr_read_long(m_in);
        auth->gid  = xdr_read_long(m_in);
        auth->len  = xdr_read_long(m_in);
        /* RFC 5531: at most 16 groups */
        if (auth->len > NUM_GROUPS) {
            nn_log("RPC: %u groups in the credentials\n", auth->len);
            return false;
        }
        len -= auth->len * 4;
        for (i = 0; i < auth->len; i++) {
            auth->gids[i] = xdr_read_long(m_in);
        }
        if (rpc->ft) {
            vfs_set_process_uid_gid(rpc->ft->vfs, auth->uid, auth->gid);
        }
    } else {
        if (rpc->auth.flavor != RPC_AUTH_NONE) {
            nn_log("RPC: authentication type %d not supported\n",
                   rpc->auth.flavor);
        }
        len = xdr_read_skip(m_in, len);
        if (rpc->ft) {
            vfs_set_process_uid_gid(rpc->ft->vfs, 0, 0);
        }
    }
    if (len) {
        nn_log("RPC: authentication decode error\n");
    }
    return true;
}

/*
 * Answer the call in rpc_in into rpc_out. Returns false when there is
 * nothing to send.
 */
static bool rpc_input(struct rpc_prog_t *prog)
{
    uint32_t status;
    uint8_t *status_ptr;
    struct rpc_t *rpc = prog->rpc;
    struct xdr_t *m_in = rpc_in;
    struct xdr_t *m_out = rpc_out;

    rpc->m_in  = m_in;
    rpc->m_out = m_out;
    rpc->port  = prog->port;
    rpc->prot  = prog->prot;

    rpc->xid = xdr_read_long(m_in);
    rpc->msg = xdr_read_long(m_in);
    if (rpc->msg != RPC_CALL) {
        nn_log("RPC: %s received\n",
               rpc->msg == RPC_REPLY ? "reply" : "unknown message");
        return false;
    }
    xdr_write_long(m_out, rpc->xid);
    xdr_write_long(m_out, RPC_REPLY);
    rpc->rpcvers = xdr_read_long(m_in);
    if (rpc->rpcvers == RPCVERS) {
        rpc->prog = xdr_read_long(m_in);
        rpc->vers = xdr_read_long(m_in);
        rpc->proc = xdr_read_long(m_in);
        rpc->auth.flavor = xdr_read_long(m_in);
        rpc->auth.length = xdr_read_long(m_in);
        rpc->verif.length = 0;
        if (rpc->auth.length <= RPC_MAX_AUTH_BYTES && rpc_read_auth(rpc)) {
            rpc->verif.flavor = xdr_read_long(m_in);
            rpc->verif.length = xdr_read_long(m_in);
        } else {
            rpc->auth.length = ~0u;     /* refused below */
        }
        if (rpc->auth.length > RPC_MAX_AUTH_BYTES ||
            rpc->verif.length > RPC_MAX_AUTH_BYTES ||
            xdr_read_skip(m_in, rpc->verif.length) < 0) {
            nn_log("RPC: bad credentials or verifier\n");
            xdr_write_long(m_out, RPC_MSG_DENIED);
            xdr_write_long(m_out, RPC_AUTH_ERROR);
            xdr_write_long(m_out, RPC_AUTH_BADCRED);
            return true;
        }

        /* our verifier: none */
        xdr_write_long(m_out, RPC_MSG_ACCEPTED);
        xdr_write_long(m_out, RPC_AUTH_NONE);
        xdr_write_long(m_out, 0);
        status_ptr = xdr_get_pointer(m_out);
        xdr_write_long(m_out, 0); /* the status goes here */

        status = rpc_call(rpc);

        xdr_write_long_at(m_out, status_ptr, status);

        if (status == RPC_PROG_MISMATCH) {
            rpc_log(rpc, "Version mismatch: req %d, min %d, max %d",
                    rpc->vers, rpc->low, rpc->high);
            xdr_write_long(m_out, rpc->low);
            xdr_write_long(m_out, rpc->high);
        } else if (status == RPC_PROG_UNAVAIL) {
            nn_log("RPC: program %d not registered (procedure %d)\n",
                   rpc->prog, rpc->proc);
        } else if (status == RPC_GARBAGE_ARGS) {
            rpc_log(rpc, "Procedure cannot decode input (garbage args)");
        } else if (status == RPC_PROC_UNAVAIL) {
            rpc_log(rpc, "Procedure not available");
        }
    } else {
        nn_log("RPC: version mismatch (%d)\n", rpc->rpcvers);
        xdr_write_long(m_out, RPC_MSG_DENIED);
        xdr_write_long(m_out, RPC_MISMATCH);
        xdr_write_long(m_out, 2); /* lowest version */
        xdr_write_long(m_out, 2); /* highest version */
    }
    if (m_out->error) {
        nn_log("RPC: the reply does not fit\n");
        return false;
    }
    return true;
}

/* The reply starts out_room bytes into rpc_out, after a record mark */
static void rpc_out_reset(size_t out_room)
{
    rpc_out->data = rpc_out->head + out_room;
    rpc_out->size = 0;
    rpc_out->capacity = rpc_out_capacity - out_room;
    rpc_out->error = false;
}

static void rpc_xdr_reset(const uint8_t *data, uint32_t len, size_t out_room)
{
    memcpy(rpc_in->head, data, len);
    rpc_in->data = rpc_in->head;
    rpc_in->size = len;
    rpc_out_reset(out_room);
}

int proc_null(struct rpc_t *rpc)
{
    rpc_log(rpc, "NULL");
    return RPC_SUCCESS;
}

/* ---- calls from the NeXT filter ---- */

/* The ports of the programs the guest finds by the port mapper */
#define RPC_FIRST_PORT 600

/* A TCP connection: RPC messages come in records */
struct nn_rpc_stream {
    struct rpc_prog_t *prog;
    uint8_t *buf;
    size_t len;
};

static uint16_t rpc_next_port = RPC_FIRST_PORT;

static struct rpc_prog_t *rpc_find(int prot, uint16_t port)
{
    struct rpc_prog_t *prog = rpc_server ? rpc_server->prog_list : NULL;

    for (; prog; prog = prog->next) {
        if (prog->prot == prot && prog->port == port) {
            return prog;
        }
    }
    return NULL;
}

bool nn_rpc_has_port(int prot, uint16_t port)
{
    return rpc_find(prot, port) != NULL;
}

bool nn_rpc_udp(uint16_t port, const uint8_t *data, size_t len,
                const uint8_t **reply, size_t *reply_len)
{
    struct rpc_prog_t *prog = rpc_find(IPPROTO_UDP, port);

    if (!prog || len > rpc_in->capacity) {
        return false;
    }
    rpc_xdr_reset(data, len, 0);
    if (!rpc_input(prog)) {
        return false;
    }
    *reply = rpc_out->head;
    *reply_len = rpc_out->size;
    return true;
}

struct nn_rpc_stream *nn_rpc_stream_new(uint16_t port)
{
    struct rpc_prog_t *prog = rpc_find(IPPROTO_TCP, port);
    struct nn_rpc_stream *st;

    if (!prog) {
        return NULL;
    }
    st = g_new0(struct nn_rpc_stream, 1);
    st->prog = prog;
    return st;
}

void nn_rpc_stream_free(struct nn_rpc_stream *st)
{
    if (st) {
        g_free(st->buf);
        g_free(st);
    }
}

/*
 * Each record has fragments, each with a 4-byte mark: the last one has
 * bit 31 set, and the other bits give its length. The replies, as
 * records, go to out.
 */
static void rpc_stream_messages(struct nn_rpc_stream *st, GByteArray *out)
{
    /* when the guest is slow to take the replies, the rest waits */
    while (out->len < NN_RPC_STREAM_QUEUE) {
        size_t off = 0, msg_len = 0;
        bool last = false;
        g_autofree uint8_t *msg = NULL;

        /* is a whole record here? */
        while (!last) {
            uint32_t mark;

            if (st->len < off + 4) {
                return;
            }
            mark = ldl_be_p(st->buf + off);
            last = mark & 0x80000000u;
            mark &= 0x7FFFFFFFu;
            if (st->len - off - 4 < mark) {
                return;
            }
            off += 4 + mark;
            msg_len += mark;
        }

        /* put the fragments together */
        msg = g_malloc(msg_len ? msg_len : 1);
        msg_len = 0;
        for (size_t p = 0; p < off;) {
            uint32_t mark = ldl_be_p(st->buf + p) & 0x7FFFFFFFu;

            memcpy(msg + msg_len, st->buf + p + 4, mark);
            msg_len += mark;
            p += 4 + mark;
        }
        memmove(st->buf, st->buf + off, st->len - off);
        st->len -= off;

        rpc_xdr_reset(msg, msg_len, 4);
        if (rpc_input(st->prog)) {
            stl_be_p(rpc_out->head, 0x80000000u | rpc_out->size);
            g_byte_array_append(out, rpc_out->head, rpc_out->size + 4);
        }
    }
}

bool nn_rpc_stream_input(struct nn_rpc_stream *st, const uint8_t *data,
                         size_t len, GByteArray *out)
{
    /* the whole record must fit in rpc_in */
    if (len > MAX_RECORD - st->len) {
        nn_log("RPC: record too long\n");
        return false;
    }
    if (len) {
        st->buf = g_realloc(st->buf, st->len + len);
        memcpy(st->buf + st->len, data, len);
        st->len += len;
    }
    rpc_stream_messages(st, out);
    return true;
}

void rpc_add_program(struct rpc_t *rpc, struct rpc_prog_t *prog)
{
    struct rpc_prog_t **entry = &rpc->prog_list;
    bool tcp = prog->prot == IPPROTO_TCP;

    prog->rpc = rpc;
    if (prog->port == 0) {
        prog->port = rpc_next_port++;
    }
    nn_log("RPC: %s started (%s: %d)\n", prog->name,
           tcp ? "TCP" : "UDP", prog->port);

    while (*entry) {
        entry = &(*entry)->next;
    }
    *entry = prog;
    prog->next = NULL;
}

static void rpc_remove_all_programs(struct rpc_t *rpc)
{
    while (rpc->prog_list) {
        struct rpc_prog_t *next = rpc->prog_list->next;

        g_free(rpc->prog_list);
        rpc->prog_list = next;
    }
    rpc_next_port = RPC_FIRST_PORT;
}

static void rpc_add_table(struct rpc_t *rpc, const struct rpc_prog_t *table,
                          size_t n)
{
    for (size_t i = 0; i < n; i++) {
        rpc_add_program(rpc, g_memdup2(&table[i], sizeof(table[i])));
    }
}

bool nn_rpc_start(const char *share)
{
    struct rpc_t *rpc;

    if (rpc_server) {
        return true;
    }
    rpc = rpc_server = g_new0(struct rpc_t, 1);
    rpc_in = xdr_init();
    rpc_out = xdr_init();
    rpc_out_capacity = rpc_out->capacity;
    pstrcpy(rpc->hostname, sizeof(rpc->hostname), nn_cfg.server_name);
    rpc->ip_addr = CTL_NET | CTL_NFSD;

    if (share && *share) {
        rpc->ft = ft_init(share, "/");
        if (!rpc->ft) {
            nn_log("RPC: cannot share '%s'\n", share);
        }
    }

    netinfo_build_nidb();
    /* the server of the network domain, and of a share if there is one */
    netinfo_add_host(rpc->hostname, rpc->ip_addr, rpc->ft != NULL);
    rpc_add_table(rpc, rpc_prog_table_template,
                  TBL_SIZE(rpc_prog_table_template));
    if (rpc->ft) {
        rpc_add_table(rpc, rpc_nfs_table_template,
                      TBL_SIZE(rpc_nfs_table_template));
    }
    nibind_init(rpc);
    return true;
}

void nn_rpc_stop(void)
{
    struct rpc_t *rpc = rpc_server;

    if (!rpc) {
        return;
    }
    rpc_remove_all_programs(rpc);
    nibind_uninit(rpc);
    mount_uninit(rpc);
    netinfo_delete_nidb();
    rpc->ft = ft_uninit(rpc->ft);
    xdr_uninit(rpc_in);
    xdr_uninit(rpc_out);
    g_free(rpc);
    rpc_server = NULL;
}

const char *rpc_ip_str(char *buf, uint32_t addr, int count, size_t maxsize)
{
    switch (count) {
    case 3:
        snprintf(buf, maxsize, "%u.%u.%u", (addr >> 24) & 0xFF,
                 (addr >> 16) & 0xFF, (addr >> 8) & 0xFF);
        break;
    case 4:
        snprintf(buf, maxsize, "%u.%u.%u.%u", (addr >> 24) & 0xFF,
                 (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF);
        break;
    default:
        buf[0] = '\0';
        break;
    }
    return buf;
}

void rpc_log(struct rpc_t *rpc, const char *format, ...)
{
    va_list vargs;

    if (rpc->log && nn_cfg.debug) {
        va_start(vargs, format);
        fprintf(stderr, "next-net: %s: %s %d: ", rpc->hostname, rpc->name,
                rpc->proc);
        vfprintf(stderr, format, vargs);
        fprintf(stderr, "\n");
        va_end(vargs);
    }
}
