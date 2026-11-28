#!/bin/sh
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack a BeBox kit: beos.qcow2, flash.bin, run.sh, run.cmd, README.txt and
# qemu-system-ppc for macOS, Linux and Windows.
# KIT is a folder next to this one: 1.1d7, 1.1d7.1, dr8, pr, pr2, r3, r4, r45 or
# r5 (see ../README.md).
# VMDIR holds the kit's beos.qcow2 (or beos.img for a raw one) and flash.bin, the
# boot flash the kit was installed with. It is only read. The pack converts the
# disk to qcow2, which leaves out what was never written.
# BINDIR is STAGE/ppc from ../../release/stage.sh.
# OUT names the kit: the folder inside is its file name without .tar.lz.
# usage: mkrelease.sh KIT VMDIR BINDIR OUT.tar.lz
set -e
[ $# -eq 4 ] || { echo "usage: $0 KIT VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
kit=$1 vm=$2 bin=$3
case $kit in
1.1d7|1.1d7.1|dr8|pr|pr2|r3|r4|r45|r5) ;;
*) echo "unknown kit: $kit" >&2; exit 1 ;;
esac
src=$vm/beos.qcow2
[ -f "$src" ] || src=$vm/beos.img
[ -f "$src" ] || { echo "$0: no $vm/beos.qcow2 or beos.img" >&2; exit 1; }
[ -f "$vm/flash.bin" ] || { echo "$0: no $vm/flash.bin" >&2; exit 1; }
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
sh "$here/../../release/sign-mac.sh" -c "$bin/macos-arm64"
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
r=$t/$name
mkdir -p "$r/bin/share/qemu/keymaps"

qemu-img convert -O qcow2 "$src" "$r/beos.qcow2"
cp "$vm/flash.bin" "$r/"
cp "$here/../$kit/run.sh" "$here/../$kit/README.txt" "$r/"
sed 's/$/\r/' "$here/../$kit/run.cmd" > "$r/run.cmd"
cp "$top/COPYING" "$top/COPYING.LIB" "$r/bin/"
find "$top/pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in $plats; do
    cp -a "$bin/$p" "$r/bin/"
done
chmod 755 "$r/run.sh"
chmod 644 "$r/beos.qcow2" "$r/flash.bin"

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - "$name" | plzip -9 > "$out.tmp"
lzip -t "$out.tmp"
mv "$out.tmp" "$out"
