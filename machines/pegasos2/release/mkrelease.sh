#!/bin/sh
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack a Pegasos II kit: the disk, boot.img or flash.bin, run.sh, run.cmd,
# README.txt and qemu-system-ppc for macOS, Linux and Windows.
# morphos: MorphOS 3.21. VMDIR holds the disk morphos.qcow2 and boot.img, the
# boot image from the MorphOS CD, as the user's VM folder does. They are only
# read. The firmware is QEMU's Virtual Open Firmware (pc-bios/vof.bin) with
# its device tree (pc-bios/dtb/pegasos2.dtb), which QEMU looks for in its data
# folder.
# aos41feu1: AmigaOS 4.1 Final Edition Update 1. VMDIR holds the disk
# aos41feu1.qcow2 and flash.bin, the board's flash: bPlan's SmartFirmware
# (from its Pegasos II update up050404) with the NVRAM set to boot hd:0
# amigaboot.of.
# BINDIR is STAGE/ppc from ../../release/stage.sh.
# OUT names the kit: the folder inside is its file name without .tar.lz.
# usage: mkrelease.sh morphos|aos41feu1 VMDIR BINDIR OUT.tar.lz
set -e
[ $# -eq 4 ] || { echo "usage: $0 morphos|aos41feu1 VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
os=$1 vm=$2 bin=$3
case $os in
morphos) disk=morphos.qcow2 boot=boot.img ;;
aos41feu1) disk=aos41feu1.qcow2 boot=flash.bin ;;
*) echo "unknown OS: $os" >&2; exit 1 ;;
esac
for f in "$disk" "$boot"; do
    [ -f "$vm/$f" ] || { echo "$0: no $vm/$f" >&2; exit 1; }
done
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
mkdir -p "$r/bin/share/qemu/keymaps"

# a fresh copy leaves out the space the guest freed
qemu-img convert -O qcow2 "$vm/$disk" "$r/$disk"
cp "$vm/$boot" "$r/"
cp "$here/../$os/run.sh" "$here/../$os/README.txt" "$r/"
sed 's/$/\r/' "$here/../$os/run.cmd" > "$r/run.cmd"
cp "$top/COPYING" "$top/COPYING.LIB" "$r/bin/"
find "$top/pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in $plats; do
    cp -a "$bin/$p" "$r/bin/"
done
if [ "$os" = morphos ]; then
    for d in "$r/bin/share/qemu" "$r/bin/windows-x86_64/share"; do
        mkdir -p "$d/dtb"
        cp "$top/pc-bios/vof.bin" "$d/"
        cp "$top/pc-bios/dtb/pegasos2.dtb" "$d/dtb/"
    done
fi
chmod 644 "$r/$disk" "$r/$boot"

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - "$name" | plzip -9 > "$out.tmp"
lzip -t "$out.tmp"
mv "$out.tmp" "$out"
