Solaris 2.5.1 for PowerPC on QEMU's IBM 40P (PReP)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root without a password. Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The QEMU monitor is in the terminal. CD-ROM is empty: change cd0 file.iso
Network is QEMU user mode NAT: pcn0 is 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3.
Installed: Sun WorkShop Compilers 4.2 (cc, CC) with make and the system headers, and GNU bash 3.2.57 in /usr/local/bin.
Every link prints "unsupported # of bits for relocation" lines from the 2.5.1 linker: the program still works, but C++ exceptions do not.
Wabi 2.1 is a November 1995 evaluation copy: with today's date it says it has expired.
QEMU halts the CPU while Solaris is idle, so an idle VM uses about 3% of a host core.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
