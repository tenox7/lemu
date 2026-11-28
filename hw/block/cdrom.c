/*
 * QEMU ATAPI CD-ROM Emulator
 *
 * Copyright (c) 2006 Fabrice Bellard
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

/* ??? Most of the ATAPI emulation is still in ide.c.  It should be moved
   here.  */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/scsi/scsi.h"

static void lba_to_msf(uint8_t *buf, int lba)
{
    lba += 150;
    buf[0] = (lba / 75) / 60;
    buf[1] = (lba / 75) % 60;
    buf[2] = lba % 75;
}

/* same toc as bochs. Return -1 if error or the toc length */
/* XXX: check this */
int cdrom_read_toc(int nb_sectors, uint8_t *buf, int msf, int start_track)
{
    uint8_t *q;
    int len;

    if (start_track > 1 && start_track != 0xaa)
        return -1;
    q = buf + 2;
    *q++ = 1; /* first session */
    *q++ = 1; /* last session */
    if (start_track <= 1) {
        *q++ = 0; /* reserved */
        *q++ = 0x14; /* ADR, control */
        *q++ = 1;    /* track number */
        *q++ = 0; /* reserved */
        if (msf) {
            *q++ = 0; /* reserved */
            lba_to_msf(q, 0);
            q += 3;
        } else {
            /* sector 0 */
            stl_be_p(q, 0);
            q += 4;
        }
    }
    /* lead out track */
    *q++ = 0; /* reserved */
    *q++ = 0x16; /* ADR, control */
    *q++ = 0xaa; /* track number */
    *q++ = 0; /* reserved */
    if (msf) {
        *q++ = 0; /* reserved */
        lba_to_msf(q, nb_sectors);
        q += 3;
    } else {
        stl_be_p(q, nb_sectors);
        q += 4;
    }
    len = q - buf;
    stw_be_p(buf, len - 2);
    return len;
}

/* mostly same info as PearPc */
int cdrom_read_toc_raw(int nb_sectors, uint8_t *buf, int msf, int session_num)
{
    uint8_t *q;
    int len;

    q = buf + 2;
    *q++ = 1; /* first session */
    *q++ = 1; /* last session */

    *q++ = 1; /* session number */
    *q++ = 0x14; /* data track */
    *q++ = 0; /* track number */
    *q++ = 0xa0; /* lead-in */
    *q++ = 0; /* min */
    *q++ = 0; /* sec */
    *q++ = 0; /* frame */
    *q++ = 0;
    *q++ = 1; /* first track */
    *q++ = 0x00; /* disk type */
    *q++ = 0x00;

    *q++ = 1; /* session number */
    *q++ = 0x14; /* data track */
    *q++ = 0; /* track number */
    *q++ = 0xa1;
    *q++ = 0; /* min */
    *q++ = 0; /* sec */
    *q++ = 0; /* frame */
    *q++ = 0;
    *q++ = 1; /* last track */
    *q++ = 0x00;
    *q++ = 0x00;

    *q++ = 1; /* session number */
    *q++ = 0x14; /* data track */
    *q++ = 0; /* track number */
    *q++ = 0xa2; /* lead-out */
    *q++ = 0; /* min */
    *q++ = 0; /* sec */
    *q++ = 0; /* frame */
    if (msf) {
        *q++ = 0; /* reserved */
        lba_to_msf(q, nb_sectors);
        q += 3;
    } else {
        stl_be_p(q, nb_sectors);
        q += 4;
    }

    *q++ = 1; /* session number */
    *q++ = 0x14; /* ADR, control */
    *q++ = 0;    /* track number */
    *q++ = 1;    /* point */
    *q++ = 0; /* min */
    *q++ = 0; /* sec */
    *q++ = 0; /* frame */
    if (msf) {
        *q++ = 0;
        lba_to_msf(q, 0);
        q += 3;
    } else {
        *q++ = 0;
        *q++ = 0;
        *q++ = 0;
        *q++ = 0;
    }

    len = q - buf;
    stw_be_p(buf, len - 2);
    return len;
}

