MorphOS 3.21 on QEMU's Genesi/bPlan Pegasos II (MPC7457 G4, 1 GB, 64 MB Radeon 9250 in the AGP slot, IDE disk, VIA Rhine-II Ethernet, VIA AC97 sound)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
There is no Pegasos II firmware here: QEMU's Virtual Open Firmware starts boot.img, MorphOS's boot image, which boots MorphOS from the disk. The Ambient desktop comes up in under a minute at 1920x1080; Prefs/Screens changes the mode.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
CD-ROM: in the monitor, change cd0 file.iso. It mounts as CD0.
Network is QEMU user mode NAT. eth1 (via_rhinepci.device) gets its address by DHCP; ping 10.0.2.2 reaches the host side. eth0 (mv6436x_eth.device) is the board's Marvell Gigabit Ethernet, on its own NAT: tick it in Network prefs instead of eth1 to use it (the prefs take one of the two).
Network prefs: Use and Save only apply what was changed. On a fresh system, change the interface (for example to Static and back to DHCP) before Use.
Sound: AHI with pegasos.audio. C:Play plays a sample, e.g. Play SYS:Games/Diamonds/Sfx/gameend.wav.
Graphics: the Radeon driver draws with the command processor through its PIO path. 3D works through TinyGL, for example the 3D screen blankers in Prefs/Blanker (Atlantis, Gears, GLMatrix). "Radeon: Failed to allocate CCE DMA memory" on the serial port is normal: MorphOS only does Radeon DMA on PCI Express cards or behind Apple's AGP bridges, and a real Pegasos II has neither.
Shut down: Ambient menu, Shut Down, Shutdown. The window closes when MorphOS is done. Reboot from the same menu works.
MorphOS is not free software: see morphos-team.net for its license and registration.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
Copyright (c) 2026 Natalia Portillo <claunia@claunia.com> for this kit.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
