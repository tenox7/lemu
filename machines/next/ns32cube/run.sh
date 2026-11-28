#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# NeXTSTEP 3.2 on a NeXTcube (next-cube): 68040, 64 MB RAM, 1120x832 screen with 4 greys.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-m68k ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-m68k ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-m68k ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The machine starts from NeXT's ROM, Rev_2.5_v66.bin. nvram.bin holds its NVRAM: the boot
# command, the Ethernet address, volume and brightness. The disk is at SCSI ID 0.
# Notes: machines/next/README.md in the QEMU source.
# Network: QEMU user networking (NAT), which also answers NeXTSTEP's bootparam,
# NetInfo and time requests. telnet port 32320 on this host reaches the guest.
# The terminal holds the QEMU monitor.
exec "$q" \
	-name "NeXTSTEP 3.2 68040 NeXTcube" \
	-M next-cube -bios Rev_2.5_v66.bin \
	-drive file=nvram.bin,format=raw,if=mtd \
	-drive file=ns32.qcow2,if=none,id=hd0,format=qcow2 \
	-device scsi-hd,scsi-id=0,drive=hd0 \
	-nic user,hostfwd=tcp:127.0.0.1:32320-:23 \
	-monitor stdio \
	"$@"
