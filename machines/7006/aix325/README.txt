AIX 3.2.5 for the IBM RS/6000 on QEMU's 7006-41T (PowerPC 601, GXT150L graphics)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root (no password) at the xdm login on the graphics console, the serial console in the terminal or telnet localhost 2323.
rsh to localhost port 2514 runs commands as root (no .rhosts needed).
Installed: everything from the AIX 3.2.5 Volume 1 and 2 and AIXwindows 1.2.5 CDs, and the Enhancement 5 (PMP 3251) fixes.
/usr/local has software from the UCLA AIX PDS archive: bash, emacs, gcc, gdb, make, perl, the GNU utilities, less, screen, mc, lynx, wget, ncftp, Mosaic, nedit, xv, rxvt and more.
Mathematica 2.2 is in /usr/local/mathematica: mathematica starts the notebook front end (in X), math the text kernel with Motif graphics windows. Sample notebooks: /usr/local/mathematica/FrontEnd/SampleNotebooks.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
QEMU halts the CPU while AIX is idle, so an idle VM uses 6-15% of a host core.
The operator panel window shows the LED codes; click a key position to turn the key. KEY=service ./run.sh sets it at power-on.
The terminal holds the serial console and the QEMU monitor: Ctrl-A c switches between them, Ctrl-A x quits.
The CD-ROM is empty: change cd0 file.iso in the monitor, then mount -v cdrfs -r /dev/cd0 /mnt in AIX.
Network is QEMU user mode NAT; AIX is 10.0.2.15 with gateway 10.0.2.2 and DNS 10.0.2.3.
Run shutdown -F and wait for "Halt completed" before quitting QEMU.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
