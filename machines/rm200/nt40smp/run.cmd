@echo off
cd /d "%~dp0"
qemu\windows-x64\qemu-system-mips64el.exe ^
 -name "Windows NT 4.0 MIPS RM300" ^
 -M sni-rm300,nvram=nvram-smp.bin,scsi-delay=0 ^
 -smp 2 ^
 -m 64 ^
 -rtc base=localtime ^
 -accel tcg,thread=multi ^
 -bios "RM200C-NT-SMP.BIN" ^
 -monitor stdio ^
 -nic user,model=pcnet,hostfwd=tcp:127.0.0.1:42514-:514,hostfwd=tcp:127.0.0.1:42389-:3389 ^
 -drive file=nt40-smp.hda,if=none,id=hd0,format=raw ^
 -device scsi-hd,drive=hd0,scsi-id=0 ^
 %*
