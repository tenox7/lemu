@echo off
cd /d "%~dp0"
qemu\windows-x64\qemu-system-mips64el.exe ^
 -name "Windows NT 4.0 MIPS RM200" ^
 -M sni-rm200,nvram=nvram-nt.bin ^
 -m 64 ^
 -icount shift=3,align=on ^
 -rtc base=localtime ^
 -bios "RM200C-NT-AM29F040@PLCC32.BIN" ^
 -monitor stdio ^
 -nic user,model=pcnet ^
 -drive file=nt40.hda,if=none,id=hd0,format=raw ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 %*
