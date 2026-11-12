#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# HP-UX 9.07 on an HP 9000/715 (PA-7100LC): one CPU, 128 MB RAM, Artist graphics,
# SCSI disk, LASI LAN. This starts the kit hpux907-715-nnm-lemu (README.txt).
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-hppa ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-hppa ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-hppa ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The firmware is LEMU's SeaBIOS-hppa 27 (bin/share/qemu/hppa-firmware.img): the graphics
# console of HP-UX 9 needs its audio module. -boot c makes the firmware boot the disk.
# HP-UX 9.07 has no SMP: one CPU. A snapshot (savevm) loads only with the same RAM size.
# An idle vCPU sleeps (hpux-idle=on is the default). The X server's timer calibration
# is slowed down (hpux-x11-pace=on is the default); without it the X server dies
# with SIGFPE at start and the VUE login never appears. With -display none the VUE
# login still runs, out of sight; with -machine 715,graphics=off (no Artist card)
# the console repeats "X-server can not be started".
# User mode network on 192.168.1.0/24: HP-UX is 192.168.1.208, the host and gateway
# is 192.168.1.1. The NNM key is locked to .208. telnet localhost 9733
# (root has no password); ftp localhost 9731 needs a user with a password. The ports
# differ from the 9.05 and 9.07 kits', so all can run at the same time.
# The terminal holds the QEMU monitor: savevm NAME and loadvm NAME take and load a
# snapshot of the whole VM, or start with -loadvm NAME.
exec "$q" \
	-name "HP-UX 9.07 PA-RISC 715 NNM" \
	-machine 715 -m 128 \
	-boot c -boot menu=off \
	-drive if=scsi,bus=0,index=6,file=hpux907nnm.qcow2,format=qcow2 \
	-nic user,model=lasi_82596,net=192.168.1.0/24,host=192.168.1.1,hostfwd=tcp:127.0.0.1:9733-192.168.1.208:23,hostfwd=tcp:127.0.0.1:9731-192.168.1.208:21 \
	-monitor stdio \
	"$@"
