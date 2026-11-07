@echo off
cd /d "%~dp0"
rem HP-UX 9.07 on an HP 9000/715 (PA-7100LC): one CPU, 128 MB RAM. See run.sh.
bin\windows-x86_64\qemu-system-hppa.exe ^
 -name "HP-UX 9.07 PA-RISC 715" ^
 -machine 715 -m 128 ^
 -boot c -boot menu=off ^
 -drive if=scsi,bus=0,index=6,file=hpux907.qcow2,format=qcow2 ^
 -nic user,model=lasi_82596,hostfwd=tcp:127.0.0.1:9723-:23,hostfwd=tcp:127.0.0.1:9721-:21 ^
 -monitor stdio ^
 %*
