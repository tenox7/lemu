@echo off
cd /d "%~dp0"
bin\windows-x86_64\qemu-system-ppc.exe ^
 -name "Windows NT 3.51 POWERPC 40P" ^
 -M 40p -m 128 ^
 -bios fw/P12H0456.IMG ^
 -drive file=nvram.bin,if=none,id=nvram,format=raw -global isa-m48t59.drive=nvram ^
 -rtc base=localtime ^
 -vga none -device p9100 ^
 -monitor stdio ^
 -nic none -netdev user,id=n0,hostfwd=tcp:127.0.0.1:38514-:514,hostfwd=tcp:127.0.0.1:38389-:3389 -device ne2k_isa,iobase=0x300,irq=5,netdev=n0 ^
 -drive if=floppy,index=0,id=fd0 ^
 -drive file=nt351.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,drive=cd0,scsi-id=3 ^
 %*
