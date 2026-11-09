#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack an HP-UX kit: the disk, run.sh, run.cmd, README.txt, the firmware and
# qemu-system-hppa for macOS, Linux and Windows.
# hpux1020: HP-UX 10.20 on the HP B160L; VMDIR holds the disk hpux.img (qcow2).
# hpux905: HP-UX 9.05 on the HP 9000/715; VMDIR holds the disk hpux905.qcow2.
# hpux907: HP-UX 9.07 on the HP 9000/715; VMDIR holds the disk hpux907.qcow2.
# hpux907nnm: HP-UX 9.07 with OpenView NNM 3.3 on the 715; VMDIR holds hpux907.qcow2.
# hpux11v1: HP-UX 11i v1 on the HP B160L; VMDIR holds the disk hpux.img (qcow2).
# The pack converts the disk, which leaves its snapshots (savevm) out.
# BINDIR is STAGE/hppa from ../../release/stage.sh.
# usage: mkrelease.sh hpux1020|hpux905|hpux907|hpux907nnm|hpux11v1 VMDIR BINDIR OUT.tar.lz
set -e
[ $# -eq 4 ] || { echo "usage: $0 hpux1020|hpux905|hpux907|hpux907nnm|hpux11v1 VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
os=$1 vm=$2 bin=$3 out=$4
case $os in
hpux1020) name=hpux1020b160l-qemu img=$vm/hpux.img ;;
hpux905) name=hpux905-715-qemu img=$vm/hpux905.qcow2 ;;
hpux907) name=hpux907-715-qemu img=$vm/hpux907.qcow2 ;;
hpux907nnm) name=hpux907-715-nnm-qemu img=$vm/hpux907.qcow2 ;;
hpux11v1) name=hpux11v1b160l-qemu img=$vm/hpux.img ;;
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
# the B160L and the 715 boot the SeaBIOS-hppa firmware
cp "$here/../../../pc-bios/hppa-firmware.img" "$r/bin/share/qemu/"
cp "$here/../../../pc-bios/hppa-firmware.img" "$r/bin/windows-x86_64/share/"
chmod 644 "$r/$os.qcow2"

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - $name | plzip -9 > "$out"
lzip -t "$out"
