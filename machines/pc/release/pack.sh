#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack a PC kit: the disk and firmware from VMDIR, run.sh, run.cmd and
# README.txt from ../GUEST, the binaries from BINDIR
# usage: pack.sh nt351 VMDIR BINDIR OUT.tar.lz
#   VMDIR   holds disk/GUEST.qcow2, fw/bios-GUEST.bin and fw/928-n9.bin;
#           it is only read
#   BINDIR  STAGE/i386 of ../../release/stage.sh
set -e
[ $# -eq 4 ] || { echo "usage: $0 nt351 VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
g=$1 vm=$2 bin=$3
[ -f "$here/../$g/run.sh" ] || { echo "$0: no guest $g in $here/.." >&2; exit 1; }
for f in disk/$g.qcow2 fw/bios-$g.bin fw/928-n9.bin; do
    [ -f "$vm/$f" ] || { echo "$0: no $vm/$f" >&2; exit 1; }
done
plats="macos-arm64 linux-x86_64 linux-aarch64 windows-x86_64"
for p in $plats; do
    [ -d "$bin/$p" ] || { echo "$0: no $bin/$p" >&2; exit 1; }
done
out=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")
k=${g}x86-qemu
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
r=$t/$k
mkdir -p "$r/bin/share/qemu/keymaps" "$r/fw"

qemu-img convert -O qcow2 "$vm/disk/$g.qcow2" "$r/$g.qcow2"
cp "$vm/fw/bios-$g.bin" "$vm/fw/928-n9.bin" "$r/fw/"
cp "$here/../$g/run.sh" "$here/../$g/README.txt" "$r/"
sed 's/$/\r/' "$here/../$g/run.cmd" > "$r/run.cmd"
cp "$top/COPYING" "$top/COPYING.LIB" "$r/bin/"
find "$top/pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in $plats; do
    cp -a "$bin/$p" "$r/bin/"
done
# the PC loads this option ROM for the CPU's local APIC
cp "$top/pc-bios/kvmvapic.bin" "$r/bin/share/qemu/"
cp "$top/pc-bios/kvmvapic.bin" "$r/bin/windows-x86_64/share/"
chmod 644 "$r/$g.qcow2" "$r"/fw/*

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - $k | plzip -9 > "$out.tmp"
lzip -t "$out.tmp"
mv "$out.tmp" "$out"
