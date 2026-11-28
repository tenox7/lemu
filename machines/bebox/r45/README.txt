BeOS Release 4.5 on QEMU's Be Inc. BeBox (two PowerPC 603e)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
BeOS boots from an IDE disk. flash.bin is the BeBox boot flash with the boot ROM of the BeOS R4.5 CD; BeOS can rewrite it.
The QEMU monitor is in the terminal: change cd0 file.iso puts a CD in the IDE CD-ROM drive.
Network is QEMU user mode NAT on a DEC 21041 in slot 2. In Network preferences add "DEC 21040/21041 based adapter (PCI)" with IP 10.0.2.15, mask 255.255.255.0, router 10.0.2.2 and DNS 10.0.2.3.
Sound plays through the Crystal CS4231A codec (the BeBox's AD1848 sound) into QEMU's default audio output.
BeOS buttons want the mouse button held a moment; a quick click may be missed.
The kernel debugger talks on serial 4, the "serial3" window of the View menu.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
