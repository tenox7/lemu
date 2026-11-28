@echo off
cd /d "%~dp0"
rem HP-UX 11i v3 (B.11.31) on an HP A400 (PA-8700): one CPU, 2 GB RAM, Radeon 7500. See run.sh.
rem set CONSOLE=graphics before run.cmd: the card is the system console (see run.sh).
set ROM=
set CON=
if "%CONSOLE%"=="graphics" (
 set ROM=,romfile=hppa-sti-radeon.rom
 set CON=-fw_cfg opt/console,string=graphics
)
bin\windows-x86_64\qemu-system-hppa.exe ^
 -name "HP-UX 11i v3 PA-RISC A400" ^
 -M A400,pluto=on -smp 1 -m 2048 ^
 -accel tcg,thread=multi,tb-size=512 ^
 -boot c -boot menu=off ^
 -drive if=scsi,bus=0,index=6,file=hpux11v3.qcow2,format=qcow2 ^
 -nic user,model=tulip,hostfwd=tcp:127.0.0.1:11131-10.0.2.15:23 ^
 -device ati-vga,model=rv200,x-linear-aper-size=0x4000000,guest_hwcursor=true%ROM% ^
 -serial mon:stdio %CON% ^
 %*
