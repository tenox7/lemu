#!/usr/bin/env python3
#
# Functional test that boots the NetBSD/sparc install CD on a
# SPARCstation 5 and checks that the installer starts
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import QemuSystemTest, Asset
from qemu_test import wait_for_console_pattern


class NetBSDSun4m(QemuSystemTest):

    ASSET_CD = Asset(
        ('https://cdn.netbsd.org/pub/NetBSD/NetBSD-11.1/images/'
         'NetBSD-11.1-sparc.iso'),
        '4a8985ecf7d21dad66c34c184d1535a9f901ee4ccc4d962c37558545b63b4636')

    def test_netbsd_111_installer(self):
        self.set_machine('SS-5')
        cdrom_path = self.ASSET_CD.fetch()

        # The CD-ROM on the esp bus at target 6 with 512-byte blocks, as
        # on a real SPARCstation; OpenBIOS boots it with -boot d
        self.vm.set_console()
        self.vm.add_args('-m', '256',
                         '-prom-env', 'input-device=ttya',
                         '-prom-env', 'output-device=ttya',
                         '-drive', f'file={cdrom_path},if=none,id=cd,'
                                   'media=cdrom,readonly=on,format=raw',
                         '-device', 'scsi-cd,drive=cd,scsi-id=6,'
                                    'physical_block_size=512',
                         '-boot', 'd',
                         '-no-reboot')
        self.vm.launch()

        wait_for_console_pattern(self, 'NetBSD 11.1 (', 'panic:')
        # The microroot asks where to load the installation tools from
        wait_for_console_pattern(
            self,
            'Installation medium to load the additional utilities from:',
            'panic:')


if __name__ == '__main__':
    QemuSystemTest.main()
