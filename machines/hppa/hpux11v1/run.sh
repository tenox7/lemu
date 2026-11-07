#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# HP-UX 11i v1 (B.11.11) on an HP B160L (PA-7300LC): four CPUs, one host thread each, 512 MB RAM.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-hppa ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-hppa ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-hppa ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The firmware is LEMU's SeaBIOS-hppa 27 (bin/share/qemu/hppa-firmware.img).
# tb-size=512 keeps the code buffer where it is fast on Apple Silicon. Don't add
# -d nochain (3 times slower); the first kit needed it on Apple Silicon, LEMU does not.
# An idle vCPU sleeps (hpux-idle=on is the default).
# HP-UX takes its address from DHCP. Its /etc/hosts says hpux is 192.168.1.109, and the
# CDE login waits for that address, so QEMU's DHCP hands it out.
# The terminal holds the QEMU monitor.
exec "$q" \
	-name "HP-UX 11i v1 PA-RISC B160L" \
	-M B160L -smp 4 -m 512 \
	-accel tcg,thread=multi,tb-size=512 \
	-boot c -boot menu=off \
	-drive if=scsi,bus=0,index=6,file=hpux11v1.qcow2,format=qcow2 \
	-nic user,model=tulip,net=192.168.1.0/24,host=192.168.1.1,dhcpstart=192.168.1.109,hostfwd=tcp:127.0.0.1:11123-192.168.1.109:23,hostfwd=tcp:127.0.0.1:11122-192.168.1.109:22 \
	-monitor stdio \
	"$@"
