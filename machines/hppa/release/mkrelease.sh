#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack an HP-UX kit: the disk, run.sh, run.cmd, README.txt, the firmware and
# qemu-system-hppa for macOS, Linux and Windows.
# hpux1020: HP-UX 10.20 on the HP B160L; VMDIR holds the disk hpux.img (qcow2).
# hpux1020nnm: HP-UX 10.20 with OpenView NNM 6.1 on the B160L; VMDIR holds hpux.img.
# hpux905: HP-UX 9.05 on the HP 9000/715; VMDIR holds the disk hpux905.qcow2.
# hpux907: HP-UX 9.07 on the HP 9000/715; VMDIR holds the disk hpux907.qcow2.
# hpux907nnm: HP-UX 9.07 with OpenView NNM 3.3 on the 715; VMDIR holds hpux907.qcow2.
# hpux1100: HP-UX 11.00 on the HP B160L; VMDIR holds the disk hpux1100.qcow2.
# hpux11v1: HP-UX 11i v1 on the HP B160L; VMDIR holds the disk hpux.img (qcow2).
# hpux11v2: HP-UX 11i v2 (64-bit) on the HP C3700; VMDIR holds the disk hpux1123.qcow2.
# hpux11v3: HP-UX 11i v3 on the HP A400 (pluto=on); VMDIR holds the disk hpux1131.qcow2.
# The pack converts the disk, which leaves its snapshots (savevm) out.
# BINDIR is STAGE/hppa from ../../release/stage.sh.
# usage: mkrelease.sh hpux1020|hpux1020nnm|hpux905|hpux907|hpux907nnm|hpux1100|hpux11v1|hpux11v2|hpux11v3 VMDIR BINDIR OUT.tar.lz
set -e
[ $# -eq 4 ] || { echo "usage: $0 hpux1020|hpux1020nnm|hpux905|hpux907|hpux907nnm|hpux1100|hpux11v1|hpux11v2|hpux11v3 VMDIR BINDIR OUT.tar.lz" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
os=$1 vm=$2 bin=$3 out=$4
# the B160L and the 715 boot the 32-bit SeaBIOS-hppa firmware, the C3700 the
# 64-bit one; artist-pci reads its STI ROM, ati-vga its PCI ROM
fw=hppa-firmware.img
case $os in
hpux1020) name=hpux1020b160l-lemu img=$vm/hpux.img ;;
hpux1020nnm) name=hpux1020b160l-nnm-lemu img=$vm/hpux.img ;;
hpux905) name=hpux905-715-lemu img=$vm/hpux905.qcow2 ;;
hpux907) name=hpux907-715-lemu img=$vm/hpux907.qcow2 ;;
hpux907nnm) name=hpux907-715-nnm-lemu img=$vm/hpux907.qcow2 ;;
hpux1100) name=hpux1100b160l-lemu img=$vm/hpux1100.qcow2 ;;
hpux11v1) name=hpux11v1b160l-lemu img=$vm/hpux.img ;;
hpux11v2) name=hpux11v2c3700-lemu img=$vm/hpux1123.qcow2
	fw="hppa-firmware64.img hppa-sti-artist.rom" ;;
hpux11v3) name=hpux11v3a400-lemu img=$vm/hpux1131.qcow2
	fw="hppa-firmware64.img vgabios-ati.bin hppa-sti-radeon.rom" ;;
*) echo "unknown OS: $os" >&2; exit 1 ;;
esac
# the macOS binaries must be signed with the Developer ID and notarized
sh "$here/../../release/sign-mac.sh" -c "$bin/macos-arm64"
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
for f in $fw; do
    cp "$here/../../../pc-bios/$f" "$r/bin/share/qemu/"
    cp "$here/../../../pc-bios/$f" "$r/bin/windows-x86_64/share/"
done
chmod 644 "$r/$os.qcow2"

COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -C "$t" -cf - $name | plzip -9 > "$out"
lzip -t "$out"
