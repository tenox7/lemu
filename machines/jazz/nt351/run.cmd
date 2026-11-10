@echo off
cd /d "%~dp0"
qemu\windows-x64\qemu-system-mips64el.exe ^
 -name "Windows NT 3.51 MIPS JAZZ" ^
 -M magnum -m 128 ^
 -icount shift=3,align=on ^
 -rtc base=localtime ^
 -bios NTPROM.RAW ^
 -global ds1225y.filename=nvram.bin ^
 -monitor stdio ^
 -nic user,model=dp83932,hostfwd=tcp:127.0.0.1:36514-:514,hostfwd=tcp:127.0.0.1:36389-:3389 ^
 -drive file=nt351.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,drive=cd0,scsi-id=4 ^
 %*
