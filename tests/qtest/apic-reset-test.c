/*
 * QTest: after a reset, CPUID shows the local APIC again
 *
 * A guest that turns its local APIC off through IA32_APIC_BASE also
 * hides it in CPUID.  A reset turns the APIC on again, so CPUID must
 * show it again: firmware that finds no APIC there leaves LINT0 masked,
 * and the 8259's interrupts never reach the CPU.  Windows NT 4.0 hung
 * in its boot menu after every reboot that way.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

/* At the reset vector: turn the APIC off through IA32_APIC_BASE, halt */
static const uint8_t apic_off[16] = {
    0x66, 0xb9, 0x1b, 0x00, 0x00, 0x00,     /* mov ecx, 0x1b */
    0x0f, 0x32,                             /* rdmsr */
    0x80, 0xe4, 0xf7,                       /* and ah, 0xf7: clear bit 11 */
    0x0f, 0x30,                             /* wrmsr */
    0xf4,                                   /* hlt */
    0xeb, 0xfd,                             /* jmp back to the hlt */
};

static char *bios_path;

static QTestState *start(const char *cpu)
{
    return qtest_initf("-accel tcg -machine pc -nodefaults -bios %s %s",
                       bios_path, cpu);
}

static char *cpu0_path(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'query-cpus-fast'}");
    QDict *cpu = qobject_to(QDict,
                            qlist_peek(qdict_get_qlist(rsp, "return")));
    char *path = g_strdup(qdict_get_str(cpu, "qom-path"));

    qobject_unref(rsp);
    return path;
}

static bool cpuid_apic(QTestState *qts, const char *path)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments':"
                           " {'path': %s, 'property': 'apic'}}", path);
    bool on = qdict_get_bool(rsp, "return");

    qobject_unref(rsp);
    return on;
}

/* Stopped, so the guest can't turn the APIC off again after the reset */
static void reset_stopped(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(qts, "RESET");
}

static void test_apic_back(void)
{
    QTestState *qts = start("-cpu pentium2");
    g_autofree char *path = cpu0_path(qts);
    int i;

    /* the guest turns its APIC off, which hides it in CPUID */
    for (i = 0; i < 1000 && cpuid_apic(qts, path); i++) {
        g_usleep(10 * 1000);
    }
    g_assert_false(cpuid_apic(qts, path));

    reset_stopped(qts);
    g_assert_true(cpuid_apic(qts, path));
    qtest_quit(qts);
}

static void test_no_apic_stays_off(void)
{
    /* with two CPUs QEMU makes an APIC even for a model without one */
    QTestState *qts = start("-cpu pentium2,apic=off -smp 2");
    g_autofree char *path = cpu0_path(qts);

    g_assert_false(cpuid_apic(qts, path));
    reset_stopped(qts);
    g_assert_false(cpuid_apic(qts, path));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_autofree uint8_t *rom = g_malloc0(64 * KiB);
    int fd, ret;

    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_accel("tcg")) {
        return g_test_run();
    }

    /* a 64 KiB BIOS ends at 4 GiB, so its last 16 bytes are the reset vector */
    memcpy(rom + 64 * KiB - sizeof(apic_off), apic_off, sizeof(apic_off));
    fd = g_file_open_tmp("apic-reset-XXXXXX.bin", &bios_path, NULL);
    g_assert(fd >= 0);
    g_assert(write(fd, rom, 64 * KiB) == 64 * KiB);
    close(fd);

    qtest_add_func("/apic/reset/cpuid-back", test_apic_back);
    qtest_add_func("/apic/reset/no-apic-stays-off", test_no_apic_stays_off);
    ret = g_test_run();

    unlink(bios_path);
    g_free(bios_path);
    return ret;
}
