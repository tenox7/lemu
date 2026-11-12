#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack a SunOS kit: the disk, run.sh, run.cmd, README.txt, the firmware and
# qemu-system-sparc for macOS, Linux and Windows.
# sunos414: SunOS 4.1.4 on the SPARCstation 5; VMDIR holds the disk sunos414.img (qcow2).
# The pack converts the disk, which leaves its snapshots (savevm) out.
# BINDIR is STAGE/sparc from ../../release/stage.sh.
# OUT names the kit: the folder inside is its file name without .tar.gz or .tar.lz.
# usage: mkrelease.sh sunos414 VMDIR BINDIR OUT.tar.gz|OUT.tar.lz
set -e
[ $# -eq 4 ] || { echo "usage: $0 sunos414 VMDIR BINDIR OUT.tar.gz|OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
os=$1 vm=$2 bin=$3 out=$4
case $os in
sunos414) img=$vm/sunos414.img ;;
*) echo "unknown OS: $os" >&2; exit 1 ;;
esac
case $out in
*.tar.gz) name=$(basename "$out" .tar.gz) pack="gzip -9" check="gzip -t" ;;
*.tar.lz) name=$(basename "$out" .tar.lz) pack="plzip -9" check="lzip -t" ;;
*) echo "OUT must end in .tar.gz or .tar.lz" >&2; exit 1 ;;
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
# the SPARCstation 5 boots OpenBIOS, which runs the frame buffer's FCode ROM
for f in openbios-sparc32 QEMU,cgthree.bin QEMU,tcx.bin; do
    cp "$here/../../../pc-bios/$f" "$r/bin/share/qemu/"
    cp "$here/../../../pc-bios/$f" "$r/bin/windows-x86_64/share/"
done
chmod 644 "$r/$os.qcow2"

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - "$name" | $pack > "$out"
$check "$out"
