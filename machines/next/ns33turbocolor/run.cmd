@echo off
cd /d "%~dp0"
rem NeXTSTEP 3.3 on a NeXTstation Turbo Color (next-station-turbo-color): 68040, 32 MB RAM, 1120x832 screen with 4096 colours. See run.sh.
bin\windows-x86_64\qemu-system-m68k.exe ^
 -name "NeXTSTEP 3.3 68040 NeXTstation Turbo Color" ^
 -M next-station-turbo-color -bios Rev_3.3_v74.bin ^
 -drive file=nvram.bin,format=raw,if=mtd ^
 -drive file=ns33.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,scsi-id=0,drive=hd0 ^
 -nic user,hostfwd=tcp:127.0.0.1:32333-:23 ^
 -monitor stdio ^
 %*
