/*
 * Mount Program
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

#ifndef _MOUNT_H_
#define _MOUNT_H_

#define MOUNTPROG           100005
#define MOUNTVERS           1

#define MOUNTVERS_ORIG      1

#define MOUNTPROC_NULL      0
#define MOUNTPROC_MNT       1
#define MOUNTPROC_DUMP      2
#define MOUNTPROC_UMNT      3
#define MOUNTPROC_UMNTALL   4
#define MOUNTPROC_EXPORT    5
#define MOUNTPROC_EXPORTALL 6

int mount_prog(struct rpc_t* rpc);

void mount_uninit(struct rpc_t* rpc);

#endif /* _MOUNT_H_ */
