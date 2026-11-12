#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# SunOS 4.1.4 on a SPARCstation 5 (sun4m): 256 MB RAM, of which SunOS gets 240, and a cg3 screen.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-sparc ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-sparc ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-sparc ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The disk is at SCSI target 0, which SunOS calls sd3. The firmware is LEMU's OpenBIOS
# (bin/share/qemu). Notes: machines/sun4m/README.md in the QEMU source.
#   RES=1920x1080 ./run.sh   another screen size (default 1280x1024): from 640x480 to
#                            3200x1800, with a width that 8 divides
#   CD=/full/path/file.iso ./run.sh
#                            a CD in a drive at SCSI target 6, which SunOS calls sr0. The drive
#                            has blocks of 512 bytes, as Sun's drives: SunOS can't read others.
# An idle SunOS halts the CPU (sunos-idle is on by default).
# Network: QEMU user networking (NAT). SunOS has the fixed address 192.168.1.123, so QEMU
# is given that network: telnet port 41423 and rsh port 41414 on this host.
# The terminal holds the QEMU monitor.
exec "$q" \
	-name "SunOS 4.1.4 SPARC SS-5" \
	-M SS-5 -m 256 \
	-vga cg3 -g "${RES:-1280x1024}x8" \
	-drive file=sunos414.qcow2,if=scsi,bus=0,unit=0,format=qcow2 \
	${CD:+-drive "file=$CD,if=none,id=cd,media=cdrom,readonly=on,format=raw"} \
	${CD:+-device scsi-cd,drive=cd,scsi-id=6,physical_block_size=512} \
	-nic user,model=lance,net=192.168.1.0/24,host=192.168.1.10,dns=192.168.1.1,hostfwd=tcp:127.0.0.1:41423-192.168.1.123:23,hostfwd=tcp:127.0.0.1:41414-192.168.1.123:514 \
	-monitor stdio \
	"$@"
