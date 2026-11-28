/*
 * QTest for the SWIM2 floppy controller of the Quadra 800
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The machine starts without its ROM. The test drives the SWIM2 and its
 * SuperDrive through the registers as the .Sony driver does: it reads the
 * drive's sense lines, finds a sector header, reads the sector, writes it
 * back, formats a track in another layout and checks what reached the
 * image file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define VIA1            0x50000000
#define VIA_ORA         (VIA1 + (1 << 9))
#define VIA_DDRA        (VIA1 + (3 << 9))
#define VIA_HEADSEL     0x20

#define SWIM            0x5001e000
#define SWIM_W(r)       (SWIM + ((r) << 9))
#define SWIM_R(r)       (SWIM + (((r) | 8) << 9))

/* Registers */
#define R_DATA          0
#define R_MARK          1
#define R_CRC           2       /* write */
#define R_PHASES        4
#define R_SETUP         5
#define R_MODE0         6       /* write: clear bits */
#define R_MODE1         7       /* write: set bits */
#define R_HANDSHAKE     7       /* read */

#define MODE_CLFIFO     0x01
#define MODE_ENBL1      0x02
#define MODE_ACTION     0x08
#define MODE_WRITE      0x10
#define MODE_MOTON      0x80

#define HS_MARK         0x01
#define HS_CRC_ERROR    0x02
#define HS_SENSE        0x08
#define HS_DAT2BYTE     0x40
#define HS_DAT1BYTE     0x80

/* Drive registers: CA2-CA0, SEL in bit 3 */
#define DR_MOTOR        0x2
#define DR_NODISK       0x8
#define DR_WRITABLE     0x9
#define DR_TRACK0       0xa
#define DR_MFM          0xd
#define DR_DD           0xf

/* Drive commands */
#define DC_MOTOR_ON     0x2
#define DC_MFM          0x9
#define DC_GCR          0xd

#define BYTE_NS         16000   /* one HD byte; DD and GCR take longer */

static void swim_w(QTestState *qts, int reg, uint8_t v)
{
    qtest_writeb(qts, SWIM_W(reg), v);
}

static uint8_t swim_r(QTestState *qts, int reg)
{
    return qtest_readb(qts, SWIM_R(reg));
}

static void set_sel(QTestState *qts, bool sel)
{
    qtest_writeb(qts, VIA_DDRA, VIA_HEADSEL);
    qtest_writeb(qts, VIA_ORA, sel ? VIA_HEADSEL : 0);
}

static void drive_select(QTestState *qts)
{
    swim_w(qts, R_MODE0, 0xff);
    swim_w(qts, R_MODE1, MODE_MOTON | MODE_ENBL1);
}

static bool sense(QTestState *qts, int reg)
{
    set_sel(qts, reg & 8);
    swim_w(qts, R_PHASES, 0xf0 | (reg & 7));
    return swim_r(qts, R_HANDSHAKE) & HS_SENSE;
}

static void command(QTestState *qts, int reg)
{
    set_sel(qts, reg & 8);
    swim_w(qts, R_PHASES, 0xf0 | (reg & 7));
    swim_w(qts, R_PHASES, 0xf8 | (reg & 7));
    swim_w(qts, R_PHASES, 0xf0 | (reg & 7));
}

static void read_start(QTestState *qts, bool gcr)
{
    swim_w(qts, R_MODE0, MODE_ACTION | MODE_WRITE);
    swim_w(qts, R_SETUP, gcr ? 0x04 : 0x00);
    swim_w(qts, R_MODE1, MODE_CLFIFO);
    swim_w(qts, R_MODE0, MODE_CLFIFO);
    swim_w(qts, R_MODE1, MODE_ACTION);
}

static void xfer_stop(QTestState *qts)
{
    swim_w(qts, R_MODE0, MODE_ACTION | MODE_WRITE);
}

