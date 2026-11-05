@echo off
cd /d "%~dp0"
qemu\windows-x64\qemu-system-mips64el.exe ^
 -name "Windows NT 4.0 MIPS JAZZ" ^
 -M magnum -m 128 ^
 -bios NTPROM.RAW ^
 -global ds1225y.filename=nvram.bin ^
 -rtc base=1995-07-08T11:12:13,clock=vm ^
 -monitor stdio ^
 -nic user ^
 -drive file=nt40.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 %*
