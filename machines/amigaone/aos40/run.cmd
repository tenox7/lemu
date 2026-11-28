@echo off
cd /d "%~dp0"
rem Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
rem AmigaOS 4.0 on an AmigaOne (amigaone): MPC7457 G4, 512 MB RAM, 64 MB Radeon 7000. See run.sh.
bin\windows-x86_64\qemu-system-ppc.exe ^
 -name "AmigaOS 4.0 G4 AMIGAONE" ^
 -M amigaone -m 512 ^
 -bios fw/u-boot-amigaone.bin ^
 -vga none -device ati-vga,model=rv100,vgamem_mb=64,romfile=fw/vgabios-ati.bin ^
 -drive file=aos40.qcow2,if=none,id=hd0,format=qcow2 ^
 -device ide-hd,drive=hd0,bus=ide.0,unit=0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device ide-cd,drive=cd0,bus=ide.1,unit=0 ^
 -nic user ^
 -monitor stdio ^
 %*