/* The next byte, with the handshake seen just before it */
static uint8_t read_byte(QTestState *qts, uint8_t *hs)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t h = swim_r(qts, R_HANDSHAKE);

        if (h & HS_DAT1BYTE) {
            if (hs) {
                *hs = h;
            }
            return swim_r(qts, R_MARK);
        }
        qtest_clock_step(qts, BYTE_NS);
    }
    g_assert_not_reached();
}

/* Wait in read mode for A1 A1 A1 @am; true when found within a turn */
static bool find_mark(QTestState *qts, uint8_t am)
{
    int marks = 0;

    for (int i = 0; i < 20000; i++) {
        uint8_t hs, b = read_byte(qts, &hs);

        if ((hs & HS_MARK) && b == 0xa1) {
            marks++;
        } else if (marks >= 3 && b == am) {
            return true;
        } else {
            marks = 0;
        }
    }
    return false;
}

/* Read the header of the next sector: C H R N, with a good CRC */
static void read_header(QTestState *qts, uint8_t *chrn)
{
    uint8_t hs;

    g_assert_true(find_mark(qts, 0xfe));
    for (int i = 0; i < 4; i++) {
        chrn[i] = read_byte(qts, NULL);
    }
    read_byte(qts, NULL);
    read_byte(qts, &hs);
    /* The handshake before the last CRC byte says the CRC is zero */
    g_assert_cmphex(hs & HS_CRC_ERROR, ==, 0);
}

static void write_byte(QTestState *qts, int reg, uint8_t v)
{
    for (int i = 0; i < 100000; i++) {
        if (swim_r(qts, R_HANDSHAKE) & HS_DAT1BYTE) {
            swim_w(qts, reg, v);
            return;
        }
        qtest_clock_step(qts, BYTE_NS);
    }
    g_assert_not_reached();
}

static void write_start(QTestState *qts)
{
    swim_w(qts, R_MODE0, MODE_ACTION);
    swim_w(qts, R_SETUP, 0x00);
    swim_w(qts, R_MODE1, MODE_WRITE | MODE_CLFIFO);
    swim_w(qts, R_MODE0, MODE_CLFIFO);
}

/* Wait until the FIFO has drained, then end the write */
static void write_end(QTestState *qts)
{
    for (int i = 0; i < 100; i++) {
        if ((swim_r(qts, R_HANDSHAKE) & (HS_DAT1BYTE | HS_DAT2BYTE)) ==
            (HS_DAT1BYTE | HS_DAT2BYTE)) {
            break;
        }
        qtest_clock_step(qts, BYTE_NS);
    }
    qtest_clock_step(qts, 4 * BYTE_NS);
    xfer_stop(qts);
}

/* An MFM data field: preamble, marks, @am, 512 bytes and the CRC */
static void write_data_field(QTestState *qts, uint8_t am, const uint8_t *d,
                             int n, bool action_first)
{
    write_byte(qts, R_DATA, 0x00);
    write_byte(qts, R_DATA, 0x00);
    if (action_first) {
        swim_w(qts, R_MODE1, MODE_ACTION);
    }
    for (int i = 0; i < 10; i++) {
        write_byte(qts, R_DATA, 0x00);
    }
    for (int i = 0; i < 3; i++) {
        write_byte(qts, R_MARK, 0xa1);
    }
    write_byte(qts, R_DATA, am);
    for (int i = 0; i < n; i++) {
        write_byte(qts, R_DATA, d[i]);
    }
    write_byte(qts, R_CRC, 0);
}

static char *make_image(size_t size, uint8_t seed)
{
    g_autoptr(GError) err = NULL;
    g_autofree uint8_t *buf = g_malloc(size);
    char *path;
    int fd;

    fd = g_file_open_tmp("q800-swim-XXXXXX.img", &path, &err);
    g_assert_no_error(err);
    for (size_t i = 0; i < size; i++) {
        buf[i] = (i * 7 + i / 512 + seed) & 0xff;
    }
    g_assert_cmpint(write(fd, buf, size), ==, size);
    close(fd);
    return path;
}

