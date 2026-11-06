AIX 6.1 (6100-09-00) on QEMU's pSeries (POWER7 in POWER6 mode, ATI ES1000 graphics, USB keyboard and mouse)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root (no password) at the CDE login in the QEMU window (1280x1024), telnet localhost 6123 or rsh localhost port 61514.
GNU tools are RPMs from perzl.org in /opt/freeware, linked into /usr/bin: gcc and g++ 4.9.3, bash 5.0, gmake 4.3, gdb 7.12, gtar, gzip, bzip2, xz, zip, unzip, less, autoconf, wget and curl. AIX's own make, tar, m4, patch, diff and cpp stay the default; the GNU ones are in /opt/freeware/bin.
Screen size: as root run chdev -l ati0 -a monitor_type=VESA -a VESA=1024x768@60Hz -P, then shutdown -Fr. AIX offers 640x480, 800x600, 1024x768 and 1280x1024, at 60 or 85 Hz.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Everything shows in the QEMU window and uses the USB keyboard: the firmware, the AIX boot and the AIX console. There is no serial console. The small second window is the operator panel with the progress codes.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
The CD-ROM is empty: change cd0 file.iso in the monitor.
Network is QEMU user mode NAT; AIX is 10.0.2.15 with gateway 10.0.2.2 and DNS 10.0.2.3.
The time zone is US Pacific (PST8PDT). Change it with chtz, then reboot.
Run shutdown -F: QEMU quits when AIX has halted.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc64" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc64 --version names the commit.
