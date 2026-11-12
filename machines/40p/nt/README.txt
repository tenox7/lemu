Windows NT 4.0 for PowerPC on QEMU's IBM 40P (PReP)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
NT logs on automatically as Administrator (password: password). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
Mouse: the vmmouse driver (LEMU machines/vmmouse) is installed, so the pointer moves in and out of the window without a grab, and the wheel scrolls.
The QEMU monitor is in the terminal: sendkey ctrl-alt-delete, change cd0 file.iso, change fd0 file.img.
Network is QEMU user mode NAT on an ISA NE2000.
Compilers: Visual C++ 4.1 for PowerPC (cl 10.10) in C:\MSDEV and the Win32 SDK of August 1996 in C:\MSTOOLS. cl and nmake work in any Command Prompt and over rsh.
DDK: the Windows NT 4.0 DDK (build 1381) is in C:\DDK. To build a driver: Start, Programs, Windows NT DDK (Common), Free Build Environment, then cd to a folder with a sources file and type build -cef. The driver lands in C:\DDK\lib\ppc\free.
DDK from rsh or a batch job: first type set TMP=C:\TEMP, set TEMP=C:\TEMP and call C:\DDK\bin\setenv.bat C:\DDK free.
rsh: the ntrshd service (rsh and rcp server, github.com/tenox7/ntrshd) starts at boot and runs commands as Administrator. It is unauthenticated: it asks for no password, so anyone who can reach its port can run any command as Administrator. 127.0.0.1:46514 on this computer goes to it, e.g. rsh-mcp (github.com/tenox7/rsh-mcp) with host 127.0.0.1 and port 46514. 127.0.0.1:46389 goes to port 3389 (RDP) in NT, for an RDP server you install there. Both listen on 127.0.0.1 only, so other computers can't reach them.
QEMU halts the CPU while NT is idle, so an idle VM uses about 2% of a host core.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
