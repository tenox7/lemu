#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack a NeXT kit: the disk, the NVRAM and the ROM, run.sh, run.cmd, README.txt and
# qemu-system-m68k for macOS, Linux and Windows.
# KIT is a folder next to this one, such as ns22computer or os42color (see ../README.md).
# VMDIR holds the kit's disk under the name run.sh gives it (ns33.qcow2, or .img for a raw
# one), NeXT's ROM under the name run.sh gives it, and nvram.bin if the VM has one. It is
# only read. The pack converts the disk to qcow2, which leaves out what was never written.
# BINDIR is STAGE/m68k from ../../release/stage.sh.
# OUT names the kit: the folder inside is its file name without .tar.lz.
# usage: mkrelease.sh KIT VMDIR BINDIR OUT.tar.lz
set -e
[ $# -eq 4 ] || { echo "usage: $0 KIT VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
kit=$1 vm=$2 bin=$3
[ -f "$here/../$kit/run.sh" ] || { echo "unknown kit: $kit" >&2; exit 1; }
# the disk and the ROM, as run.sh names them
disk=$(sed -n 's/.*-drive file=\([^,]*\.qcow2\),if=none,id=hd0.*/\1/p' "$here/../$kit/run.sh")
rom=$(sed -n 's/.*-bios \([^ ]*\).*/\1/p' "$here/../$kit/run.sh")
[ -n "$disk" ] && [ -n "$rom" ] || { echo "$0: cannot read $kit/run.sh" >&2; exit 1; }
src=$vm/$disk
[ -f "$src" ] || src=$vm/${disk%.qcow2}.img
[ -f "$src" ] || { echo "$0: no $vm/$disk or ${disk%.qcow2}.img" >&2; exit 1; }
[ -f "$vm/$rom" ] || { echo "$0: no $vm/$rom" >&2; exit 1; }
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

qemu-img convert -O qcow2 "$src" "$r/$disk"
cp "$vm/$rom" "$r/"
# 64 bytes of NVRAM; zeros make the ROM build its defaults
if [ -f "$vm/nvram.bin" ]; then
    cp "$vm/nvram.bin" "$r/"
else
    head -c 64 /dev/zero > "$r/nvram.bin"
fi
cp "$here/../$kit/run.sh" "$here/../$kit/README.txt" "$r/"
sed 's/$/\r/' "$here/../$kit/run.cmd" > "$r/run.cmd"
cp "$top/COPYING" "$top/COPYING.LIB" "$r/bin/"
find "$top/pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in $plats; do
    cp -a "$bin/$p" "$r/bin/"
done
chmod 644 "$r/$disk" "$r/$rom" "$r/nvram.bin"

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - "$name" | plzip -9 > "$out.tmp"
lzip -t "$out.tmp"
mv "$out.tmp" "$out"
