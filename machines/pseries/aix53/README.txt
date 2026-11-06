AIX 5.3 (5300-12-05) on QEMU's pSeries (POWER7 in POWER6 mode, ATI ES1000 graphics, USB keyboard and mouse)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root (no password) at the CDE login in the QEMU window (1280x1024), the serial console in the terminal, telnet localhost 5323 or rsh localhost port 53514.
GNU tools are RPMs from perzl.org in /opt/freeware, linked into /usr/bin: gcc and g++ 4.8.4, bash 5.0, gmake 4.3, gdb 7.12, gtar, gzip, bzip2, xz, zip, unzip, less, autoconf, wget and curl. AIX's own make, tar, m4, patch, diff and cpp stay the default; the GNU ones are in /opt/freeware/bin.
Screen size: as root run chdev -l ati0 -a monitor_type=VESA -a VESA=1024x768@60Hz -P, then shutdown -Fr. AIX offers 640x480, 800x600, 1024x768 and 1280x1024, at 60 or 85 Hz.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
The firmware and the AIX boot banner show in the QEMU window and use the USB keyboard. The small second window is the operator panel with the progress codes.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The terminal holds the serial console and the QEMU monitor: Ctrl-A c switches between them, Ctrl-A x quits.
The CD-ROM is empty: change cd0 file.iso in the monitor.
Network is QEMU user mode NAT; AIX is 10.0.2.15 with gateway 10.0.2.2 and DNS 10.0.2.3.
Run shutdown -F: QEMU quits when AIX has halted.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc64" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc64 --version names the commit.
