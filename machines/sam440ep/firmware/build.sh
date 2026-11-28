#!/bin/sh
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds pc-bios/u-boot-sam440ep.bin: ACube's U-Boot for the Sam440ep
# (u-boot-2015.d, GPL) with the changes LEMU needs (u-boot-sam440ep.patch),
# built with the cross compiler of ../../amigaone/firmware/Dockerfile.
#
# The source is ACube's tarball. It is downloaded unless TARBALL names a
# copy of it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
url=https://acube-systems.biz/download/u-boot-2015.d_prod.tar.gz
sha256=806799c1e5b0435e442a660499508158dce574b7b58d7be364d64649afe94066

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tarball=${TARBALL:-$work/u-boot-2015.d_prod.tar.gz}
[ -f "$tarball" ] || curl -sSfL -o "$tarball" "$url"
echo "$sha256  $tarball" | sha256sum -c --quiet -
tar -xzf "$tarball" -C "$work"
src=$work/u-boot-2015.d
patch -d "$src" -p1 -s < "$here/u-boot-sam440ep.patch"
# ATI's video BIOS for the M9 is not used: leave it out of the tree
rm -f "$src/board/ACube/bios_emulator_helper/radeon_m9_bios.h"
docker build -q -t lemu-u-boot-ppc "$top/machines/amigaone/firmware" >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$src:/src" -w /src \
    lemu-u-boot-ppc sh -c 'make -s Sam440ep_config &&
                            make -s CROSS_COMPILE=powerpc-linux-gnu- u-boot.bin'
[ "$(stat -c %s "$src/u-boot.bin")" -eq 524288 ] || {
    echo "u-boot.bin is not 512 KB" >&2; exit 1; }
cp "$src/u-boot.bin" "$top/pc-bios/u-boot-sam440ep.bin"
echo "Built $top/pc-bios/u-boot-sam440ep.bin"
