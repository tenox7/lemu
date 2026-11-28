#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# OPENSTEP 4.0 on a NeXTcube Turbo (next-cube-turbo): 68040, 64 MB RAM, 1120x832 screen with 4 greys.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-m68k ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-m68k ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-m68k ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The machine starts from NeXT's ROM, Rev_3.3_v74.bin. nvram.bin holds its NVRAM: the boot
# command, the Ethernet address, volume and brightness. The disk is at SCSI ID 0.
# Notes: machines/next/README.md in the QEMU source.
# Network: QEMU user networking (NAT), which also answers NeXTSTEP's bootparam,
# NetInfo and time requests. telnet port 32403 on this host reaches the guest.
# The terminal holds the QEMU monitor.
exec "$q" \
	-name "OPENSTEP 4.0 68040 NeXTcube Turbo" \
	-M next-cube-turbo -bios Rev_3.3_v74.bin \
	-drive file=nvram.bin,format=raw,if=mtd \
	-drive file=os40.qcow2,if=none,id=hd0,format=qcow2 \
	-device scsi-hd,scsi-id=0,drive=hd0 \
	-nic user,hostfwd=tcp:127.0.0.1:32403-:23 \
	-monitor stdio \
	"$@"
