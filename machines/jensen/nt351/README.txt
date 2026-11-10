Windows NT 3.51 Workstation (build 1057, SP5) for DEC Alpha on QEMU's DECpc AXP 150 (Jensen)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
NT logs on automatically as Administrator (password: password). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The firmware takes about a minute to show its boot menu, then boots NT by itself.
Mouse: no vmmouse driver, so QEMU grabs the mouse when you click in the window. Ctrl-Alt-G lets it go.
The screen is 1280x1024 with 65536 colours (S3 86C928, 4 MB).
C: is the 10 MB FAT system partition, NT is in D:\WINNT35 (NTFS), the CD-ROM is E:.
Compilers: Visual C++ 4.1 for Alpha (cl 10.10) in D:\MSDEV and the Win32 SDK of August 1996 in D:\MSTOOLS. cl and nmake work in any new Command Prompt.
APPVER=3.51 makes the SDK's win32.mak build programs for NT 3.51. The SDK tools need MSVCRT.DLL 4.20 and MFC42.DLL: NT 3.51 has neither, so both are in D:\MSTOOLS\BIN.
Developer Studio asks for the VC++ CD when it starts (the help is on that CD): press Cancel.
QEMU halts the CPU while NT is idle, so an idle VM uses about 3% of a host core.
The clock runs 28 years behind (the firmware rejects current years). Network is QEMU user mode NAT (NE2000, DHCP, 10.0.2.15).
rsh: the ntrshd service (rsh and rcp server, github.com/tenox7/ntrshd) starts at boot and runs commands as Administrator. 127.0.0.1:37514 on this computer goes to it, e.g. rsh-mcp (github.com/tenox7/rsh-mcp) with host 127.0.0.1 and port 37514. 127.0.0.1:37389 goes to port 3389 (RDP) in NT, for an RDP server you install there.
QEMU monitor: Ctrl-Alt-2 (back with Ctrl-Alt-1). CD-ROM and floppy are empty: change cd0 file.iso, change fd0 file.img.
Send Ctrl-Alt-Del from the monitor with: sendkey ctrl-alt-delete
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-alpha" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-alpha --version names the commit.
