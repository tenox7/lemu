#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -e
[ $# -eq 4 ] || { echo "usage: $0 aix53|aix61|aix71|aix72 VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
os=$1 vm=$2 bin=$3 out=$4
case $os in
aix53) name=aix53pseries-lemu img=$vm/aix53.qcow2 ;;
aix61) name=aix61pseries-lemu img=$vm/aix61.qcow2 ;;
aix71) name=aix71pseries-lemu img=$vm/aix71.qcow2 ;;
aix72) name=aix72pseries-lemu img=$vm/hdisk0.qcow2 ;;
*) echo "unknown OS: $os" >&2; exit 1 ;;
esac
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
r=$t/$name
mkdir -p "$r/bin/share/qemu/keymaps"

qemu-img convert -O qcow2 "$img" "$r/$os.qcow2"
cp "$here/../$os/run.sh" "$here/../$os/README.txt" "$r/"
sed 's/$/\r/' "$here/../$os/run.cmd" > "$r/run.cmd"
cp "$here/../../../COPYING" "$here/../../../COPYING.LIB" "$r/bin/"
find "$here/../../../pc-bios/keymaps" -type f ! -name meson.build \
    -exec cp {} "$r/bin/share/qemu/keymaps/" \;
for p in macos-arm64 linux-x86_64 linux-aarch64 windows-x86_64; do
    cp -a "$bin/$p" "$r/bin/"
done
# the pseries machine boots the SLOF firmware
cp "$here/../../../pc-bios/slof.bin" "$r/bin/share/qemu/"
cp "$here/../../../pc-bios/slof.bin" "$r/bin/windows-x86_64/share/"
chmod 644 "$r/$os.qcow2"

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - $name | plzip -9 > "$out"
lzip -t "$out"
