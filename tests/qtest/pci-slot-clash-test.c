/*
 * QTest: a PCI device for a slot that is in use fails cleanly
 *
 * When a PCI device could not be put in its slot, its realize used to
 * give back its acpi-index through a NULL pointer, and QEMU crashed: on
 * the command line ("-vga none -device e1000,addr=02.0" clashes with
 * the default network card) and with device_add alike.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"

static void device_add_error(QTestState *qts, const char *args,
                             const char *want)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'device_add', 'arguments': %p}",
                           qobject_from_json(args, &error_abort));
    QDict *err = qdict_get_qdict(rsp, "error");
    const char *desc;

    g_assert(err);
    desc = qdict_get_str(err, "desc");
    if (!strstr(desc, want)) {
        g_test_message("'%s' lacks '%s'", desc, want);
        g_assert_not_reached();
    }
    qobject_unref(rsp);
}

static void test_slot_clash(void)
{
    QTestState *qts;

    qts = qtest_init("-machine pc -nodefaults -device e1000,addr=02.0");

    /* slot 2 is taken: an error, not a crash */
    device_add_error(qts, "{'driver': 'e1000', 'id': 'nic1',"
                     " 'addr': '02.0', 'acpi-index': 5}",
                     "not available");
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");

    /* the acpi-index of the device that failed is free again */
    qtest_qmp_assert_success(qts, "{'execute': 'device_add', 'arguments':"
                             " {'driver': 'e1000', 'id': 'nic2',"
                             " 'addr': '03.0', 'acpi-index': 5}}");
    device_add_error(qts, "{'driver': 'e1000', 'id': 'nic3',"
                     " 'addr': '04.0', 'acpi-index': 5}",
                     "acpi-index = 5 already exist");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_device("e1000")) {
        qtest_add_func("/pci/slot-clash", test_slot_clash);
    }
    return g_test_run();
}
