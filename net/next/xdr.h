/*
 * External Data Representation
 *
 * Copyright (c) 2025 Andreas Grabher
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

#ifndef _XDR_H_
#define _XDR_H_

struct xdr_t {
    uint8_t* data;
    uint8_t* head;
    uint32_t size;
    uint32_t capacity;
    bool     error;     /* a write did not fit */
};

uint32_t xdr_read_long(struct xdr_t* xdr);
int      xdr_read_string(struct xdr_t* xdr, char* str, uint32_t maxlen);
int      xdr_read_data(struct xdr_t* xdr, void* data, uint32_t len);
int      xdr_read_skip(struct xdr_t* xdr, uint32_t len);

void xdr_write_long(struct xdr_t* xdr, uint32_t val);
void xdr_write_string(struct xdr_t* xdr, const char* str, uint32_t maxlen);
void xdr_write_data(struct xdr_t* xdr, void* data, uint32_t len);
void xdr_write_skip(struct xdr_t* xdr, uint32_t len);

void xdr_write_zero(struct xdr_t* xdr, uint32_t len);
int  xdr_write_check(struct xdr_t* xdr, uint32_t len);

uint8_t* xdr_get_pointer(struct xdr_t* xdr);
void     xdr_write_long_at(struct xdr_t* xdr, uint8_t* data, uint32_t val);
uint32_t xdr_read_long_at(uint8_t* data);

struct xdr_t* xdr_init(void);
void xdr_uninit(struct xdr_t* xdr);

#endif /* _XDR_H_ */
