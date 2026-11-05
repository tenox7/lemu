#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# HP-UX 9.05 on an HP 9000/715 (PA-7100LC): one CPU, 128 MB RAM, Artist graphics,
# SCSI disk, LASI LAN.
# There is no kit for this machine, so this script is an example. It runs
# qemu-system-hppa from PATH, which must be LEMU's. Put hpux905.qcow2 next to it:
# HP-UX 9.05 on SCSI ID 6 (README.md tells how to install it).
# The firmware is LEMU's SeaBIOS-hppa 27: the graphics console of HP-UX 9.05 needs
# its audio module. -boot c makes the firmware boot the disk by itself.
# HP-UX 9.05 has no SMP: one CPU. 128 MB is what the snapshots of the guest were
# made with: a snapshot loads only with the same RAM size.
# An idle vCPU sleeps (hpux-idle=on is the default). The X server's timer calibration
# is slowed down (hpux-x11-pace=on is the default); without it the X server dies
# with SIGFPE at start and the VUE login never appears.
# User mode network: HP-UX is 10.0.2.15, the host is 10.0.2.2. telnet localhost 2323,
# ftp localhost 2121 (root has no password).
# The terminal holds the QEMU monitor: savevm NAME and loadvm NAME take and load a
# snapshot of the whole VM, or start with -loadvm NAME.
cd "$(dirname "$0")" || exit 1
exec qemu-system-hppa \
	-name "HP-UX 9.05 PA-RISC 715" \
	-machine 715 -m 128 \
	-boot c -boot menu=off \
	-drive if=scsi,bus=0,index=6,file=hpux905.qcow2,format=qcow2 \
	-nic user,model=lasi_82596,hostfwd=tcp:127.0.0.1:2323-:23,hostfwd=tcp:127.0.0.1:2121-:21 \
	-monitor stdio \
	"$@"
