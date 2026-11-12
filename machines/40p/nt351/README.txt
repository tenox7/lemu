Windows NT 3.51 Workstation (build 1057, SP5) for PowerPC on QEMU's IBM 40P (PReP)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
The ARC menu boots NT by itself after 30 seconds; Enter boots it at once.
NT logs on automatically as Administrator (password: password). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
Mouse: no vmmouse driver, so QEMU grabs the mouse when you click in the window. Ctrl-Alt-G lets it go.
The QEMU monitor is in the terminal: sendkey ctrl-alt-delete, change cd0 file.iso (E: in NT), change fd0 file.img.
The screen is 1280x1024 with 65536 colours (Weitek P9100, NT's weitekp9 driver). 16.7 million colours shows wrong colours.
NT is in C:\WINNT35 (NTFS). D: is the 5 MB FAT system partition.
Compilers: Visual C++ 4.1 for PowerPC (cl 10.10) in C:\MSDEV and the Win32 SDK of August 1996 in C:\MSTOOLS. cl and nmake work in any new Command Prompt.
APPVER=3.51 makes the SDK's win32.mak build programs for NT 3.51. The SDK tools need MSVCRT.DLL 4.20 and MFC42.DLL: NT 3.51 has neither, so both are in C:\MSTOOLS\BIN.
Developer Studio asks for the VC++ CD when it starts (the help is on that CD): press Cancel.
DDK: the Windows NT 3.51 DDK (build 1057) is in C:\DDK. To build a driver, open Program Manager, group Windows NT DDK, Free Build Environment, cd to a folder with a sources file and type build -cef. The driver lands in C:\DDK\lib\ppc\free.
DDK from rsh or a batch job: first type set TMP=C:\TEMP, set TEMP=C:\TEMP and call C:\DDK\bin\setenv.bat C:\DDK free.
The 1995 DDK lacks helpers that Visual C++ 4.1 for PowerPC calls, so nothing linked. Fixed with C:\DDK\lib\ppc\vc41help.lib (5 objects from VC++'s libc.lib) and two lines in setenv.bat: set USING_VC2X=1 and set CRTLIBS=%BASEDIR%\lib\ppc\vc41help.lib.
The clock is the host's local time. The firmware shows the year as 1926; NT shows the right one.
Network is QEMU user mode NAT on an ISA NE2000 (DHCP, 10.0.2.15).
rsh: the ntrshd service (rsh and rcp server, github.com/tenox7/ntrshd) starts at boot and runs commands as Administrator. It is unauthenticated: it asks for no password, so anyone who can reach its port can run any command as Administrator. 127.0.0.1:38514 on this computer goes to it, e.g. rsh-mcp (github.com/tenox7/rsh-mcp) with host 127.0.0.1 and port 38514. 127.0.0.1:38389 goes to port 3389 (RDP) in NT, for an RDP server you install there. Both listen on 127.0.0.1 only, so other computers can't reach them.
QEMU halts the CPU while NT is idle, so an idle VM uses about 5% of a host core.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
