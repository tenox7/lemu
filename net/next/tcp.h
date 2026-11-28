/*
 * NeXT network services: TCP for the RPC servers, inside the filter
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef NET_NEXT_TCP_H
#define NET_NEXT_TCP_H

typedef struct NNTcp NNTcp;

/* Send an Ethernet frame to the guest */
typedef void NNTcpSend(void *opaque, uint8_t *frame, size_t len);

/* our_mac is read at each send: the filter learns it later */
NNTcp *nn_tcp_new(const uint8_t *our_mac, uint32_t our_addr,
                  NNTcpSend *send, void *opaque);
void nn_tcp_free(NNTcp *t);

/* A TCP segment from the guest for our_addr: the frame, its IP header checked */
void nn_tcp_input(NNTcp *t, const uint8_t *frame, size_t len);

#endif
