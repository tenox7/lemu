HP-UX 9.05 on QEMU's HP 9000/715 (one PA-7100LC CPU, 128 MB, Artist graphics, SCSI disk, LASI LAN)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root at the HP VUE login in the QEMU window (about 90 s after the start): root has no password, press Return. Or telnet localhost 9023.
Installed: HP-UX 9.05 with the July 1997 patch bundle XSW700GR0905; HP C (ANSI C, cc -Aa); GNU tools (gcc 3.3.6 and 2.7.2.2, emacs 19.34, bash 3.2.57, gmake 3.81, gzip and more); Netscape Communicator 4.04; NCSA Mosaic 2.7b4; WordPerfect 6.0; FrameMaker 5.1.1; CorelDRAW 3.5; Wabi 2.2 (Windows 3.11); SoftWindows 2.0; SoftPC 3.1 (MS-DOS 5.00); MAE 3.0 (Mac OS 7.5); Battalion, SimCity and Maelstrom. C SoftBench is installed, but its tools need licence passwords that are not there.
Apps: in an hpterm or a telnet session type netscape, mosaic, wordperfect, framemaker, coreldraw, wabi, softwindows, mae, battalion, simcity or maelstrom. The program opens on the VUE screen within a minute (at(1) starts it). SoftPC: type /usr/bin/spc in an hpterm.
Netscape: its first start shows the licence dialog: press Accept. HP-UX has no DNS here, so loading the home page (or any address with a host name) gives 2 "unknown host" errors: press OK. Quit it with File > Exit. kill makes it spin; then kill -9 it and rm /.netscape/lock.
FrameMaker's licence wants the hostname hpux9: keep it.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1. With -display none the VUE login still runs, out of sight: use telnet. VUE needs the Artist graphics card: with -machine 715,graphics=off the console repeats "X-server can not be started".
The terminal holds the QEMU monitor. quit there stops QEMU at once. savevm NAME saves the whole running VM into hpux905.qcow2; ./run.sh -loadvm NAME starts from it in seconds.
Network is QEMU user mode NAT: HP-UX is 10.0.2.15 (hostname hpux9), the host side is 10.0.2.2. telnet localhost 9023. ftp localhost 9021 needs a user with a password (root has none: set one with passwd, or use telnet).
HP-UX 9.05 refuses the 2026 hardware clock ("time-of-day register time is out-of-range") and goes on from the time of its last shutdown, in November 1998 (1998 has the calendar of 2026).
The serial port (serial0, HP-UX /dev/tty00) has a root login without a password (TERM=dumb), for scripts.
Run /etc/shutdown -h -y 0 as root. When HP-UX says "Halted, you may now cycle power", type quit in the monitor.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-hppa" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-hppa --version names the commit.
