@echo off
cd /d "%~dp0"
rem OPENSTEP 4.0 on a NeXTcube Turbo (next-cube-turbo): 68040, 64 MB RAM, 1120x832 screen with 4 greys. See run.sh.
bin\windows-x86_64\qemu-system-m68k.exe ^
 -name "OPENSTEP 4.0 68040 NeXTcube Turbo" ^
 -M next-cube-turbo -bios Rev_3.3_v74.bin ^
 -drive file=nvram.bin,format=raw,if=mtd ^
 -drive file=os40.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,scsi-id=0,drive=hd0 ^
 -nic user,hostfwd=tcp:127.0.0.1:32403-:23 ^
 -monitor stdio ^
 %*
