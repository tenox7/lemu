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
 */

#ifndef NET_NEXT_RPC_H
#define NET_NEXT_RPC_H

#include "nextnet.h"
#include "xdr.h"
#include "filetable.h"

#define PORT_RPC 111

#define RPCVERS 2

#define RPC_CALL          0
#define RPC_REPLY         1

#define RPC_MISMATCH      0  /* RPC version number != 2          */
#define RPC_AUTH_ERROR    1  /* remote can't authenticate caller */

#define RPC_MSG_ACCEPTED  0
#define RPC_MSG_DENIED    1

#define RPC_SUCCESS       0
#define RPC_PROG_UNAVAIL  1
#define RPC_PROG_MISMATCH 2
#define RPC_PROC_UNAVAIL  3
#define RPC_GARBAGE_ARGS  4

#define RPC_AUTH_BADCRED  1  /* bad credentials (seal broken) */

/* RFC 5531: the body of a credential or verifier has at most 400 bytes */
#define RPC_MAX_AUTH_BYTES 400

#define RPC_AUTH_NONE     0
#define RPC_AUTH_UNIX     1
#define RPC_AUTH_SHORT    2
#define RPC_AUTH_DES      3

#define MAX_MACHINE_NAME  255
#define NUM_GROUPS        16

#define FHSIZE            32

#define MAX_HOST_NAME     64

struct auth_unix_t {
    uint32_t time;
    char machine[MAX_MACHINE_NAME];
    uint32_t uid;
    uint32_t gid;
    uint32_t len;
    uint32_t gids[NUM_GROUPS];
};

struct cred_t {
    uint32_t flavor;
    uint32_t length;
    void *data;
};

struct rpc_t {
    uint32_t xid;
    uint32_t msg;
    uint32_t rpcvers;
    uint32_t prog;
    uint32_t vers;
    uint32_t proc;
    struct cred_t auth;
    struct cred_t verif;

    struct auth_unix_t auth_unix;

    struct xdr_t *m_in;
    struct xdr_t *m_out;

    struct ft_t *ft;            /* the shared folder, or NULL */

    uint16_t port;
    uint32_t prot;
    uint32_t low;
    uint32_t high;

    uint32_t ip_addr;
    char hostname[MAX_HOST_NAME];

    struct rpc_prog_t *prog_list;
    struct nireg_t *nireg;
    struct mount_t *rmtab;

    int log;
    const char *name;
};

struct rpc_prog_t {
    uint32_t prog;
    uint32_t vers;
    uint16_t prot;
    uint16_t port;              /* as the guest sees it */

    int (*run)(struct rpc_t *rpc);
    int          log;
    const char  *name;

    struct rpc_t *rpc;

    struct rpc_prog_t *next;
};

#define TBL_SIZE(x) (sizeof(x) / sizeof(x[0]))

void rpc_add_program(struct rpc_t *rpc, struct rpc_prog_t *prog);

int proc_null(struct rpc_t *rpc);

int rpc_match_prog(struct rpc_t *rpc, struct rpc_prog_t *prog);

const char *rpc_ip_str(char *buf, uint32_t addr, int count, size_t maxsize);
void rpc_log(struct rpc_t *rpc, const char *format, ...) G_GNUC_PRINTF(2, 3);

#endif