static QTestState *start(const char *img, bool ro)
{
    return qtest_initf("-M q800 -drive if=floppy,format=raw,file=%s%s",
                       img, ro ? ",readonly=on" : "");
}

static void spin_up(QTestState *qts, bool mfm)
{
    drive_select(qts);
    command(qts, DC_MOTOR_ON);
    command(qts, mfm ? DC_MFM : DC_GCR);
    qtest_clock_step(qts, 1000000);
}

/* Sense lines with an HD disk, a DD disk, and none */
static void test_sense(void)
{
    g_autofree char *hd = make_image(1474560, 1);
    g_autofree char *dd = make_image(819200, 2);
    QTestState *qts;

    qts = start(hd, false);
    drive_select(qts);
    g_assert_false(sense(qts, DR_NODISK));
    g_assert_true(sense(qts, DR_WRITABLE));
    g_assert_false(sense(qts, DR_TRACK0));
    g_assert_false(sense(qts, DR_DD));
    g_assert_true(sense(qts, DR_MOTOR));        /* off */
    command(qts, DC_MOTOR_ON);
    g_assert_false(sense(qts, DR_MOTOR));
    command(qts, DC_GCR);
    g_assert_false(sense(qts, DR_MFM));
    command(qts, DC_MFM);
    g_assert_true(sense(qts, DR_MFM));
    qtest_quit(qts);

    qts = start(dd, true);
    drive_select(qts);
    g_assert_true(sense(qts, DR_DD));
    g_assert_false(sense(qts, DR_WRITABLE));
    qtest_quit(qts);

    qts = qtest_init("-M q800");
    drive_select(qts);
    g_assert_true(sense(qts, DR_NODISK));
    swim_w(qts, R_MODE0, MODE_MOTON);
    /* No drive selected: the sense line is pulled up */
    g_assert_true(swim_r(qts, R_HANDSHAKE) & HS_SENSE);
    qtest_quit(qts);

    unlink(hd);
    unlink(dd);
}

/* Read sector 1 of track 0 of a 1.44M disk, then write it */
static void test_mfm_rw(void)
{
    g_autofree char *img = make_image(1474560, 3);
    g_autofree uint8_t *file = NULL;
    uint8_t chrn[4], data[512], fresh[512];
    QTestState *qts = start(img, false);
    gsize len;

    spin_up(qts, true);
    set_sel(qts, false);

    /* Find sector 1 */
    read_start(qts, false);
    for (int i = 0; i < 40; i++) {
        read_header(qts, chrn);
        if (chrn[2] == 1) {
            break;
        }
    }
    g_assert_cmpint(chrn[0], ==, 0);
    g_assert_cmpint(chrn[1], ==, 0);
    g_assert_cmpint(chrn[2], ==, 1);
    g_assert_cmpint(chrn[3], ==, 2);

    /* Its data field */
    g_assert_true(find_mark(qts, 0xfb));
    for (int i = 0; i < 512; i++) {
        data[i] = read_byte(qts, NULL);
        g_assert_cmpint(data[i], ==, (i * 7 + 3) & 0xff);
    }
    xfer_stop(qts);

    /* Write sector 2 the way the driver does: header, then data field */
    read_start(qts, false);
    do {
        read_header(qts, chrn);
    } while (chrn[2] != 2);
    for (int i = 0; i < 512; i++) {
        fresh[i] = 0xff - i;
    }
    write_start(qts);
    write_data_field(qts, 0xfb, fresh, 512, true);
    write_end(qts);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(img, (char **)&file, &len, NULL));
    g_assert_cmpint(len, ==, 1474560);
    g_assert_cmpmem(file + 512, 512, fresh, 512);
    /* Its neighbours are as they were */
    for (int i = 0; i < 512; i++) {
        g_assert_cmpint(file[i], ==, (i * 7 + 3) & 0xff);
        g_assert_cmpint(file[1024 + i], ==, ((1024 + i) * 7 + 2 + 3) & 0xff);
    }
    unlink(img);
}

