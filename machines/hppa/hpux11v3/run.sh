#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# HP-UX 11i v3 (B.11.31) on an HP A400 (PA-8700) with a zx1 style system bus adapter
# (pluto=on): one CPU, 2 GB RAM, an ATI Radeon 7500 PCI graphics card for CDE.
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-hppa ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-hppa ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-hppa ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# The firmware is LEMU's SeaBIOS-hppa (bin/share/qemu/hppa-firmware64.img), and the
# card's PCI ROM is bin/share/qemu/vgabios-ati.bin.
# tb-size=512 keeps the code buffer where it is fast on Apple Silicon.
# The terminal is the serial console. Ctrl-A c = QEMU monitor (again = back), Ctrl-A x = quit.
# CONSOLE=graphics ./run.sh: the card is the system console. The card's ROM is then
#   hppa-sti-radeon.rom (the VGA BIOS and an STI ROM): firmware text, HP-UX boot messages
#   and the console login are in the window, with the USB keyboard. The terminal gets a
#   copy of the firmware text and no login. The firmware boot menu reads the terminal.
# guest_hwcursor=true: QEMU draws the mouse pointer into the screen, so it does not jump.
# HP-UX takes 10.0.2.15 from QEMU's DHCP (hostname hpux11v3).
rom= con=
if [ "$CONSOLE" = graphics ]; then
	rom=,romfile=hppa-sti-radeon.rom
	con="-fw_cfg opt/console,string=graphics"
fi
exec "$q" \
	-name "HP-UX 11i v3 PA-RISC A400" \
	-M A400,pluto=on -smp 1 -m 2048 \
	-accel tcg,thread=multi,tb-size=512 \
	-boot c -boot menu=off \
	-drive if=scsi,bus=0,index=6,file=hpux11v3.qcow2,format=qcow2 \
	-nic user,model=tulip,hostfwd=tcp:127.0.0.1:11131-10.0.2.15:23 \
	-device ati-vga,model=rv200,x-linear-aper-size=0x4000000,guest_hwcursor=true$rom \
	-serial mon:stdio $con \
	"$@"
