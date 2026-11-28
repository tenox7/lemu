@echo off
cd /d "%~dp0"
rem NeXTSTEP 3.3 on a NeXTstation (next-station): 68040, 32 MB RAM, 1120x832 screen with 4 greys. See run.sh.
bin\windows-x86_64\qemu-system-m68k.exe ^
 -name "NeXTSTEP 3.3 68040 NeXTstation" ^
 -M next-station -bios Rev_2.5_v66.bin ^
 -drive file=nvram.bin,format=raw,if=mtd ^
 -drive file=ns33.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,scsi-id=0,drive=hd0 ^
 -nic user,hostfwd=tcp:127.0.0.1:32323-:23 ^
 -monitor stdio ^
 %*
