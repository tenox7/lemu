#!/usr/bin/env python3
#
# Functional test that boots the NetBSD/sparc64 install CD on sun4u
# from the built-in IDE controller (cmd646) and checks that the
# installer starts
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import QemuSystemTest, Asset
from qemu_test import wait_for_console_pattern


class NetBSDSun4u(QemuSystemTest):

    ASSET_CD = Asset(
        ('https://cdn.netbsd.org/pub/NetBSD/NetBSD-11.1/images/'
         'NetBSD-11.1-sparc64.iso'),
        'a782b26be57325c8419fdb02030ea4e686a6ccf097baad2dae0a1a0c39e96f4e')

    def test_netbsd_111_installer(self):
        self.set_machine('sun4u')
        cdrom_path = self.ASSET_CD.fetch()

        # The CD-ROM is the secondary master of the cmd646, where
        # OpenBIOS's cdrom alias points; it boots it with -boot d
        self.vm.set_console()
        self.vm.add_args('-m', '512',
                         '-prom-env', 'input-device=ttya',
                         '-prom-env', 'output-device=ttya',
                         '-drive', f'file={cdrom_path},if=none,id=cd,'
                                   'media=cdrom,readonly=on,format=raw',
                         '-device', 'ide-cd,bus=ide.1,unit=0,drive=cd',
                         '-boot', 'd',
                         '-no-reboot')
        self.vm.launch()

        wait_for_console_pattern(self, 'NetBSD 11.1 (', 'panic:')
        # The root file system on the CD is mounted and sysinst's
        # start script asks for the terminal type
        wait_for_console_pattern(self, 'Terminal type (just hit ENTER',
                                 'panic:')


if __name__ == '__main__':
    QemuSystemTest.main()
