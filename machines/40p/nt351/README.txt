Windows NT 3.51 Workstation (build 1057, SP5) for PowerPC on QEMU's IBM 40P (PReP)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
The ARC menu boots NT by itself after 30 seconds; Enter boots it at once.
Log on: Ctrl+Alt+Del, user Administrator, blank password. Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
Mouse: no vmmouse driver, so QEMU grabs the mouse when you click in the window. Ctrl-Alt-G lets it go.
The QEMU monitor is in the terminal: sendkey ctrl-alt-delete, change cd0 file.iso (E: in NT), change fd0 file.img.
The screen is 1280x1024 with 65536 colours (Weitek P9100, NT's weitekp9 driver). 16.7 million colours shows wrong colours.
NT is in C:\WINNT35 (NTFS). D: is the 5 MB FAT system partition.
Compilers: Visual C++ 4.1 for PowerPC (cl 10.10) in C:\MSDEV and the Win32 SDK of August 1996 in C:\MSTOOLS. cl and nmake work in any new Command Prompt.
APPVER=3.51 makes the SDK's win32.mak build programs for NT 3.51. The SDK tools need MSVCRT.DLL 4.20 and MFC42.DLL: NT 3.51 has neither, so both are in C:\MSTOOLS\BIN.
Developer Studio asks for the VC++ CD when it starts (the help is on that CD): press Cancel.
The clock is the host's local time. The firmware shows the year as 1926; NT shows the right one.
Network is QEMU user mode NAT on an ISA NE2000 (DHCP, 10.0.2.15).
QEMU halts the CPU while NT is idle, so an idle VM uses about 5% of a host core.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
