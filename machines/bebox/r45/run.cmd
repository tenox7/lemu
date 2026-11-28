@echo off
rem Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
rem SPDX-License-Identifier: GPL-2.0-or-later
cd /d "%~dp0"
bin\windows-x86_64\qemu-system-ppc.exe ^
 -name "BeOS R4.5 POWERPC BEBOX" ^
 -M bebox -m 32 ^
 -drive if=pflash,file=flash.bin,format=raw ^
 -monitor stdio ^
 -drive file=beos.qcow2,if=none,id=hd0,format=qcow2 ^
 -device ide-hd,drive=hd0,bus=ide.0,unit=0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device ide-cd,drive=cd0,bus=ide.0,unit=1 ^
 -nic none -netdev user,id=n0 -device tulip-21041,netdev=n0,addr=0e ^
 -serial null -serial null -serial null -serial vc ^
 %*
