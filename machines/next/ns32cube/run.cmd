@echo off
cd /d "%~dp0"
rem NeXTSTEP 3.2 on a NeXTcube (next-cube): 68040, 64 MB RAM, 1120x832 screen with 4 greys. See run.sh.
bin\windows-x86_64\qemu-system-m68k.exe ^
 -name "NeXTSTEP 3.2 68040 NeXTcube" ^
 -M next-cube -bios Rev_2.5_v66.bin ^
 -drive file=nvram.bin,format=raw,if=mtd ^
 -drive file=ns32.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,scsi-id=0,drive=hd0 ^
 -nic user,hostfwd=tcp:127.0.0.1:32320-:23 ^
 -monitor stdio ^
 %*