/*
 * Discs with a real track list (block/cdrom-image.h): TOC, session and
 * full TOC replies, and READ CD sectors made from the raw sectors.
 */

static uint8_t to_bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static void cdrom_put_address(uint8_t *q, int64_t lba, int msf)
{
    if (msf) {
        q[0] = 0;
        lba_to_msf(q + 1, lba);
    } else {
        stl_be_p(q, lba);
    }
}

static uint8_t cdrom_track_adr_control(const BlockCdTrack *t)
{
    return 0x10 | (t->control & 0x0f);
}

int cdrom_read_toc_tracks(const BlockCdToc *toc, uint8_t *buf, int msf,
                          int start_track)
{
    const BlockCdTrack *last = &toc->tracks[toc->n_tracks - 1];
    uint8_t *q = buf + 4;
    int len;

    if (start_track > last->number && start_track != 0xaa) {
        return -1;
    }

    buf[2] = toc->tracks[0].number;
    buf[3] = last->number;

    for (int i = 0; i < toc->n_tracks; i++) {
        const BlockCdTrack *t = &toc->tracks[i];

        if (start_track == 0xaa || t->number < start_track) {
            continue;
        }
        *q++ = 0;
        *q++ = cdrom_track_adr_control(t);
        *q++ = t->number;
        *q++ = 0;
        cdrom_put_address(q, t->start, msf);
        q += 4;
    }

    /* lead-out */
    *q++ = 0;
    *q++ = 0x10 | (last->control & 0x0f);
    *q++ = 0xaa;
    *q++ = 0;
    cdrom_put_address(q, toc->leadout, msf);
    q += 4;

    len = q - buf;
    stw_be_p(buf, len - 2);
    return len;
}

int cdrom_read_session_info(const BlockCdToc *toc, uint8_t *buf, int msf)
{
    const BlockCdTrack *first = NULL;

    /* first track of the last session */
    for (int i = 0; i < toc->n_tracks; i++) {
        if (toc->tracks[i].session == toc->last_session) {
            first = &toc->tracks[i];
            break;
        }
    }
    if (!first) {
        first = &toc->tracks[0];
    }

    memset(buf, 0, 12);
    stw_be_p(buf, 10);
    buf[2] = toc->first_session;
    buf[3] = toc->last_session;
    buf[5] = cdrom_track_adr_control(first);
    buf[6] = first->number;
    cdrom_put_address(buf + 8, first->start, msf);
    return 12;
}

/*
 * Adds an 11-byte descriptor of a full TOC at *len, if it fits in @size
 * bytes. *len counts it even when it does not fit, so the TOC length is
 * the one of the whole TOC, and the guest gets what its buffer holds.
 */
static void cdrom_put_raw_descriptor(uint8_t *buf, int size, int *len,
                                     uint8_t session, uint8_t adr_control,
                                     uint8_t point, uint8_t pmin, uint8_t psec,
                                     uint8_t pframe)
{
    if (*len + 11 <= size) {
        uint8_t *q = buf + *len;

        q[0] = session;
        q[1] = adr_control;
        q[2] = 0;     /* TNO */
        q[3] = point;
        q[4] = 0;     /* ATIME */
        q[5] = 0;
        q[6] = 0;
        q[7] = 0;     /* zero */
        q[8] = pmin;
        q[9] = psec;
        q[10] = pframe;
    }
    *len += 11;
}

