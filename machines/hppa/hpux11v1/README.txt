HP-UX 11i v1 (B.11.11) on QEMU's HP B160L (four PA-7300LC CPUs, 512 MB, Artist graphics, SCSI disk, Tulip network)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root, password root, at the CDE login in the QEMU window (1280x1024, about 70 s after the start), or telnet localhost 11123.
ssh needs two options for HP-UX's old Secure Shell: ssh -p 11122 -o HostKeyAlgorithms=+ssh-rsa -o KexAlgorithms=+diffie-hellman-group14-sha1 root@localhost (scp -P 11122 with the same options).
Installed: HP-UX 11i v1 with the Required Patch Bundle of June 2003 and the Hardware Enablement bundle of December 2006, CDE, GlancePlus, Secure Shell, Perl 5.8.8, Java 1.4, Firefox 1.5 and Mozilla 1.7. gcc 4.0.4, gmake, gtar, bash, ncftp and TenoxVNC are in /usr/local/bin.
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1. With -display none the CDE login still runs, out of sight: use telnet or ssh.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
Network is QEMU user mode NAT on 192.168.1.0/24: HP-UX gets 192.168.1.109 (hostname hpux) from QEMU's DHCP, the host side is 192.168.1.1 and DNS is 192.168.1.3. HP-UX looks up host names in /etc/hosts only (see /etc/nsswitch.conf).
Each boot reports one error, in /etc/rc.log: the iCAP agent (S602icod) finds no CIM server. It does no harm.
Run shutdown -h -y 0 as root. When HP-UX says "System has halted", type quit in the monitor.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-hppa" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-hppa --version names the commit.
