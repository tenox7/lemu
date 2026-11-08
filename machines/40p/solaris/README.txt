Solaris 2.5.1 for PowerPC on QEMU's IBM 40P (PReP)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root without a password. Extra QEMU options are passed through, e.g. ./run.sh -display vnc=:1.
The QEMU monitor is in the terminal. CD-ROM is empty: change cd0 file.iso
Network is QEMU user mode NAT: pcn0 is 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3.
Installed: Sun WorkShop Compilers 4.2 (cc, CC) with make, the system headers and libm (math.h).
GNU tools in /usr/local/bin: bash 3.2.57, gmake 3.81, grep 2.5.4, gzip 1.3.12, gtar 1.15.1, GCC 2.95.3 (gcc, g++) and binutils 2.16.1. They come last in PATH, so plain grep, as and ld are the Solaris ones; gcc runs the GNU as and ld itself.
Sun cc links print "unsupported # of bits for relocation" lines from the 2.5.1 linker: the program still works, but C++ exceptions do not. With g++ they do.
gcc does not link -lw -lintl as Sun cc does: add them for gettext or wide characters.
Wabi 2.1 is a November 1995 evaluation copy: with today's date it says it has expired.
NCSA Mosaic 2.7b5 web browser: right-click the desktop, then Programs > Mosaic Web Browser (or run /usr/local/bin/Mosaic). It has no HTTPS, so only http:// sites work, such as http://68k.news/ and http://theoldnet.com/.
QEMU halts the CPU while Solaris is idle, so an idle VM uses about 3% of a host core.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc --version names the commit.
