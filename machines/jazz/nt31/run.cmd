@echo off
cd /d "%~dp0"
qemu\windows-x64\qemu-system-mips64el.exe ^
 -name "Windows NT 3.1 MIPS JAZZ" ^
 -M magnum -m 64 ^
 -icount shift=3,align=on ^
 -bios NTPROM.RAW ^
 -global ds1225y.filename=nvram.bin ^
 -monitor stdio ^
 -nic user,hostfwd=tcp:127.0.0.1:32514-:514,hostfwd=tcp:127.0.0.1:32389-:3389 ^
 -drive file=nt31.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 %*
