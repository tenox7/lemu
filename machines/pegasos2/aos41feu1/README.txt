AmigaOS 4.1 Final Edition Update 1 on QEMU's Genesi/bPlan Pegasos II (MPC7457 G4, 1 GB, 64 MB Radeon 9250 in the AGP slot, IDE disk, VIA Rhine-II Ethernet, VIA AC97 sound)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
The firmware is bPlan's SmartFirmware in flash.bin, the board's flash, which also holds its NVRAM. After a second it boots amigaboot.of from the BOOT partition (hd:0), and AmigaOS comes up in about a minute at 1024x768. The firmware's console is the serial port.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
CD-ROM: in the monitor, change cd0 file.iso. It mounts as CD0. A bootable AmigaOS CD in the drive boots instead of the disk.
Disk: BOOT (64 MB, FFS, holds amigaboot.of) and DH0 System (FFS2), made with Media Toolbox and the AmigaOS installer.
Network is QEMU user mode NAT. The VIA Rhine II gets its address by DHCP (DEVS:NetInterfaces/VIA-RHINE); ping 10.0.2.2 reaches the host side. The board's Marvell Gigabit Ethernet is there too, on its own NAT, but AmigaOS 4.1 Final Edition Update 1 has no driver for it.
Sound: AHI with via-ac97.audio, VIA-AC97:HiFi 16 bit stereo++ (Prefs/AHI, Play a test sound).
Graphics: ATIRadeon.chip, with the AmigaOS 4.1 Final Edition Update 1 compositing; Warp3D through W3D_R200.
Firmware prompt: Esc on the serial port in that second stops at the "ok" prompt. boot hd:0 amigaboot.of boots AmigaOS from there; setenv and printenv change and show the NVRAM, which flash.bin keeps.
Shut down: AmigaOS has no shutdown; close QEMU once the disk is quiet. Its "reboot" closes QEMU too.
Clock: the VT8231's battery-backed clock starts at the host's local time. The kit's SYS:Kickstart/battclock.resource.kmod is patched to find it: the 4.1 Final Edition one only looks for the VIA 686 and the ATI SB600, so on a real Pegasos II too the date starts at 1978.
AmigaOS 4.1 Final Edition Update 1 is not free software: see hyperion-entertainment.com for its license. SmartFirmware is (c) CodeGen and bPlan, from bPlan's Pegasos II firmware update.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
Copyright (c) 2026 Natalia Portillo <claunia@claunia.com> for this kit.
