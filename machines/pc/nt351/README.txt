Windows NT 3.51 Workstation (build 1057, SP5) for x86 on QEMU's PC (Pentium, i440FX)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
NT logs on automatically as Administrator (password: password). Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The QEMU monitor is in the terminal: sendkey ctrl-alt-delete, change cd0 file.iso (D: in NT), change fd0 file.img.
Mouse: no vmmouse driver, so QEMU grabs the mouse when you click in the window. Ctrl-Alt-G lets it go.
The screen is 1280x1024 with 65536 colours (S3 86C928, 4 MB, NT's own S3 driver). NT is in C:\WINNT35 (NTFS).
Compilers: Visual C++ 4.1 (cl 10.10) in C:\MSDEV and the Win32 SDK of August 1996 in C:\MSTOOLS. cl and nmake work in any new Command Prompt.
APPVER=3.51 makes the SDK's win32.mak build programs for NT 3.51. The SDK tools need MSVCRT.DLL 4.20 and MFC42.DLL: NT 3.51 has neither, so both are in C:\MSTOOLS\BIN.
Developer Studio asks for the VC++ CD when it starts (the help is on that CD): press Cancel.
fw/bios-nt351.bin is SeaBIOS with the E820 memory map and the MP table turned off: NT 3.51 setup crashes with the stock one.
QEMU halts the CPU while NT is idle, so an idle VM uses about 2% of a host core.
The clock is the host's local time. Network is QEMU user mode NAT (NE2000, DHCP, 10.0.2.15).
rsh: the ntrshd service (rsh and rcp server, github.com/tenox7/ntrshd) starts at boot and runs commands as Administrator. 127.0.0.1:35514 on this computer goes to it, e.g. rsh-mcp (github.com/tenox7/rsh-mcp) with host 127.0.0.1 and port 35514. 127.0.0.1:35389 goes to port 3389 (RDP) in NT, for an RDP server you install there.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-i386" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-i386 --version names the commit.
