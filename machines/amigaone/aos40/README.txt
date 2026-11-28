AmigaOS 4.0 on QEMU's AmigaOne (MPC7457 G4, 512 MB, 64 MB Radeon 7000, IDE disk, 3Com 3C920 Ethernet, VIA AC97 sound)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
The AmigaOne starts U-Boot (fw/u-boot-amigaone.bin, built from source), which counts down 5 seconds and runs the second level booter from the disk. It counts down again, and in under a minute the Workbench comes up at 800x600.
Press a key other than Space during the first count to stop at the U-Boot prompt (]); boota boots from there.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
CD-ROM: in the monitor, change cd0 file.iso. U-Boot tries the CD before the disk, so a bootable AmigaOS CD in the drive boots instead of the disk; eject cd0 to boot from the disk again.
Network is QEMU user mode NAT. The interface eth0 (eth3com.device) gets its address by DHCP; ping 10.0.2.2 reaches the host side.
Sound: AHI with via-ac97.audio, unit 0 set to VIA-AC97:HiFi 16 bit stereo. Prefs/AHI plays a test sound. Recording works too, e.g. copy AUDIO:B/16/C/2/F/22050/S/2 RAM:rec.raw records 2 seconds.
The installer does not copy the AC97 driver: on a new install, copy Contrib/AHI/VIA-AC97/via-ac97.audio from the CD to DEVS:AHI/ and Contrib/AHI/VIA-AC97/VIA-AC97 to DEVS:AudioModes/, then choose the mode in Prefs/AHI.
Graphics: ATIRadeon.chip; Warp3D through W3D_Radeon (GLQuake and Quake III Arena from Aminet run).
Shut down: AmigaOS 4.0 has no power off, so close the window or type quit in the terminal when the disk is idle.
AmigaOS 4.0 is not free software: see hyperion-entertainment.com for its license.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit. The U-Boot source is the patch in machines/amigaone/firmware there (GPLv2).
Copyright (c) 2026 Natalia Portillo <claunia@claunia.com> for this kit.
