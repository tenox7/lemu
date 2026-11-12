#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# HP-UX 10.20 on an HP B160L (PA-7300LC): four CPUs, one host thread each, 512 MB RAM.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-hppa ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-hppa ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-hppa ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The firmware is LEMU's SeaBIOS-hppa 27 (bin/share/qemu/hppa-firmware.img). Version 26,
# the one in the first kit, boots four CPUs too, and 25 with -M legacy-fw=on.
# tb-size=512 keeps the code buffer where it is fast on Apple Silicon. Don't add
# -d nochain (3 times slower). With -display none the VUE login still runs, out of
# sight: use telnet or rsh, or -display vnc=127.0.0.1:1 to see it.
# An idle vCPU sleeps (hpux-idle=on is the default). The X server's timer calibration
# is slowed down (hpux-x11-pace=on is the default); without it the X server crashes at
# start on a fast host and the VUE login never appears.
# HP-UX has the fixed address 192.168.1.235, so the user mode network is 192.168.1.0/24.
# This starts the kit hpux1020b160l-nnm-lemu: HP-UX 10.20 with OpenView NNM 6.1 (README.txt).
# The terminal holds the QEMU monitor.
exec "$q" \
	-name "HP-UX 10.20 PA-RISC B160L NNM" \
	-M B160L -smp 4 -m 512 \
	-accel tcg,thread=multi,tb-size=512 \
	-boot c -boot menu=off \
	-drive if=scsi,bus=0,index=6,file=hpux1020nnm.qcow2,format=qcow2 \
	-nic user,model=tulip,net=192.168.1.0/24,host=192.168.1.2,hostfwd=tcp:127.0.0.1:10233-192.168.1.235:23,hostfwd=tcp:127.0.0.1:10534-192.168.1.235:514 \
	-monitor stdio \
	"$@"
