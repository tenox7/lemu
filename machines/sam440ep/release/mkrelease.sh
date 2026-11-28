#!/bin/sh
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack a Sam440ep kit: the disk, the firmware, run.sh, run.cmd, README.txt and
# qemu-system-ppc for macOS, Linux and Windows.
# aos41: AmigaOS 4.1. VMDIR holds the disk aos41.qcow2, as the user's VM folder
# does. It is only read. The firmware comes from this source tree: U-Boot
# (pc-bios/u-boot-sam440ep.bin, see ../firmware) and the Radeon's VGA BIOS.
# BINDIR is STAGE/ppc from ../../release/stage.sh.
# OUT names the kit: the folder inside is its file name without .tar.lz.
# usage: mkrelease.sh aos41 VMDIR BINDIR OUT.tar.lz
set -e
[ $# -eq 4 ] || { echo "usage: $0 aos41 VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
os=$1 vm=$2 bin=$3
case $os in
aos41) disk=aos41.qcow2 ;;
*) echo "unknown OS: $os" >&2; exit 1 ;;
esac
[ -f "$vm/$disk" ] || { echo "$0: no $vm/$disk" >&2; exit 1; }
plats="macos-arm64 linux-x86_64 linux-aarch64 windows-x86_64"
for p in $plats; do
    [ -d "$bin/$p" ] || { echo "$0: no $bin/$p" >&2; exit 1; }
done
case $4 in
*.tar.lz) name=$(basename "$4" .tar.lz) ;;
*) echo "OUT must end in .tar.lz" >&2; exit 1 ;;
esac
out=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")
# the macOS binaries must be signed with the Developer ID and notarized
sh "$top/machines/release/sign-mac.sh" -c "$bin/macos-arm64"
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
r=$t/$name
mkdir -p "$r/bin/share/qemu/keymaps" "$r/fw"

# a fresh copy leaves out the space the guest freed
qemu-img convert -O qcow2 "$vm/$disk" "$r/$disk"
cp "$top/pc-bios/u-boot-sam440ep.bin" "$top/pc-bios/vgabios-ati.bin" "$r/fw/"
cp "$here/../$os/run.sh" "$here/../$os/README.txt" "$r/"
sed 's/$/\r/' "$here/../$os/run.cmd" > "$r/run.cmd"
cp "$top/COPYING" "$top/COPYING.LIB" "$r/bin/"
find "$top/pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in $plats; do
    cp -a "$bin/$p" "$r/bin/"
done
chmod 644 "$r/$disk" "$r"/fw/*

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - "$name" | plzip -9 > "$out.tmp"
lzip -t "$out.tmp"
mv "$out.tmp" "$out"
