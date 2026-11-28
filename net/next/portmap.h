/*
 * Portmap Program
 *
 * Created by Simon Schubiger on 09.01.2021
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

#ifndef _PORTMAP_H_
#define _PORTMAP_H_

#define PORTMAPPORT         111
#define PORTMAPPROG         100000
#define PORTMAPVERS         2

#define PORTMAPVERS_PROTO   2
#define PORTMAPVERS_ORIG    1

#define PORTMAPPROC_NULL    0
#define PORTMAPPROC_SET     1
#define PORTMAPPROC_UNSET   2
#define PORTMAPPROC_GETPORT 3
#define PORTMAPPROC_DUMP    4
#define PORTMAPPROC_CALLIT  5

int portmap_prog(struct rpc_t* rpc);

#endif /* _PORTMAP_H_ */
