SunOS 4.1.4 on QEMU's SPARCstation 5 (microSPARC II, 256 MB, cg3 graphics, SCSI disk, Lance network)

Run ./run.sh (macOS 26+ arm64, Linux x86_64/aarch64 with glibc 2.34+ and X11/XWayland) or run.cmd (Windows 10+ x64).
Log in as root, no password, at the console in the QEMU window (about 30 s after the start), telnet localhost 41423 or rsh localhost port 41414. Telnet and rsh let root in without a password. Both ports listen on this computer only.
Type /usr/openwin/bin/openwin for OpenWindows 3, or sunview for SunView. The screen is 1280x1024. RES=1152x900 ./run.sh gives another size, from 640x480 to 3200x1800 with a width that 8 divides (Windows: set RES=1152x900, then run.cmd). The text console uses the top part of a big screen.
Installed: Sun C (cc); gcc 3.0.4, GNU make 3.82, bash 3.2, gawk 3.1.6, GNU tar, gzip and binutils 2.14 in /usr/local/bin; Lotus 1-2-3 1.2; Netscape 1.0 and Navigator 4.61; NCSA Mosaic 2.7b4; the SunView games 2048, bubble, flap, mines, snake, sokoban and tetris in /usr/local/bin, with their sources in /home/games; Micropolis for SunView in /usr/simcity-sunview; Altsys Virtuoso 1.0, a NeWS drawing program from 1993, in /usr/virtuoso.
Apps: start them in a cmdtool under OpenWindows (root's shell is csh). 1-2-3: setenv WINDOW_MGR olwm; set path=(/usr/lotus/123.v12/sun4sol1/bin $path); 123. Navigator 4.61: setenv XNLSPATH /usr/netscape/nls; /usr/netscape/netscape. Netscape 1.0: /usr/netscape1/netscape. Mosaic: /usr/mosaic/Mosaic-sun-2.7b4. Virtuoso: virtuoso. To open a sample drawing, click Open..., type /usr/virtuoso/SampleArt/Earth.vrt in the File field and press Return (more drawings are in that folder). SunOS has no name server set up, so the browsers need addresses on 192.168.1.x.
NeWS PostScript: psh /home/NeWS/hello.ps in a cmdtool opens a Hello World window, with comments in the file. Click the window to close it.
Games: they are SunView programs. Type sunview at the console, then 2048, tetris and so on in its shelltool. Under OpenWindows they start too, with wrong colours.
Micropolis, the city game, is a SunView program too: type micropolis in the shelltool. micropolis -mono plays in black and white. Source: https://github.com/tenox7/micropolis-sunview. QEMU plays no sound for SunOS.
Also on the disk, but they did not start when this kit was tested: Lucid Energize 2.5 (/usr/energize: it wants its ObjectStore servers), FrameMaker 2.1.1 (/home/framemaker: font database error), SimCity for HyperLook (/usr/simcity-hyperlook: no window comes up) and battalion (/usr/battalion: a Solaris 2 program).
Click in the QEMU window to grab the mouse; Ctrl-Alt-G (Ctrl-Option-G on a Mac) lets it go. Right after OpenWindows or SunView starts, the pointer stands still for the first moment of mouse movement: SunOS is checking the mouse's line speed.
CD-ROM: CD=/full/path/file.iso ./run.sh puts a CD in a drive at SCSI target 6. In SunOS: mount -rt hsfs /dev/sr0 /cdrom.
Extra QEMU options are passed through, e.g. ./run.sh -display vnc=127.0.0.1:1.
The terminal holds the QEMU monitor. quit there stops QEMU at once.
Network is QEMU user mode NAT on 192.168.1.0/24: SunOS is 192.168.1.123 (hostname solqemux), the host side is 192.168.1.10. SunOS has no default route, so it reaches only 192.168.1.x.
An idle SunOS uses a few percent of one host core: QEMU halts the CPU in SunOS's idle loop (-M SS-5,sunos-idle=off turns that off).
Run shutdown -h now as root. QEMU exits when SunOS has halted.
macOS: if the binary is blocked, run xattr -cr . in this folder.

AI agents (macOS, Linux): in this folder, claude mcp add lemu -s user -- "$PWD/bin/macos-arm64/qemu-system-sparc" -mcp-bridge (on Linux bin/linux-x86_64 or bin/linux-aarch64) lets an agent such as Claude Code see the screen and use the keyboard, mouse and serial ports of the running VM. See MCP.md in the QEMU source.
QEMU source: https://github.com/tenox7/lemu branch master (GPLv2, see bin/COPYING). qemu-system-sparc --version names the commit.
