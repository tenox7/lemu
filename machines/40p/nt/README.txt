Windows NT 4.0 for PowerPC on QEMU's IBM 40P (PReP)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
NT logs on automatically as Administrator (password: password). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
Mouse: the vmmouse driver (LEMU machines/vmmouse) is installed, so the pointer moves in and out of the window without a grab, and the wheel scrolls.
The QEMU monitor is in the terminal: sendkey ctrl-alt-delete, change cd0 file.iso, change fd0 file.img.
Network is QEMU user mode NAT on an ISA NE2000.
rsh: the ntrshd service (rsh and rcp server, github.com/tenox7/ntrshd) starts at boot and runs commands as Administrator. 127.0.0.1:46514 on this computer goes to it, e.g. rsh-mcp (github.com/tenox7/rsh-mcp) with host 127.0.0.1 and port 46514. 127.0.0.1:46389 goes to port 3389 (RDP) in NT, for an RDP server you install there.
QEMU halts the CPU while NT is idle, so an idle VM uses about 2% of a host core.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
