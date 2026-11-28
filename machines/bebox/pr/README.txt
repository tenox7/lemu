BeOS Preview Release on QEMU's Be Inc. BeBox (two PowerPC 603e)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
BeOS boots from a SCSI disk with the optional items installed. flash.bin is the BeBox boot flash with the boot ROM of the Preview Release 2 CD, as the Preview Release CD brings none; BeOS can rewrite it.
The QEMU monitor is in the terminal: change cd0 file.iso puts a CD in the SCSI CD-ROM drive.
Network is QEMU user mode NAT on a DEC 21041 in slot 2. In Network preferences click Add Interface, pick "DEC 21040/21041 based adapter (PCI)" with IP 10.0.2.15 and mask 255.255.255.0, set the router to 10.0.2.2 and the primary DNS server to 10.0.2.3, then Save and Restart.
The Preview Release shows a wrong date, in December 1901; it is from 1997 and does not read today's clock.
Menus want the mouse button held down: press on the menu, move to the item, release.
The kernel debugger talks on serial 4, the "serial3" window of the View menu.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
