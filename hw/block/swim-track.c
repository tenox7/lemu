/*
 * Macintosh floppy tracks as the SWIM2 sees them: GCR nibbles and MFM bytes
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The GCR tables, the 6-and-2 encoding with its running checksum and the
 * header layout are ported from MAME's src/lib/formats/flopimg.cpp and
 * ap_dsk35.cpp (BSD-3-Clause, copyright-holders: Olivier Galibert).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A track is kept as the bytes the SWIM2 hands the CPU, not as flux:
 * - GCR: the nibbles (MSB set) its shift register frames, with the
 *   self-sync bytes of the gaps read as 0xFF;
 * - MFM: the bytes after the clock is found, with SWIM_MARK on the A1 and
 *   C2 bytes that have a missing clock.
 */

#include "qemu/osdep.h"
#include "swim-track.h"

static const uint8_t gcr6fw[0x40] = {
    0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6,
    0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc,
    0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
    0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
    0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
    0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
    0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

/* The reverse of gcr6fw, 0xff for a byte that is not a nibble */
static uint8_t gcr6bw[0x100];

static void gcr6bw_init(void)
{
    static bool done;

    if (!done) {
        memset(gcr6bw, 0xff, sizeof(gcr6bw));
        for (int i = 0; i < 0x40; i++) {
            gcr6bw[gcr6fw[i]] = i;
        }
        done = true;
    }
}

static uint8_t gcr6_dec(uint8_t e)
{
    return gcr6bw[e] == 0xff ? 0 : gcr6bw[e];
}

static bool gcr6_ok(uint8_t e)
{
    return gcr6bw[e] != 0xff;
}

uint16_t swim_crc_byte(uint16_t crc, uint8_t b)
{
    crc ^= b << 8;
    for (int i = 0; i < 8; i++) {
        crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

/* Layouts */

const char *swim_layout_name(SwimLayout l)
{
    switch (l) {
    case SWIM_LAYOUT_GCR400:
        return "400K GCR";
    case SWIM_LAYOUT_GCR800:
        return "800K GCR";
    case SWIM_LAYOUT_MFM360:
        return "360K MFM";
    case SWIM_LAYOUT_MFM720:
        return "720K MFM";
    case SWIM_LAYOUT_MFM1440:
        return "1.44M MFM";
    default:
        return "none";
    }
}

SwimLayout swim_layout_from_size(int64_t size)
{
    switch (size) {
    case 409600:
        return SWIM_LAYOUT_GCR400;
    case 819200:
        return SWIM_LAYOUT_GCR800;
    case 368640:
        return SWIM_LAYOUT_MFM360;
    case 737280:
        return SWIM_LAYOUT_MFM720;
    case 1474560:
        return SWIM_LAYOUT_MFM1440;
    default:
        return SWIM_LAYOUT_NONE;
    }
}

int64_t swim_layout_size(SwimLayout l)
{
    switch (l) {
    case SWIM_LAYOUT_GCR400:
        return 409600;
    case SWIM_LAYOUT_GCR800:
        return 819200;
    case SWIM_LAYOUT_MFM360:
        return 368640;
    case SWIM_LAYOUT_MFM720:
        return 737280;
    case SWIM_LAYOUT_MFM1440:
        return 1474560;
    default:
        return 0;
    }
}

SwimEnc swim_layout_enc(SwimLayout l)
{
    switch (l) {
    case SWIM_LAYOUT_GCR400:
    case SWIM_LAYOUT_GCR800:
        return SWIM_ENC_GCR;
    case SWIM_LAYOUT_MFM360:
    case SWIM_LAYOUT_MFM720:
    case SWIM_LAYOUT_MFM1440:
        return SWIM_ENC_MFM;
    default:
        return SWIM_ENC_NONE;
    }
}

int swim_layout_heads(SwimLayout l)
{
    return l == SWIM_LAYOUT_GCR400 || l == SWIM_LAYOUT_MFM360 ? 1 : 2;
}

bool swim_layout_hd(SwimLayout l)
{
    return l == SWIM_LAYOUT_MFM1440;
}

static int gcr_spt(int cyl)
{
    return 12 - MIN(cyl / 16, 4);
}

int swim_layout_spt(SwimLayout l, int cyl)
{
    switch (swim_layout_enc(l)) {
    case SWIM_ENC_GCR:
        return gcr_spt(cyl);
    case SWIM_ENC_MFM:
        return l == SWIM_LAYOUT_MFM1440 ? 18 : 9;
    default:
        return 0;
    }
}

int64_t swim_layout_offset(SwimLayout l, int cyl, int head, int idx)
{
    int heads = swim_layout_heads(l);
    int spt = swim_layout_spt(l, cyl);
    int64_t lba = 0;

    if (!spt || cyl < 0 || cyl >= SWIM_CYLS || head < 0 || head >= heads ||
        idx < 0 || idx >= spt) {
        return -1;
    }
    if (swim_layout_enc(l) == SWIM_ENC_GCR) {
        for (int c = 0; c < cyl; c++) {
            lba += gcr_spt(c) * heads;
        }
    } else {
        lba = (int64_t)cyl * heads * spt;
    }
    lba += head * spt + idx;
    return lba * SWIM_SECTOR;
}

/* Tracks */

/* Bit cells in one turn of a GCR track (2 us cells), from MAME */
static int gcr_cells(int cyl)
{
    static const int rpm[5] = { 394, 429, 472, 525, 590 };

    return 30318342 / rpm[MIN(cyl / 16, 4)];
}

/* Nibbles of one GCR sector as swim_track_build() writes it */
#define GCR_SECTOR_NIBBLES  767
#define GCR_SECTOR_CELLS    6208
/* Room for a format that writes a little more than one turn holds */
#define GCR_SLACK           64

int swim_track_rpm(SwimEnc enc, int cyl)
{
    static const int rpm[5] = { 394, 429, 472, 525, 590 };

    return enc == SWIM_ENC_GCR ? rpm[MIN(cyl / 16, 4)] : 300;
}

static int gcr_pregap(int cyl)
{
    /* 5 sync nibbles (FF and four 10-bit FFs) take 48 cells */
    return (gcr_cells(cyl) - GCR_SECTOR_CELLS * gcr_spt(cyl)) * 5 / 48;
}

int swim_track_len(SwimEnc enc, bool hd, int cyl)
{
    switch (enc) {
    case SWIM_ENC_GCR:
        return gcr_pregap(cyl) + gcr_spt(cyl) * GCR_SECTOR_NIBBLES +
               GCR_SLACK;
    case SWIM_ENC_MFM:
        return hd ? 12500 : 6250;
    default:
        return 0;
    }
}

static int put_n(uint16_t *buf, int p, uint16_t v, int n)
{
    while (n--) {
        buf[p++] = v;
    }
    return p;
}

static int put_gcr_sector(uint16_t *buf, int p, int cyl, int head, int sector,
                          uint8_t format, const uint8_t *data)
{
    uint8_t raw[12 + SWIM_SECTOR] = { 0 };     /* 12 tag bytes, then data */
    uint8_t side = (cyl & 0x40 ? 1 : 0) | (head ? 0x20 : 0);
    uint8_t check = cyl ^ sector ^ side ^ format;
    uint8_t ca = 0, cb = 0, cc = 0;

    memcpy(raw + 12, data, SWIM_SECTOR);

    p = put_n(buf, p, 0xff, 40);
    buf[p++] = 0xd5;
    buf[p++] = 0xaa;
    buf[p++] = 0x96;
    buf[p++] = gcr6fw[cyl & 0x3f];
    buf[p++] = gcr6fw[sector & 0x3f];
    buf[p++] = gcr6fw[side & 0x3f];
    buf[p++] = gcr6fw[format & 0x3f];
    buf[p++] = gcr6fw[check & 0x3f];
    buf[p++] = 0xde;
    buf[p++] = 0xaa;
    buf[p++] = 0xff;
    p = put_n(buf, p, 0xff, 5);
    buf[p++] = 0xd5;
    buf[p++] = 0xaa;
    buf[p++] = 0xad;
    buf[p++] = gcr6fw[sector & 0x3f];

    for (int i = 0; i < 175; i++) {
        uint8_t va = raw[3 * i];
        uint8_t vb = raw[3 * i + 1];
        uint8_t vc = i != 174 ? raw[3 * i + 2] : 0;
        uint16_t suma, sumb;

        cc = (cc << 1) | (cc >> 7);
        suma = ca + va + (cc & 1);
        ca = suma;
        va ^= cc;
        sumb = cb + vb + (suma >> 8);
        cb = sumb;
        vb ^= ca;
        if (i != 174) {
            cc = cc + vc + (sumb >> 8);
        }
        vc ^= cb;

        buf[p++] = gcr6fw[((va >> 2) & 0x30) | ((vb >> 4) & 0x0c) |
                          ((vc >> 6) & 0x03)];
        buf[p++] = gcr6fw[va & 0x3f];
        buf[p++] = gcr6fw[vb & 0x3f];
        if (i != 174) {
            buf[p++] = gcr6fw[vc & 0x3f];
        }
    }
    buf[p++] = gcr6fw[((ca >> 2) & 0x30) | ((cb >> 4) & 0x0c) |
                      ((cc >> 6) & 0x03)];
    buf[p++] = gcr6fw[ca & 0x3f];
    buf[p++] = gcr6fw[cb & 0x3f];
    buf[p++] = gcr6fw[cc & 0x3f];
    buf[p++] = 0xde;
    buf[p++] = 0xaa;
    buf[p++] = 0xff;
    buf[p++] = 0xff;
    return p;
}

static int put_mfm_mark(uint16_t *buf, int p, uint8_t mark, uint8_t am)
{
    p = put_n(buf, p, 0x00, 12);
    p = put_n(buf, p, SWIM_MARK | mark, 3);
    buf[p++] = am;
    return p;
}

static int put_crc(uint16_t *buf, int p, int from)
{
    uint16_t crc = SWIM_CRC_AFTER_MARKS;

    for (int i = from; i < p; i++) {
        crc = swim_crc_byte(crc, buf[i]);
    }
    buf[p++] = crc >> 8;
    buf[p++] = crc & 0xff;
    return p;
}

int swim_track_build(uint16_t *buf, SwimLayout l, int cyl, int head,
                     const uint8_t *data)
{
    SwimEnc enc = swim_layout_enc(l);
    int spt = swim_layout_spt(l, cyl);
    int len = swim_track_len(enc, swim_layout_hd(l), cyl);
    int p = 0;

    gcr6bw_init();

    if (enc == SWIM_ENC_GCR) {
        uint8_t format = swim_layout_heads(l) == 2 ? 0x22 : 0x02;
        int order[SWIM_MAX_SPT];
        int slot = 0;

        /* 2:1 interleave: logical sector i goes to physical slot order */
        for (int i = 0; i < spt; i++) {
            order[slot] = i;
            slot = (slot + 2) % spt;
            if (slot == 0) {
                slot++;
            }
        }
        p = put_n(buf, p, 0xff, gcr_pregap(cyl));
        for (int s = 0; s < spt; s++) {
            p = put_gcr_sector(buf, p, cyl, head, order[s], format,
                               data + order[s] * SWIM_SECTOR);
        }
        p = put_n(buf, p, 0xff, len - p);
    } else if (enc == SWIM_ENC_MFM) {
        int gap3 = spt == 18 ? 108 : 80;

        p = put_n(buf, p, 0x4e, 80);
        p = put_n(buf, p, 0x00, 12);
        p = put_n(buf, p, SWIM_MARK | 0xc2, 3);
        buf[p++] = 0xfc;
        p = put_n(buf, p, 0x4e, 50);
        for (int s = 0; s < spt; s++) {
            int from;

            p = put_mfm_mark(buf, p, 0xa1, 0xfe);
            from = p - 1;
            buf[p++] = cyl;
            buf[p++] = head;
            buf[p++] = s + 1;
            buf[p++] = 2;
            p = put_crc(buf, p, from);
            p = put_n(buf, p, 0x4e, 22);
            p = put_mfm_mark(buf, p, 0xa1, 0xfb);
            from = p - 1;
            for (int i = 0; i < SWIM_SECTOR; i++) {
                buf[p++] = data[s * SWIM_SECTOR + i];
            }
            p = put_crc(buf, p, from);
            p = put_n(buf, p, 0x4e, gap3);
        }
        p = put_n(buf, p, 0x4e, len - p);
    }
    assert(p == len && len <= SWIM_TRACK_MAX);
    return len;
}

int swim_track_blank(uint16_t *buf, SwimEnc enc, bool hd, int cyl)
{
    int len = swim_track_len(enc, hd, cyl);

    put_n(buf, 0, enc == SWIM_ENC_GCR ? 0xff : 0x4e, len);
    return len;
}

/* Decoding */

static inline uint16_t at(const uint16_t *buf, int len, int p)
{
    return buf[p % len];
}

/* Decode the GCR data field whose D5 AA AD ends just before @p */
static bool gcr_data(const uint16_t *buf, int len, int p, uint8_t *out)
{
    uint8_t raw[12 + SWIM_SECTOR + 1];
    uint8_t ca = 0, cb = 0, cc = 0;
    uint8_t e[4], va, vb, vc;

    p++;                            /* the sector number */
    for (int i = 0; i < 175; i++) {
        uint16_t sumb, suma;

        for (int j = 0; j < 4; j++) {
            if (i == 174 && j == 3) {
                e[j] = 0x96;
                continue;
            }
            e[j] = at(buf, len, p++);
            if (!gcr6_ok(e[j])) {
                return false;
            }
        }
        e[0] = gcr6_dec(e[0]);
        va = ((e[0] << 2) & 0xc0) | gcr6_dec(e[1]);
        vb = ((e[0] << 4) & 0xc0) | gcr6_dec(e[2]);
        vc = ((e[0] << 6) & 0xc0) | gcr6_dec(e[3]);

        cc = (cc << 1) | (cc >> 7);
        va ^= cc;
        suma = ca + va + (cc & 1);
        ca = suma;
        vb ^= ca;
        sumb = cb + vb + (suma >> 8);
        cb = sumb;
        vc ^= cb;

        raw[3 * i] = va;
        raw[3 * i + 1] = vb;
        if (i != 174) {
            cc = cc + vc + (sumb >> 8);
            raw[3 * i + 2] = vc;
        }
    }
    for (int j = 0; j < 4; j++) {
        e[j] = at(buf, len, p++);
        if (!gcr6_ok(e[j])) {
            return false;
        }
    }
    e[0] = gcr6_dec(e[0]);
    va = ((e[0] << 2) & 0xc0) | gcr6_dec(e[1]);
    vb = ((e[0] << 4) & 0xc0) | gcr6_dec(e[2]);
    vc = ((e[0] << 6) & 0xc0) | gcr6_dec(e[3]);
    if (va != ca || vb != cb || vc != cc ||
        at(buf, len, p) != 0xde || at(buf, len, p + 1) != 0xaa) {
        return false;
    }
    memcpy(out, raw + 12, SWIM_SECTOR);
    return true;
}

static int decode_gcr(const uint16_t *buf, int len, SwimSector *out, int max)
{
    int hpos[64];
    int nh = 0, n = 0;

    for (int p = 0; p < len && nh < ARRAY_SIZE(hpos); p++) {
        if (buf[p] == 0xd5 && at(buf, len, p + 1) == 0xaa &&
            at(buf, len, p + 2) == 0x96) {
            hpos[nh++] = p + 3;
        }
    }

    for (int i = 0; i < nh && n < max; i++) {
        int p = hpos[i];
        int end = i + 1 < nh ? hpos[i + 1] : hpos[0] + len;
        uint8_t h[7], v2, v3, tr, se;
        bool found = false;

        for (int j = 0; j < 7; j++) {
            h[j] = at(buf, len, p + j);
        }
        if (!gcr6_ok(h[0]) || !gcr6_ok(h[1]) || !gcr6_ok(h[2]) ||
            !gcr6_ok(h[3]) || !gcr6_ok(h[4]) ||
            h[5] != 0xde || h[6] != 0xaa) {
            continue;
        }
        v2 = gcr6_dec(h[2]);
        v3 = gcr6_dec(h[3]);
        tr = gcr6_dec(h[0]) | (v2 & 1 ? 0x40 : 0);
        se = gcr6_dec(h[1]);
        if (((tr ^ se ^ v2 ^ v3) & 0x3f) != gcr6_dec(h[4]) ||
            se >= SWIM_MAX_SPT) {
            continue;
        }
        out[n].cyl = tr;
        out[n].head = v2 & 0x20 ? 1 : 0;
        out[n].sector = se;
        out[n].size = 2;
        out[n].format = v3;

        /* The last good data field before the next header */
        for (int q = p + 7; q + 2 < end; q++) {
            if (at(buf, len, q) == 0xd5 && at(buf, len, q + 1) == 0xaa &&
                at(buf, len, q + 2) == 0xad &&
                gcr_data(buf, len, q + 3, out[n].data)) {
                found = true;
            }
        }
        if (found) {
            n++;
        }
    }
    return n;
}

static bool mfm_mark(const uint16_t *buf, int len, int p, uint8_t am)
{
    return at(buf, len, p) == (SWIM_MARK | 0xa1) &&
           at(buf, len, p + 1) == (SWIM_MARK | 0xa1) &&
           at(buf, len, p + 2) == (SWIM_MARK | 0xa1) &&
           at(buf, len, p + 3) == am;
}

/* CRC of @n bytes at @p, the address mark included; 0 when it is good */
static uint16_t mfm_crc(const uint16_t *buf, int len, int p, int n)
{
    uint16_t crc = SWIM_CRC_AFTER_MARKS;

    for (int i = 0; i < n; i++) {
        crc = swim_crc_byte(crc, at(buf, len, p + i));
    }
    return crc;
}

static int decode_mfm(const uint16_t *buf, int len, SwimSector *out, int max)
{
    int ipos[64];
    int ni = 0, n = 0;

    for (int p = 0; p < len && ni < ARRAY_SIZE(ipos); p++) {
        if (mfm_mark(buf, len, p, 0xfe) && !mfm_crc(buf, len, p + 3, 7)) {
            ipos[ni++] = p;
        }
    }

    for (int i = 0; i < ni && n < max; i++) {
        int p = ipos[i] + 4;
        int end = i + 1 < ni ? ipos[i + 1] : ipos[0] + len;
        uint8_t size = at(buf, len, p + 3);
        bool found = false;

        if (size != 2) {
            continue;
        }
        out[n].cyl = at(buf, len, p);
        out[n].head = at(buf, len, p + 1);
        out[n].sector = at(buf, len, p + 2);
        out[n].size = size;
        out[n].format = 0;
        for (int q = p + 6; q + 3 < end; q++) {
            if ((mfm_mark(buf, len, q, 0xfb) || mfm_mark(buf, len, q, 0xf8)) &&
                !mfm_crc(buf, len, q + 3, 1 + SWIM_SECTOR + 2)) {
                for (int j = 0; j < SWIM_SECTOR; j++) {
                    out[n].data[j] = at(buf, len, q + 4 + j);
                }
                found = true;
            }
        }
        if (found) {
            n++;
        }
    }
    return n;
}

int swim_track_decode(const uint16_t *buf, int len, SwimEnc enc,
                      SwimSector *out, int max)
{
    gcr6bw_init();
    if (len <= 0) {
        return 0;
    }
    switch (enc) {
    case SWIM_ENC_GCR:
        return decode_gcr(buf, len, out, max);
    case SWIM_ENC_MFM:
        return decode_mfm(buf, len, out, max);
    default:
        return 0;
    }
}
