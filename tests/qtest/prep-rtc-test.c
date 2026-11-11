/*
 * QTest: the 40p clock keeps the year it was given
 *
 * The 40p's MC146818 has no century byte: the year register holds the
 * years since base_year (2000, or 1900 for OS/2).  -rtc base=1995 must
 * show 1995 (0x95 with base year 2000), and 2026 must stay 2026 with
 * base year 1900 (0xc6), also after the guest holds the clock with SET
 * and lets it go, as the 40p firmware does at every start.  Years the
 * guest writes must read back the same.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "hw/rtc/mc146818rtc_regs.h"

#define ISA_IO  0x80000000ULL           /* the 40p's ISA I/O ports */

typedef struct {
    const char *path;
    const char *base;                   /* -rtc base= */
    int base_year;                      /* 0: the machine's own, 2000 */
    uint8_t reg;                        /* what the year register holds */
    int year;
} StartCase;

static const StartCase start_cases[] = {
    { "/prep/rtc/start/1995", "1995-07-08T12:00:00", 0, 0x95, 1995 },
    { "/prep/rtc/start/1970", "1970-01-02T12:00:00", 0, 0x70, 1970 },
    { "/prep/rtc/start/2026", "2026-11-11T12:00:00", 0, 0x26, 2026 },
    { "/prep/rtc/start/2069", "2069-12-30T12:00:00", 0, 0x69, 2069 },
    { "/prep/rtc/start/1995-base-1900", "1995-07-08T12:00:00", 1900,
      0x95, 1995 },
    { "/prep/rtc/start/2026-base-1900", "2026-11-11T12:00:00", 1900,
      0xc6, 2026 },
};

static QTestState *start(const char *base, int base_year)
{
    g_autofree char *global = base_year ?
        g_strdup_printf("-global mc146818rtc.base_year=%d", base_year) :
        g_strdup("");

    return qtest_initf("-machine 40p -nodefaults -rtc base=%s,clock=vm %s",
                       base, global);
}

static uint8_t cmos_read(QTestState *qts, uint8_t reg)
{
    qtest_writeb(qts, ISA_IO + 0x70, reg);
    return qtest_readb(qts, ISA_IO + 0x71);
}

static void cmos_write(QTestState *qts, uint8_t reg, uint8_t val)
{
    qtest_writeb(qts, ISA_IO + 0x70, reg);
    qtest_writeb(qts, ISA_IO + 0x71, val);
}

/* The year the clock keeps, as the machine's rtc-time property says */
static int clock_year(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments':"
                           " { 'path': '/machine', 'property': 'rtc-time' } }");
    QDict *tm = qdict_get_qdict(rsp, "return");
    int year;

    g_assert(tm);
    year = qdict_get_int(tm, "tm_year") + 1900;
    qobject_unref(rsp);
    return year;
}

/* Hold the clock with SET and let it go, writing nothing, as the firmware */
static void set_and_clear(QTestState *qts)
{
    uint8_t b = cmos_read(qts, RTC_REG_B);

    cmos_write(qts, RTC_REG_B, b | REG_B_SET);
    cmos_write(qts, RTC_REG_B, b & ~REG_B_SET);
}

static void test_start(const void *opaque)
{
    const StartCase *c = opaque;
    QTestState *qts = start(c->base, c->base_year);
    int i;

    g_assert_cmphex(cmos_read(qts, RTC_YEAR), ==, c->reg);
    g_assert_cmpint(clock_year(qts), ==, c->year);
    for (i = 0; i < 3; i++) {
        set_and_clear(qts);
        g_assert_cmphex(cmos_read(qts, RTC_YEAR), ==, c->reg);
        g_assert_cmpint(clock_year(qts), ==, c->year);
    }
    qtest_quit(qts);
}

/* The guest sets 15 June, 12:00:00 of the year @reg, in binary if @binary */
static void guest_set_year(QTestState *qts, uint8_t reg, bool binary)
{
    uint8_t b = REG_B_24H | (binary ? REG_B_DM : 0);

    cmos_write(qts, RTC_REG_B, b | REG_B_SET);
    cmos_write(qts, RTC_SECONDS, 0);
    cmos_write(qts, RTC_MINUTES, 0);
    cmos_write(qts, RTC_HOURS, binary ? 12 : 0x12);
    cmos_write(qts, RTC_DAY_OF_MONTH, binary ? 15 : 0x15);
    cmos_write(qts, RTC_MONTH, binary ? 6 : 0x06);
    cmos_write(qts, RTC_YEAR, reg);
    cmos_write(qts, RTC_REG_B, b);
}

static void test_guest_sets(const void *opaque)
{
    static const struct {
        int base_year;
        uint8_t reg;
        bool binary;
        int year;
    } sets[] = {
        { 0, 0x96, false, 1996 },
        { 0, 0x70, false, 1970 },
        { 0, 0x00, false, 2000 },
        { 0, 0x26, false, 2026 },
        { 0, 95, true, 1995 },
        { 0, 26, true, 2026 },
        { 1900, 0x96, false, 1996 },
        { 1900, 0xc5, false, 2025 },
        { 1900, 0xf9, false, 2059 },
        { 1900, 125, true, 2025 },
    };
    int i;

    for (i = 0; i < ARRAY_SIZE(sets); i++) {
        QTestState *qts = start("2026-11-11T12:00:00", sets[i].base_year);

        guest_set_year(qts, sets[i].reg, sets[i].binary);
        g_assert_cmpint(clock_year(qts), ==, sets[i].year);
        g_assert_cmphex(cmos_read(qts, RTC_YEAR), ==, sets[i].reg);
        set_and_clear(qts);
        g_assert_cmphex(cmos_read(qts, RTC_YEAR), ==, sets[i].reg);
        g_assert_cmpint(clock_year(qts), ==, sets[i].year);
        qtest_quit(qts);
    }
}

int main(int argc, char **argv)
{
    int i;

    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_machine("40p")) {
        g_test_skip("No 40p machine");
        return g_test_run();
    }
    for (i = 0; i < ARRAY_SIZE(start_cases); i++) {
        qtest_add_data_func(start_cases[i].path, &start_cases[i], test_start);
    }
    qtest_add_data_func("/prep/rtc/guest-sets", NULL, test_guest_sets);
    return g_test_run();
}
