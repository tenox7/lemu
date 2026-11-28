#!/bin/sh
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds pc-bios/u-boot-amigaone.bin: the ACube U-Boot of roms/u-boot-sam460ex
# with the AmigaOne board added back (u-boot-amigaone.patch), built with the
# cross compiler of the Dockerfile here.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
base=1e5f4a1    # the commit of roms/u-boot-sam460ex the patch applies to

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -C "$top/roms/u-boot-sam460ex" archive "$base" | tar -x -C "$work"
patch -d "$work" -p1 -s < "$here/u-boot-amigaone.patch"
docker build -q -t lemu-u-boot-ppc "$here" >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$work:/src" -w /src \
    lemu-u-boot-ppc sh -c 'make -s AmigaOneG3SE_config &&
                            make -s CROSS_COMPILE=powerpc-linux-gnu-'
python3 "$here/pad.py" "$work/u-boot.bin" "$top/pc-bios/u-boot-amigaone.bin"
echo "Built $top/pc-bios/u-boot-amigaone.bin"
