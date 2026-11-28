@echo off
cd /d "%~dp0"
rem NeXTSTEP 2.1 on a NeXTstation Color (next-station-color): 68040, 32 MB RAM, 1120x832 screen with 4096 colours. See run.sh.
bin\windows-x86_64\qemu-system-m68k.exe ^
 -name "NeXTSTEP 2.1 68040 NeXTstation Color" ^
 -M next-station-color -bios Rev_2.5_v66.bin ^
 -drive file=nvram.bin,format=raw,if=mtd ^
 -drive file=ns21.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,scsi-id=0,drive=hd0 ^
 -nic user,hostfwd=tcp:127.0.0.1:32213-:23 ^
 -monitor stdio ^
 %*
