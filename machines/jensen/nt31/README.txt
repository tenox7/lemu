Windows NT 3.1 (build 511, SP3) for DEC Alpha on QEMU's DECpc AXP 150 (Jensen)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
NT logs on automatically as Administrator (password: password). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
Mouse: the vmmouse driver (LEMU machines/vmmouse) is installed, so the pointer moves in and out of the window without a grab.
Press a key during the firmware's 5 s countdown for the ARC menus. Its "NVRAM inconsistent" warning is harmless.
NT's boot screen stays blank until the desktop appears.
QEMU halts the CPU while NT is idle, so an idle VM uses about 2% of a host core.
The clock runs 28 years behind (the firmware rejects current years). Network is QEMU user mode NAT.
rsh: the ntrshd service (rsh and rcp server, github.com/tenox7/ntrshd) starts at boot and runs commands as Administrator. It is unauthenticated: it asks for no password, so anyone who can reach its port can run any command as Administrator. 127.0.0.1:33514 on this computer goes to it, e.g. rsh-mcp (github.com/tenox7/rsh-mcp) with host 127.0.0.1 and port 33514. 127.0.0.1:33389 goes to port 3389 (RDP) in NT, for an RDP server you install there. Both listen on 127.0.0.1 only, so other computers can't reach them.
QEMU monitor: Ctrl-Alt-2 (back with Ctrl-Alt-1). CD-ROM and floppy are empty: change cd0 file.iso, change fd0 file.img.
Send Ctrl-Alt-Del from the monitor with: sendkey ctrl-alt-delete
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-alpha" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-alpha --version names the commit.
