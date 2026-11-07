Windows NT 3.51 Workstation (build 1057, SP5) for DEC Alpha on QEMU's DECpc AXP 150 (Jensen)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log on: Ctrl+Alt+Del, user Administrator, blank password. Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The firmware takes about a minute to show its boot menu, then boots NT by itself.
Mouse: no vmmouse driver, so QEMU grabs the mouse when you click in the window. Ctrl-Alt-G lets it go.
The screen is 1280x1024 with 65536 colours (S3 86C928, 4 MB).
C: is the 10 MB FAT system partition, NT is in D:\WINNT35 (NTFS), the CD-ROM is E:.
QEMU halts the CPU while NT is idle, so an idle VM uses about 3% of a host core.
The clock runs 28 years behind (the firmware rejects current years). Network is QEMU user mode NAT (NE2000, DHCP, 10.0.2.15).
QEMU monitor: Ctrl-Alt-2 (back with Ctrl-Alt-1). CD-ROM and floppy are empty: change cd0 file.iso, change fd0 file.img.
Send Ctrl-Alt-Del from the monitor with: sendkey ctrl-alt-delete
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-alpha" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-alpha --version names the commit.
