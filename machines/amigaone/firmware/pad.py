#!/usr/bin/env python3
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Pads the AmigaOne U-Boot to its 512 KiB flash and puts at the end of it
# the routine that AmigaOS calls in the firmware ROM.
import sys

data = bytearray(open(sys.argv[1], 'rb').read())
if len(data) > 512 * 1024 - 0x80:
    sys.exit('u-boot.bin is too large for the flash')
data += b'\xff' * (512 * 1024 - len(data))
data[-0x80:-0x80 + 12] = bytes([0x54, 0x63, 0xc2, 0x3e,    # srwi r3,r3,8
                                0x7c, 0x63, 0x18, 0xf8,    # not  r3,r3
                                0x4e, 0x80, 0x00, 0x20])   # blr
open(sys.argv[2], 'wb').write(data)
