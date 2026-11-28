/*
 * NeXT network services: what the servers share
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The servers in this folder come from the Previous emulator's slirp/rpc
 * (created by Simon Schubiger, rewritten in C by Andreas Grabher), under
 * the permission notice kept in each file. This header replaces the parts
 * of Previous's slirp and configuration they used.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef NET_NEXT_NEXTNET_H
#define NET_NEXT_NEXTNET_H

#include "qemu/osdep.h"
#include "qemu/sockets.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"

#ifndef _WIN32
#include <sys/statvfs.h>
#endif

#if defined(__linux__)
#include <sys/xattr.h>
#define HAVE_SYS_XATTR_H 1
#define HAVE_LXETXATTR 1
#define st_atimespec st_atim
#define st_mtimespec st_mtim
#elif defined(__APPLE__)
#include <sys/xattr.h>
#define HAVE_SYS_XATTR_H 1
#define HAVE_LXETXATTR 0
#else
#define HAVE_SYS_XATTR_H 0
#define HAVE_LXETXATTR 0
#if !defined(_WIN32)
#define st_atimespec st_atim
#define st_mtimespec st_mtim
#endif
#endif

/* The user network as the guest sees it */
#define CTL_NET          0x0A000200u   /* 10.0.2.0 */
#define CTL_NET_MASK     0xFFFFFF00u
#define CTL_GATEWAY      2              /* the host */
#define CTL_DNS          3
#define CTL_HOST         15             /* the NeXT */
#define CTL_NFSD         254            /* these servers */
#define CTL_CLASS_MASK(x) \
    (((x) & 0x80000000u) == 0 ? 0xFF000000u : \
     ((x) & 0xC0000000u) == 0x80000000u ? 0xFFFF0000u : 0xFFFFFF00u)

#define NAME_HOST_MAX    256
#define NAME_DOMAIN_MAX  256

/* What the servers tell the guest */
typedef struct NeXTNetConfig {
    char guest_name[64];    /* the NeXT's host name */
    char server_name[64];   /* the name of these servers, "nfs" */
    char host_name[64];     /* the name of the host, at the gateway */
    char domain[64];        /* without a leading dot */
    char system_type[24];   /* "NeXTstation", "NeXTcube", ... */
    bool network_time;      /* tell the guest to take its time from us */
    bool debug;
} NeXTNetConfig;

extern NeXTNetConfig nn_cfg;

void nn_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* Is a program of the servers at this port? */
bool nn_rpc_has_port(int prot, uint16_t port);
/* Answer a UDP call; the reply stays in *reply until the next call */
bool nn_rpc_udp(uint16_t port, const uint8_t *data, size_t len,
                const uint8_t **reply, size_t *reply_len);

/* A TCP connection to a program: its bytes in, its replies out */
struct nn_rpc_stream;
struct nn_rpc_stream *nn_rpc_stream_new(uint16_t port);
/*
 * Replies go to out until it holds NN_RPC_STREAM_QUEUE bytes; the other
 * calls wait for the next call, which may have no data. false when the
 * connection must close.
 */
#define NN_RPC_STREAM_QUEUE (1024 * 1024)
bool nn_rpc_stream_input(struct nn_rpc_stream *st, const uint8_t *data,
                         size_t len, GByteArray *out);
void nn_rpc_stream_free(struct nn_rpc_stream *st);

bool nn_rpc_start(const char *share);
void nn_rpc_stop(void);

#endif