int cdrom_read_toc_raw_tracks(const BlockCdToc *toc, uint8_t *buf, int size)
{
    int len = 4;

    assert(size >= 4);
    buf[2] = toc->first_session;
    buf[3] = toc->last_session;

    for (int session = toc->first_session; session <= toc->last_session;
         session++) {
        const BlockCdTrack *first = NULL, *last = NULL;
        uint8_t msf[3];

        for (int i = 0; i < toc->n_tracks; i++) {
            if (toc->tracks[i].session == session) {
                if (!first) {
                    first = &toc->tracks[i];
                }
                last = &toc->tracks[i];
            }
        }
        if (!first) {
            continue;
        }

        cdrom_put_raw_descriptor(buf, size, &len, session,
                                 cdrom_track_adr_control(first),
                                 0xa0, first->number, toc->disc_type, 0);
        cdrom_put_raw_descriptor(buf, size, &len, session,
                                 cdrom_track_adr_control(last),
                                 0xa1, last->number, 0, 0);
        lba_to_msf(msf, session == toc->last_session ? toc->leadout
                                                     : last->end + 1);
        cdrom_put_raw_descriptor(buf, size, &len, session,
                                 cdrom_track_adr_control(last),
                                 0xa2, msf[0], msf[1], msf[2]);

        for (const BlockCdTrack *t = first; t <= last; t++) {
            if (t->session != session) {
                continue;
            }
            lba_to_msf(msf, t->start);
            cdrom_put_raw_descriptor(buf, size, &len, session,
                                     cdrom_track_adr_control(t),
                                     t->number, msf[0], msf[1], msf[2]);
        }
    }

    stw_be_p(buf, len - 2);
    return MIN(len, size);
}

/* Layout of the user data and its neighbours in a raw sector of a track */
typedef struct CdSectorLayout {
    int header;       /* 4 bytes at 12, 0 if none */
    int subheader;    /* 8 bytes at 16, 0 if none */
    int user_offset;
    int user_size;
    int edc_size;     /* EDC/ECC after the user data */
} CdSectorLayout;

static void cdrom_sector_layout(const BlockCdTrack *t, CdSectorLayout *l)
{
    switch (t->mode) {
    case BLOCK_CD_TRACK_AUDIO:
        *l = (CdSectorLayout) { 0, 0, 0, 2352, 0 };
        break;
    case BLOCK_CD_TRACK_MODE1:
        *l = (CdSectorLayout) { 4, 0, 16, 2048, 288 };
        break;
    case BLOCK_CD_TRACK_MODE2_FORM1:
        *l = (CdSectorLayout) { 4, 8, 24, 2048, 280 };
        break;
    case BLOCK_CD_TRACK_MODE2_FORM2:
        *l = (CdSectorLayout) { 4, 8, 24, 2324, 4 };
        break;
    default: /* mode 2 without forms */
        *l = (CdSectorLayout) { 4, 0, 16, 2336, 0 };
        break;
    }
}

bool cdrom_read_cd_type_matches(const BlockCdTrack *t, int expected)
{
    switch (expected) {
    case 0: /* any */
        return true;
    case 1: /* CD-DA */
        return t->mode == BLOCK_CD_TRACK_AUDIO;
    case 2: /* mode 1 */
        return t->mode == BLOCK_CD_TRACK_MODE1;
    case 3: /* mode 2 formless */
        return t->mode == BLOCK_CD_TRACK_MODE2;
    case 4: /* mode 2 form 1 */
        return t->mode == BLOCK_CD_TRACK_MODE2_FORM1;
    case 5: /* mode 2 form 2 */
        return t->mode == BLOCK_CD_TRACK_MODE2_FORM2;
    default:
        return false;
    }
}

static int cdrom_read_cd_sub_size(uint8_t sub)
{
    switch (sub) {
    case 0:
        return 0;
    case 1: /* raw P-W */
        return 96;
    case 2: /* formatted Q */
        return 16;
    default:
        return -1;
    }
}

int cdrom_read_cd_sector_size(const BlockCdTrack *t, uint8_t fields,
                              uint8_t sub)
{
    CdSectorLayout l;
    int size = 0, sub_size = cdrom_read_cd_sub_size(sub);

    if (sub_size < 0) {
        return -1;
    }

    cdrom_sector_layout(t, &l);

    if (t->mode == BLOCK_CD_TRACK_AUDIO) {
        size = (fields & 0x10) ? 2352 : 0;
    } else {
        if (fields & 0x80) {
            size += 12;
        }
        if (fields & 0x20) {
            size += l.header;
        }
        if (fields & 0x40) {
            size += l.subheader;
        }
        if (fields & 0x10) {
            size += l.user_size;
        }
        if (fields & 0x08) {
            size += l.edc_size;
        }
    }

    switch ((fields >> 1) & 3) {
    case 1: /* C2 error pointers */
        size += 294;
        break;
    case 2: /* block error byte and C2 error pointers */
        size += 296;
        break;
    case 3:
        return -1;
    }

    return size + sub_size;
}

