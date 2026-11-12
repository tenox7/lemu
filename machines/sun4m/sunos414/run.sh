#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# SunOS 4.1.4 on a SPARCstation 5 (sun4m): 256 MB RAM, of which SunOS gets 240, and a cg3 screen.
# Expects sunos414.qcow2 here, the disk at SCSI target 0 (SunOS calls it sd3), and LEMU's
# qemu-system-sparc in $QEMU or on the PATH. Notes: ../README.md.
#   RES=1920x1080 ./run.sh   another screen size (default 1280x1024)
#   CD=file.iso ./run.sh     a CD in a drive at SCSI target 6, which SunOS calls sr0. The drive
#                            has blocks of 512 bytes, as Sun's drives: SunOS can't read others.
# An idle SunOS halts the CPU (sunos-idle is on by default).
# Network: QEMU user networking (NAT). SunOS has a fixed address, here 192.168.1.123, so QEMU
# is given that network: telnet port 41423 and rsh port 41414 on this host.
# The terminal holds the QEMU monitor.
cd "$(dirname "$0")" || exit 1
exec "${QEMU:-qemu-system-sparc}" \
	-name "SunOS 4.1.4 SPARC SS-5" \
	-M SS-5 -m 256 \
	-vga cg3 -g "${RES:-1280x1024}x8" \
	-drive file=sunos414.qcow2,if=scsi,bus=0,unit=0,format=qcow2 \
	${CD:+-drive "file=$CD,if=none,id=cd,media=cdrom,readonly=on,format=raw"} \
	${CD:+-device scsi-cd,drive=cd,scsi-id=6,physical_block_size=512} \
	-nic user,model=lance,net=192.168.1.0/24,host=192.168.1.10,dns=192.168.1.1,hostfwd=tcp:127.0.0.1:41423-192.168.1.123:23,hostfwd=tcp:127.0.0.1:41414-192.168.1.123:514 \
	-monitor stdio \
	"$@"
