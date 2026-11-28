AmigaOS 4.1 on QEMU's ACube Sam440ep (PowerPC 440EP, 512 MB, 64 MB Radeon Mobility M9, SiI3114 SATA, USB keyboard and mouse, on-chip EMAC Ethernet, CS4281 sound)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
The Sam440ep starts U-Boot (fw/u-boot-sam440ep.bin, built from source), which runs the Radeon's VGA BIOS (fw/vgabios-ati.bin), shows its boot menu and boots the disk after 5 seconds. In about a minute the Workbench comes up at 1024x768.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
CD-ROM: in the monitor, change cd0 file.iso.
Network is QEMU user mode NAT. The interface EMAC0 (ppc440ep_eth.device, the 440EP's first EMAC) gets its address by DHCP; ping 10.0.2.2 reaches the host side. U-Boot saves the MAC addresses in its environment at each start, where AmigaOS reads them. A 3Com 3C905C instead: add -nic user,model=3c905c and use SYS:Storage/NetInterfaces/ETH3COM.
Sound: AHI with cs4281.audio, unit 0 set to the CS4281 HiFi 16 bit stereo mode. Prefs/AHI plays a test sound.
Graphics: ATIRadeon.chip, with the AmigaOS 4.1 compositing; Warp3D through W3D_R200.
The disk writes are slow: the file system flushes the disk after almost every write, so copying many files (an install) takes a long time.
Shut down: close the window or type quit in the terminal when the disk is idle.
AmigaOS 4.1 is not free software: see hyperion-entertainment.com for its license.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit. The U-Boot source is ACube's u-boot-2015.d with the patch in machines/sam440ep/firmware there (GPLv2).
Copyright (c) 2026 Natalia Portillo <claunia@claunia.com> for this kit.
