@echo off
rem Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
rem SPDX-License-Identifier: GPL-2.0-or-later
cd /d "%~dp0"
bin\windows-x86_64\qemu-system-ppc.exe ^
 -name "BeOS 1.1d7.1 POWERPC BEBOX" ^
 -M bebox -m 32 ^
 -drive if=pflash,file=flash.bin,format=raw ^
 -monitor stdio ^
 -drive file=beos.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,drive=cd0,scsi-id=3 ^
 -nic none -netdev user,id=n0 -device ne2k_isa,netdev=n0,iobase=0x300,irq=5 ^
 -serial null -serial null -serial null -serial vc ^
 %*