static uint16_t cdrom_q_crc(const uint8_t *q)
{
    uint16_t crc = 0;

    for (int i = 0; i < 10; i++) {
        crc ^= q[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return ~crc;
}

/* The 12 bytes of Q subchannel the disc has at @lba */
static void cdrom_make_q(const BlockCdToc *toc, int64_t lba, uint8_t *q)
{
    const BlockCdTrack *t = block_cd_toc_find(toc, lba);
    int64_t relative;
    uint8_t msf[3];
    int index = 1;

    if (!t) {
        /* in a gap before a track: the next one, counting down */
        t = &toc->tracks[toc->n_tracks - 1];
        for (int i = 0; i < toc->n_tracks; i++) {
            if (toc->tracks[i].start > lba) {
                t = &toc->tracks[i];
                break;
            }
        }
    }

    relative = lba - t->start;
    if (relative < 0) {
        index = 0;
        relative = -relative;
    }

    q[0] = cdrom_track_adr_control(t);
    q[1] = to_bcd(t->number);
    q[2] = to_bcd(index);
    lba_to_msf(msf, relative - 150);
    q[3] = to_bcd(msf[0]);
    q[4] = to_bcd(msf[1]);
    q[5] = to_bcd(msf[2]);
    q[6] = 0;
    lba_to_msf(msf, lba);
    q[7] = to_bcd(msf[0]);
    q[8] = to_bcd(msf[1]);
    q[9] = to_bcd(msf[2]);
    stw_be_p(q + 10, cdrom_q_crc(q));
}

void cdrom_read_cd_format(uint8_t *out, const uint8_t *raw,
                          const uint8_t *subchannel, const BlockCdToc *toc,
                          const BlockCdTrack *t, int64_t lba, uint8_t fields,
                          uint8_t sub)
{
    CdSectorLayout l;
    uint8_t q[12];

    cdrom_sector_layout(t, &l);

    if (t->mode == BLOCK_CD_TRACK_AUDIO) {
        if (fields & 0x10) {
            memcpy(out, raw, 2352);
            out += 2352;
        }
    } else {
        if (fields & 0x80) {
            memcpy(out, raw, 12);
            out += 12;
        }
        if ((fields & 0x20) && l.header) {
            memcpy(out, raw + 12, 4);
            out += 4;
        }
        if ((fields & 0x40) && l.subheader) {
            memcpy(out, raw + 16, 8);
            out += 8;
        }
        if (fields & 0x10) {
            memcpy(out, raw + l.user_offset, l.user_size);
            out += l.user_size;
        }
        if ((fields & 0x08) && l.edc_size) {
            memcpy(out, raw + l.user_offset + l.user_size, l.edc_size);
            out += l.edc_size;
        }
    }

    /* the sectors were read without errors */
    switch ((fields >> 1) & 3) {
    case 1:
        memset(out, 0, 294);
        out += 294;
        break;
    case 2:
        memset(out, 0, 296);
        out += 296;
        break;
    }

    if (sub == 0) {
        return;
    }

    if (subchannel) {
        if (sub == 1) {
            memcpy(out, subchannel, 96);
            return;
        }
        /* Q is bit 6 of each raw subchannel byte */
        memset(q, 0, sizeof(q));
        for (int i = 0; i < 96; i++) {
            q[i / 8] |= ((subchannel[i] >> 6) & 1) << (7 - i % 8);
        }
    } else {
        cdrom_make_q(toc, lba, q);
    }

    if (sub == 2) {
        memcpy(out, q, 12);
        memset(out + 12, 0, 4);
    } else {
        /* raw P-W with only Q: P set in the pre-gap */
        bool pause = (q[2] == 0);

        for (int i = 0; i < 96; i++) {
            out[i] = (pause ? 0x80 : 0) |
                     (((q[i / 8] >> (7 - i % 8)) & 1) << 6);
        }
    }
}
