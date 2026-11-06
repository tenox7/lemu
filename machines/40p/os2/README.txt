OS/2 Warp PowerPC Edition on QEMU's IBM 40P (PReP)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+, X11/XWayland and nc) or run.cmd (Windows 10+ x64).
It boots through the bootloader on the OS/2 CD, which the run scripts answer over COM1 (os2-com1.log), so the firmware logo stays up for about half a minute.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1. The QEMU monitor is in the terminal.
QEMU halts the CPU while OS/2 is idle, so an idle VM uses 2-6% of a host core.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
