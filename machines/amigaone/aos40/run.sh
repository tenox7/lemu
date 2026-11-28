#!/bin/sh
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# AmigaOS 4.0 on an AmigaOne (amigaone): MPC7457 G4, 512 MB RAM, 64 MB Radeon 7000,
# on-board 3Com 3C920 Ethernet and VIA AC97 sound.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-ppc ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-ppc ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-ppc ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# SDL on macOS drops the click that focuses a window
export SDL_MOUSE_FOCUS_CLICKTHROUGH=1
# fw/u-boot-amigaone.bin is U-Boot built from source (machines/amigaone/firmware
# in the QEMU source). It runs the Radeon's VGA BIOS, fw/vgabios-ati.bin, shows
# its console in the window and boots the first entry of the second level
# booter on the disk after 5 seconds. The serial port is not used.
# The CD-ROM drive is empty: change cd0 file.iso in the monitor. U-Boot tries
# the CD first, so the AmigaOS CD boots when it is in the drive.
# The terminal holds the QEMU monitor.
exec "$q" \
 -name "AmigaOS 4.0 G4 AMIGAONE" \
 -M amigaone -m 512 \
 -bios fw/u-boot-amigaone.bin \
 -vga none -device ati-vga,model=rv100,vgamem_mb=64,romfile=fw/vgabios-ati.bin \
 -drive file=aos40.qcow2,if=none,id=hd0,format=qcow2 \
 -device ide-hd,drive=hd0,bus=ide.0,unit=0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device ide-cd,drive=cd0,bus=ide.1,unit=0 \
 -nic user \
 -monitor stdio \
 "$@"
