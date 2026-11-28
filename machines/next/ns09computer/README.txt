NeXTSTEP 0.9 on the NeXT Computer (68030 at 25 MHz, 68882, 16 MB, 1120x832 screen with 4 greys, SCSI disk, Ethernet, DSP56001)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
The machine starts from NeXT's ROM 1.2 (Rev_1.2.bin), which boots the disk; within two minutes the desktop comes up, logged in as me.
The disk is an installation made with the Previous emulator.
Shells: the Terminal application. telnet localhost 32093 reaches port 23, but this release does not answer there.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go. The NeXT pointer speeds up a lot with fast moves.
Keys: the Command keys are the host's Windows keys (Command on a Mac); Alt (Option) is NeXT's Alternate.
Sound plays on the host's default sound output.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1.
The terminal holds the QEMU monitor. quit there stops QEMU at once: shut the guest down first, or its disk may need a check at the next boot.
Network is QEMU user mode NAT: the guest gets 10.0.2.15 and the name next, and QEMU answers its bootparam, NetInfo and time requests. -M next-computer,nfs-share=/some/folder shares a host folder, which the guest mounts on /Net/nfs.
Shut down: Log Out in the main menu, then Power at the login window; or as root, halt. Then quit in the monitor if QEMU is still running.
nvram.bin holds the NVRAM: the boot command, the Ethernet address, volume and brightness.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-m68k" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-m68k --version names the commit.
