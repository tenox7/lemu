/*
 * Network File System Program
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

#ifndef _NFS_H_
#define _NFS_H_

#define PORT_NFS           2049

#define NFSPROG            100003
#define NFSVERS            2

#define NFSPROC_NULL       0
#define NFSPROC_GETATTR    1
#define NFSPROC_SETATTR    2
#define NFSPROC_ROOT       3
#define NFSPROC_LOOKUP     4
#define NFSPROC_READLINK   5
#define NFSPROC_READ       6
#define NFSPROC_WRITECACHE 7
#define NFSPROC_WRITE      8
#define NFSPROC_CREATE     9
#define NFSPROC_REMOVE     10
#define NFSPROC_RENAME     11
#define NFSPROC_LINK       12
#define NFSPROC_SYMLINK    13
#define NFSPROC_MKDIR      14
#define NFSPROC_RMDIR      15
#define NFSPROC_READDIR    16
#define NFSPROC_STATFS     17

int nfs_prog(struct rpc_t* rpc);

#endif /* _NFS_H_ */
