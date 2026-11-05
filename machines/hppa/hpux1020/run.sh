#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# HP-UX 10.20 on an HP B160L (PA-7300LC): four CPUs, one host thread each, 512 MB RAM.
# There is no kit for this machine, so this script is an example. It runs
# qemu-system-hppa from PATH, or the one in $QEMU. Put these files next to it:
# - hpux.img: the HP-UX 10.20 disk, qcow2, on SCSI ID 6.
# - hppa-firmware-v25.img: SeaBIOS-hppa 25, from LEMU's history:
#     git show 90ce9a4150:pc-bios/hppa-firmware.img > hppa-firmware-v25.img
#   LEMU's own firmware (26) works too: then drop -bios and legacy-fw=on. Version 27
#   panics on four CPUs. legacy-fw=on puts the CPUs and fw_cfg where 25 expects them.
# tb-size=512 keeps the code buffer where it is fast on Apple Silicon. Don't add
# -d nochain (3 times slower). -display none does not start the X server: use the
# window, or -display vnc=127.0.0.1:1.
# An idle vCPU sleeps (hpux-idle=on is the default; =off makes it spin like HP-UX itself does).
# The X server's timer calibration is slowed down (hpux-x11-pace=on is the default); without
# it the X server crashes at start on a fast host and the VUE login never appears.
# The terminal holds the QEMU monitor.
cd "$(dirname "$0")" || exit 1
exec "${QEMU:-qemu-system-hppa}" \
	-name "HP-UX 10.20 PA-RISC B160L" \
	-M B160L,legacy-fw=on -smp 4 -m 512 \
	-accel tcg,thread=multi,tb-size=512 \
	-bios hppa-firmware-v25.img \
	-boot c -boot menu=off \
	-drive if=scsi,bus=0,index=6,file=hpux.img,format=qcow2 \
	-nic user,model=tulip \
	-monitor stdio \
	"$@"
