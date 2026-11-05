HP-UX 10.20 on QEMU's HP B160L (four PA-7300LC CPUs, 512 MB, Artist graphics, SCSI disk, Tulip network)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root, password root, at the HP VUE login in the QEMU window (1280x1024, about 90 s after the start), telnet localhost 10223 or rsh localhost port 10514.
Installed: the December 1999 Workstation ACE; HP C (ANSI C), C++, FORTRAN 77, Fortran 90 and Pascal; SoftBench; GlancePlus; Netscape Navigator 4.05 and Enterprise Server; Lotus Notes R4; Wabi 2.2; Advanced Server/9000; TN3270. gcc 4.2.2 is in /usr/local/pa11_32/bin, GNU tools (bash, gmake, gtar, gzip, binutils) and more in /usr/local/bin, perl in /usr/contrib/bin.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1. With -display none the X server does not start.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
Network is QEMU user mode NAT on 192.168.1.0/24: HP-UX is 192.168.1.234 (hostname hpux), the host side is 192.168.1.2. HP-UX's default route points at 10.0.2.2, which is not on that net, so each boot logs an error for it in /etc/rc.log and HP-UX reaches only 192.168.1.x.
HP-UX 10.20 refuses the 2026 hardware clock ("time-of-day register time is out-of-range") and goes on from the time of its last shutdown, in January 2020.
Run shutdown -h -y 0 as root. When HP-UX says "System has halted", type quit in the monitor.
macOS: if the binary is blocked, run xattr -cr . in this folder.

QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-hppa --version names the commit.
