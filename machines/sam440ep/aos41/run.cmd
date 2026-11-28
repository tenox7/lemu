@echo off
cd /d "%~dp0"
rem Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
rem AmigaOS 4.1 on an ACube Sam440ep (sam440ep): PowerPC 440EP, 512 MB RAM. See run.sh.
bin\windows-x86_64\qemu-system-ppc.exe ^
 -name "AmigaOS 4.1 440EP SAM440EP" ^
 -M sam440ep -m 512 ^
 -bios fw/u-boot-sam440ep.bin ^
 -global ati-vga.romfile=fw/vgabios-ati.bin ^
 -drive file=aos41.qcow2,if=ide,index=0,format=qcow2 ^
 -drive if=ide,index=1,media=cdrom,id=cd0 ^
 -nic user ^
 -monitor stdio ^
 %*