/* GCR: the address field of a sector of an 800K disk */
static void test_gcr_read(void)
{
    g_autofree char *img = make_image(819200, 4);
    QTestState *qts = start(img, true);
    uint32_t state = 0;
    bool found = false;

    spin_up(qts, false);
    set_sel(qts, false);
    read_start(qts, true);
    for (int i = 0; i < 20000 && !found; i++) {
        state = ((state << 8) | read_byte(qts, NULL)) & 0xffffff;
        if (state == 0xd5aa96) {
            uint8_t track = read_byte(qts, NULL);
            uint8_t sector = read_byte(qts, NULL);
            uint8_t side = read_byte(qts, NULL);
            uint8_t format = read_byte(qts, NULL);

            g_assert_cmphex(track, ==, 0x96);       /* track 0 */
            g_assert_cmphex(side, ==, 0x96);        /* side 0 */
            g_assert_cmphex(format, ==, 0xd9);      /* 0x22: two sides */
            g_assert_cmpint(sector, >=, 0x96);
            found = true;
        }
    }
    g_assert_true(found);
    qtest_quit(qts);
    unlink(img);
}

/* Format track 0 of an 800K GCR disk as 720K MFM: the image shrinks */
static void test_mfm_format_resizes(void)
{
    g_autofree char *img = make_image(819200, 5);
    g_autofree uint8_t *file = NULL;
    QTestState *qts = start(img, false);
    uint8_t data[512];
    gsize len;

    spin_up(qts, true);
    set_sel(qts, false);
    memset(data, 0xe5, sizeof(data));

    write_start(qts);
    write_byte(qts, R_DATA, 0x4e);
    write_byte(qts, R_DATA, 0x4e);
    swim_w(qts, R_MODE1, MODE_ACTION);
    for (int i = 0; i < 78; i++) {
        write_byte(qts, R_DATA, 0x4e);
    }
    for (int i = 0; i < 12; i++) {
        write_byte(qts, R_DATA, 0x00);
    }
    for (int i = 0; i < 3; i++) {
        write_byte(qts, R_MARK, 0xc2);
    }
    write_byte(qts, R_DATA, 0xfc);
    for (int i = 0; i < 50; i++) {
        write_byte(qts, R_DATA, 0x4e);
    }
    for (int s = 1; s <= 9; s++) {
        uint8_t id[4] = { 0, 0, s, 2 };

        data[0] = s;
        for (int i = 0; i < 12; i++) {
            write_byte(qts, R_DATA, 0x00);
        }
        for (int i = 0; i < 3; i++) {
            write_byte(qts, R_MARK, 0xa1);
        }
        write_byte(qts, R_DATA, 0xfe);
        for (int i = 0; i < 4; i++) {
            write_byte(qts, R_DATA, id[i]);
        }
        write_byte(qts, R_CRC, 0);
        for (int i = 0; i < 22; i++) {
            write_byte(qts, R_DATA, 0x4e);
        }
        write_data_field(qts, 0xfb, data, 512, false);
        for (int i = 0; i < 80; i++) {
            write_byte(qts, R_DATA, 0x4e);
        }
    }
    write_end(qts);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(img, (char **)&file, &len, NULL));
    g_assert_cmpint(len, ==, 737280);
    for (int s = 0; s < 9; s++) {
        g_assert_cmpint(file[s * 512], ==, s + 1);
        g_assert_cmpint(file[s * 512 + 1], ==, 0xe5);
        g_assert_cmpint(file[s * 512 + 511], ==, 0xe5);
    }
    unlink(img);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/q800-swim/sense", test_sense);
    qtest_add_func("/q800-swim/mfm-read-write", test_mfm_rw);
    qtest_add_func("/q800-swim/gcr-read", test_gcr_read);
    qtest_add_func("/q800-swim/mfm-format-resizes", test_mfm_format_resizes);

    return g_test_run();
}
