#!/usr/bin/env python3
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a BeBox boot flash image from Be's nub and a main boot ROM.

usage: mkflash.py bootnub.rom MAIN OUTFILE

MAIN is bootmain.rom (ROM 1.1d7, 128 KiB Am29F010: main at 16 KiB) or
the bebox_bootmain.image of a BeOS CD (512 KiB Am29F040: main at 64 KiB,
where the nub looks next). The "BOOT" tag in front of R4.5's image is
dropped. The output is a raw image for
"-drive if=pflash,format=raw,file=OUTFILE"; the guest can rewrite it.
"""

import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    nub = open(sys.argv[1], "rb").read()
    main_rom = open(sys.argv[2], "rb").read()
    if len(nub) != 16 * 1024 or nub[:4] != b"\xff\xf0\x01\x00":
        sys.exit("%s is not Be's 16 KiB boot nub" % sys.argv[1])
    if main_rom[:16] == b"BOOTJoy!peffpwpc":
        # R4.5 and later tag the image; the flash holds the PEF alone
        main_rom = main_rom[4:]
    if main_rom[:12] != b"Joy!peffpwpc":
        sys.exit("%s is not a PEF boot ROM" % sys.argv[2])
    body = main_rom.rstrip(b"\0")
    if len(body) <= 128 * 1024 - len(nub):
        size, at = 128 * 1024, len(nub)
    elif len(body) <= 512 * 1024 - 64 * 1024:
        size, at = 512 * 1024, 64 * 1024
    else:
        sys.exit("%s is too big for a 512 KiB flash" % sys.argv[2])
    image = bytearray(b"\xff" * size)
    image[:len(nub)] = nub
    image[at:at + len(body)] = body
    open(sys.argv[3], "wb").write(image)
    print("%s: %d KiB, main boot ROM at 0x%x" % (sys.argv[3], size // 1024, at))


if __name__ == "__main__":
    main()
