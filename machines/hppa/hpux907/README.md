# HP-UX 9.07 on the HP 9000/715

**TL;DR** HP-UX 9.07 runs on `-machine 715` the same way as 9.05
([../hpux905/README.md](../hpux905/README.md)), with no further QEMU change: the graphics
console, HP VUE 3.0, the SCSI disk, the LAN and snapshots work. The idle halt finds the
9.07 kernel's idle loop, also after the patch bundle, and `hpux-x11-pace` covers its X server.
An idle guest at the VUE desktop uses about 5% of a host core.

## Run

[run.sh](run.sh) and [run.cmd](run.cmd) start the kit `hpux907-715-lemu` (see
[README.txt](README.txt)); [../release/mkrelease.sh](../release/mkrelease.sh) packs it:
`mkrelease.sh hpux907 VMDIR STAGE/hppa OUT.tar.lz`, with the disk in `VMDIR/hpux907.qcow2`
(HP-UX 9.07 on SCSI ID 6). The pack leaves the disk's snapshots out. The kit forwards telnet
to localhost 9723 and ftp to 9721, so it can run next to the 9.05 kit (9023, 9021). Log in
as `root` at the VUE login (about 90 s after the start); root has no password.

## Install

The steps of 9.05, with the HP-UX 9.07 Install and Core OS discs for the Series 700. The Core
disc loads 176 filesets (156 MB; 9.05 has 165) in about 2 minutes. The patch bundle
XSW700GR0907 (A.B9.07.33, 131 patches) comes from the same patch CD as the 9.05 one;
`patch_install` rebuilds the kernel and reboots.

## Notes

- **Two-user licence.** `uname -a` says "two-user license": 2 logins at a time, and the VUE
  login is one of them. With a serial login (for the MCP `serial-run`) and a telnet login
  open, VUE refuses with "Maximum number of users already logged in".
- **Sound.** The LASI Harmony audio is only there for the firmware and the kernel: it plays
  nothing. Writes to `/dev/audio` stop for good once its buffer is full (8 KB go through,
  64 KB do not). A program that plays sound waits for ever: the kit's Maelstrom wrapper gives
  its sound helper a device that does not exist (`AUDIODEV=/dev/no-sound`), so the game runs
  silent. HP's audio server `Aserver` exits at boot ("cannot communicate with llbd").
- **itemap.** At boot `/etc/itemap` says "ioctl GCITE_MAPROW failed on row 133". It is
  harmless: every key of the text console came out right in single-user mode.
- HP-UX 9.07 has no `/dev/zero`.
