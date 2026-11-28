BeOS 5 Pro Edition (5.0.3) on QEMU's Be Inc. BeBox (two PowerPC 603e)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
BeOS boots from a 2 GB IDE disk with the optional software (development tools, GNU sources, media) installed. flash.bin is the BeBox boot flash with the boot ROM of the BeOS R4.5 CD, as R5 brings none of its own; BeOS can rewrite it.
The QEMU monitor is in the terminal: change cd0 file.iso puts a CD in the IDE CD-ROM drive.
Network is QEMU user mode NAT on a DEC 21041 in slot 2. In Network preferences select "DEC 21040/21041 based PCI", click Settings, choose Specify settings with IP 10.0.2.15, mask 255.255.255.0 and gateway 10.0.2.2, set DNS 10.0.2.3, then Save and Restart.
Sound plays through the Crystal CS4231A codec into QEMU's default audio output. The VM runs with -icount: the guest's clock follows the instructions it runs, at about the speed of a 133 MHz BeBox. Without it R5's audio output drops every buffer as late and plays silence.
BeOS buttons want the mouse button held a moment; a quick click may be missed.
The kernel debugger talks on serial 4, the "serial3" window of the View menu.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
