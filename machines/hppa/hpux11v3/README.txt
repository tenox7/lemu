HP-UX 11i v3 (B.11.31) on QEMU's HP A400 with a zx1 style bus adapter (one PA-8700 CPU, 2 GB, ATI Radeon 7500 PCI graphics, SCSI disk, Tulip network, USB keyboard and mouse)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root, no password, at the CDE login in the QEMU window (1280x1024, about 4 min after the start), on the console in the terminal, or telnet localhost 11131.
The terminal is the serial console: firmware text, HP-UX boot messages and a console login. The QEMU window stays black until X starts. Ctrl-A c switches to the QEMU monitor and back. Ctrl-A x stops QEMU at once.
CONSOLE=graphics ./run.sh (on Windows: set CONSOLE=graphics, then run.cmd) makes the Radeon the system console: firmware text, HP-UX boot messages and the console login are in the QEMU window, with the USB keyboard, and CDE starts as before. The terminal then gets only a copy of the firmware text; the firmware boot menu still reads the terminal.
Boot messages "SeaBIOS: Unimplemented PDC_PAT_COMPLEX function ..." are expected: the firmware does not have these calls yet, and HP-UX goes on without them.
Installed: HP-UX 11i v3 Mission Critical Operating Environment (MCOE) of June 2007, with CDE. GNU tools: gcc/g++/gfortran 4.0.2, gdb 5.5 (HP WDB) and bzip2 in /opt/TWWfsw/bin; bash 5.0, gmake 4.2.1, tar 1.29, sed, grep, gawk, less, find, diff, patch, gzip, lzip, binutils and flex in /usr/local/bin; coreutils 8.27 in /usr/local/coreutils/bin. These folders are at the end of the PATH, so the HP-UX tar, sed, find, ls and others come first.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1. With -display none the CDE login still runs, out of sight: use the console or telnet.
Network is QEMU user mode NAT. HP-UX gets 10.0.2.15 by DHCP (hostname hpux11v3), the gateway 10.0.2.2 (the host) and the DNS server 10.0.2.3.
To halt, run reboot -h as root. When HP-UX says "System has halted", press Ctrl-A x.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-hppa" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-hppa --version names the commit. Notes on HP-UX 11i v3 in QEMU: machines/hppa/hpux64/README.md there.
