@echo off
cd /d "%~dp0"
rem Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
rem AmigaOS 4.1 Final Edition Update 1 on a Genesi/bPlan Pegasos II (pegasos2): MPC7457 G4, 1 GB RAM. See run.sh.
bin\windows-x86_64\qemu-system-ppc.exe ^
 -name "AmigaOS 4.1 Final Edition Update 1 G4 PEGASOS2" ^
 -M pegasos2 -m 1024 ^
 -drive if=pflash,file=flash.bin,format=raw ^
 -rtc base=localtime ^
 -vga none -device ati-vga,model=rv280,vgamem_mb=64,romfile=,bus=pci.0 ^
 -drive file=aos41feu1.qcow2,if=ide,index=0,format=qcow2 ^
 -drive if=ide,index=2,media=cdrom,id=cd0 ^
 -nic user,model=via-rhine ^
 -nic user,model=mv643xx_eth ^
 -monitor stdio ^
 %*
