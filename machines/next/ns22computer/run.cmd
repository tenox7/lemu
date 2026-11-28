@echo off
cd /d "%~dp0"
rem NeXTSTEP 2.2 on the NeXT Computer (next-computer): 68030, 16 MB RAM, 1120x832 screen with 4 greys. See run.sh.
bin\windows-x86_64\qemu-system-m68k.exe ^
 -name "NeXTSTEP 2.2 68030 NeXT Computer" ^
 -M next-computer -bios Rev_1.2.bin ^
 -drive file=nvram.bin,format=raw,if=mtd ^
 -drive file=ns22.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,scsi-id=0,drive=hd0 ^
 -nic user,hostfwd=tcp:127.0.0.1:32223-:23 ^
 -monitor stdio ^
 %*
