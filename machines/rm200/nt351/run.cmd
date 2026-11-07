@echo off
cd /d "%~dp0"
qemu\windows-x64\qemu-system-mips64el.exe ^
 -name "Windows NT 3.51 MIPS RM200" ^
 -M sni-rm200,nvram=nvram.bin ^
 -m 128 ^
 -icount shift=3,align=on ^
 -rtc base=localtime ^
 -bios "RM200C-NT-AM29F040@PLCC32.BIN" ^
 -monitor stdio ^
 -nic user,model=pcnet ^
 -drive file=nt351.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,drive=cd0,scsi-id=5 ^
 %*
