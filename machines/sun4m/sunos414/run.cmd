@echo off
cd /d "%~dp0"
rem SunOS 4.1.4 on a SPARCstation 5 (sun4m): 256 MB RAM, cg3 screen. See run.sh.
rem set RES=1920x1080 before run.cmd gives another screen size.
if "%RES%"=="" set RES=1280x1024
bin\windows-x86_64\qemu-system-sparc.exe ^
 -name "SunOS 4.1.4 SPARC SS-5" ^
 -M SS-5 -m 256 ^
 -vga cg3 -g %RES%x8 ^
 -drive file=sunos414.qcow2,if=scsi,bus=0,unit=0,format=qcow2 ^
 -nic user,model=lance,net=192.168.1.0/24,host=192.168.1.10,dns=192.168.1.1,hostfwd=tcp:127.0.0.1:41423-192.168.1.123:23,hostfwd=tcp:127.0.0.1:41414-192.168.1.123:514 ^
 -monitor stdio ^
 %*
