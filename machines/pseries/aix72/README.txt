AIX 7.2 (7200-05-03) on QEMU's pSeries (POWER8 logical partition, virtio SCSI disk)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+) or run.cmd (Windows 10+ x64).
AIX runs on the serial console in the terminal, which also holds the QEMU monitor: Ctrl-A c switches between them, Ctrl-A x quits.
Log in as root (no password) at the console login prompt.
The VM has 8 GB of memory. The first boot after a restore takes a few minutes.
Extra QEMU options are passed through.
The CD-ROM is empty: change cd0 file.iso in the monitor.
Telnet localhost 7223 and ssh localhost 7222 reach AIX once its network is set up (QEMU user mode NAT: 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3).
Run shutdown -F and wait for "Halt completed" before quitting QEMU.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-ppc64" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-ppc64 --version names the commit.
