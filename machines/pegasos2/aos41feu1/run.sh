#!/bin/sh
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# AmigaOS 4.1 Final Edition Update 1 on a Genesi/bPlan Pegasos II (pegasos2): MPC7457
# G4, 1 GB RAM, a 64 MB Radeon 9250 in the AGP slot, on-board VIA Rhine-II
# Ethernet and VIA AC97 sound.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-ppc ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-ppc ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-ppc ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# SDL on macOS drops the click that focuses a window
export SDL_MOUSE_FOCUS_CLICKTHROUGH=1
# flash.bin is the board's flash: bPlan's SmartFirmware and its NVRAM, set
# to boot amigaboot.of from the first partition of the disk (hd:0) without
# waiting. The firmware console is the serial port. The Radeon needs no VGA
# BIOS. The CD-ROM drive is empty: change cd0 file.iso in the monitor.
# The terminal holds the QEMU monitor.
exec "$q" \
 -name "AmigaOS 4.1 Final Edition Update 1 G4 PEGASOS2" \
 -M pegasos2 -m 1024 \
 -drive if=pflash,file=flash.bin,format=raw \
 -rtc base=localtime \
 -vga none -device ati-vga,model=rv280,vgamem_mb=64,romfile=,bus=pci.0 \
 -drive file=aos41feu1.qcow2,if=ide,index=0,format=qcow2 \
 -drive if=ide,index=2,media=cdrom,id=cd0 \
 -nic user,model=via-rhine \
 -nic user,model=mv643xx_eth \
 -monitor stdio \
 "$@"
