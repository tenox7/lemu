#!/bin/sh
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
# AmigaOS 4.1 on an ACube Sam440ep (sam440ep): PowerPC 440EP, 512 MB RAM,
# Radeon, SiI3114 SATA, USB keyboard and mouse, CS4281 sound and the 440EP's
# on-chip Ethernet (EMAC).
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-ppc ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-ppc ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-ppc ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# SDL on macOS drops the click that focuses a window
export SDL_MOUSE_FOCUS_CLICKTHROUGH=1
# fw/u-boot-sam440ep.bin is ACube's U-Boot built from source
# (machines/sam440ep/firmware in the QEMU source). It runs the Radeon's VGA
# BIOS, fw/vgabios-ati.bin, shows its boot menu in the window and boots the
# disk after 5 seconds.
# The CD-ROM drive is empty: change cd0 file.iso in the monitor.
# The terminal holds the QEMU monitor.
exec "$q" \
 -name "AmigaOS 4.1 440EP SAM440EP" \
 -M sam440ep -m 512 \
 -bios fw/u-boot-sam440ep.bin \
 -global ati-vga.romfile=fw/vgabios-ati.bin \
 -drive file=aos41.qcow2,if=ide,index=0,format=qcow2 \
 -drive if=ide,index=1,media=cdrom,id=cd0 \
 -nic user \
 -monitor stdio \
 "$@"
