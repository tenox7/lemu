# A/UX 3.1.1 on the Quadra 800 (q800)

**TL;DR** `aux311/run.sh` boots A/UX 3.1.1 on `-M q800` from Apple's
Quadra 800 ROM. The machine is QEMU's own; LEMU adds the kit and the
release tools.

## Kit

[aux311/run.sh](aux311/run.sh) and [run.cmd](aux311/run.cmd) start the kit
`aux311q800-lemu` (see [README.txt](aux311/README.txt));
[release/mkrelease.sh](release/mkrelease.sh) packs it from the one build of
[../release](../release/README.md):

    sh release/mkrelease.sh aux311 VMDIR STAGE/m68k aux311q800-lemu.tar.lz

`VMDIR` holds the disk `root.img` (raw), the PRAM `pram-aux.img` and
`Quadra800.rom`. The kit gets them as `aux311.qcow2`, `pram.img` and
`Quadra800.rom`.

## Files

| File | Option | What it is |
|---|---|---|
| `Quadra800.rom` | `-bios` | Apple's ROM of the Quadra 800, 1 MB. Not in this source tree: the kit carries it. |
| `pram.img` | `-drive if=mtd` | The PRAM, 512 bytes. It keeps the Mac's settings, such as the start-up disk. |
| `aux311.qcow2` | `scsi-hd,scsi-id=0` | The disk, 2 GB, with an Apple partition map |

The disk holds three partitions:

| Partition | Type | Size | |
|---|---|---|---|
| MacOS | Apple_HFS | 488 MB | Mac OS 7 with A/UX Startup, which boots A/UX |
| Swap | Apple_UNIX_SVR2 | 133 MB | A/UX swap |
| UNIX Root&Usr slice 0 | Apple_UNIX_SVR2 | 1427 MB | `/`, a 4.2 BSD file system: 8 KB blocks, 1 KB fragments |

## Network

- The Ethernet is `-nic ...,model=dp83932` (SONIC). A/UX calls it `ao0`.
- A/UX wants an Apple MAC address (08:00:07:...). QEMU gives the q800
  08:00:07:12:34:56 when no `mac=` is set. On a bridged LAN, give each
  q800 VM its own address with that prefix.
- A/UX keeps its address in `/etc/NETADDRS`
  (`ao0 address broadcast netmask`), its name in `/etc/HOSTNAME`, and its
  name server in `/etc/resolv.conf`. It sets no default route at boot:
  `/usr/etc/route add default GATEWAY 1` adds one until the next boot.
- The kit's A/UX has the fixed address 192.168.1.2, so the kit gives slirp
  that network: `net=192.168.1.0/24,host=192.168.1.10,dns=192.168.1.1`.

## Shutdown

`/etc/shutdown -y -g0 -i0 now` (or Special > Shut Down in the Finder) ends
with "You may now switch off your Macintosh safely". The q800 has no soft
power-off, so QEMU keeps running: quit it then.

## Tested

The kit of 2026-11-14 on an Apple Silicon Mac (the kit of 2026-11-26
is the same, with Archaic in `/usr/local/bin`):

- The Finder comes up about 35 s after QEMU starts, logged in as root.
- Idle, QEMU uses about 2% of one host core: the 68040 is halted 99% of
  the time.
- telnet (port 31123) and rsh (port 31514) answer. After
  `/usr/etc/route add default 192.168.1.1 1`, wget and curl reach the
  Internet.
- MacX starts from the Apple menu (answer OK to its memory question).
  xterm and SimCity (`/usr/SimCity`) then run on it. `/mac/bin/launch`
  from a telnet session fails with error -108: start Mac programs from
  the Finder.
- On Linux x86_64 and aarch64 (`../release/ltest-ppc.sh`), QEMU runs the
  kit until the test stops it.

## Making a kit

1. Copy the VM's disk and PRAM (`cp -c`) and boot the copy with the
   kit's slirp line, never on the LAN. Add `restrict=on`, so that no DNS
   query of the guest leaves the host while it is cleaned. telnetd then
   waits about a minute for its name lookup before the login prompt.
2. Log in with telnet. A/UX has no `/dev/zero`, no `grep -r` and no
   `find -name '*x'` that matches dot files. Clear root's `.sh_history`
   and `.bash_history`, `/etc/wtmp` and `/etc/utmp` (`> file`), core
   files, and the logs of programs that talked to the LAN. Keep only
   `localhost` and the guest's own name (`aux`) in `/etc/hosts`.
   To copy files in with rcp (port 31514), A/UX's rshd needs a name for
   192.168.1.10 in `/etc/hosts` (with `restrict=on` it can't ask DNS):
   add a line for the copy, and take it out again before the shutdown.
3. Shut down as above, quit QEMU, then zero the free space of `/`:
   `python3 -I release/ffszero.py root.img 1272096 --write` (the start
   sector of the root partition; the tool checks the file system first).
   A fill from inside A/UX misses the free fragments of partly used
   blocks, with the old data of deleted files in them.
4. Scan the raw image for the LAN's addresses and names, then pack.

## Sony MD DATA drive

`-device scsi-md` is a Sony MDH-10 (or, with `model=mdm110`, an
MDM-110) MD DATA drive. Mac OS 7.5 with Sony's "MD DATA Installation
Software for Macintosh" 1.81 mounts, reads, writes and formats its discs:

    -drive file=disc.img,if=none,id=md0,format=raw \
    -device scsi-md,scsi-id=3,drive=md0

- **The disc** is a raw image of 70464 blocks of 2048 bytes (144310272
  bytes), the layout of an Aaru dump. A blank disc:
  `qemu-img create -f raw disc.img 144310272`. Format it from the Mac
  with "MD DATA quick format" or "MD DATA safe format".
- **The U-TOC** lives next to the image as `disc.utoc.bin` (the image
  name without its extension), as Aaru keeps it. Without one the drive
  makes the MD DATA U-TOC for a disc with an MDFS volume, and a blank one
  otherwise. `utoc=path` puts it somewhere else.
- **The drivers:** copy "MD Driver", "MD File System" and "File System
  Manager" into the System Folder.
- **Ejecting:** drag the disc to the Trash. The drive opens its tray and
  the driver polls it. Put a disc in with the monitor:
  `change md0 disc.img raw`. A reset puts the last disc back.
- **A read-only image** is a write-protected disc.
- **Trace:** `-trace "scsi_md_*"` logs every command and its sense.

## Floppy drive

The q800 has its SWIM2 and an internal Apple SuperDrive (Sony MFD-75W).
The drive is always there; it starts empty unless a disk is given:

    -drive if=floppy,format=raw,file=disk.img

`index=1` adds a second, external SuperDrive. Change disks with the
`media` tool or the monitor (`change floppy0 disk.img raw`). Eject from
the guest first (Put Away, or `eject /dev/rfloppy0` in A/UX): a Mac
only looks for a new disk once its driver has ejected the old one.

- **The images are raw.** Their size tells the drive what is on them:

  | Size | Media | Layout |
  |---|---|---|
  | 409600 | MF1DD | Mac 400K, GCR, one side |
  | 819200 | MF2DD | Mac 800K, GCR |
  | 368640 | MF1DD | DOS 360K, MFM, one side, 80 tracks |
  | 737280 | MF2DD | DOS 720K, MFM |
  | 1474560 | MF2HD | Mac 1.4 MB or DOS 1.44 MB, MFM |

  Any other size is an unformatted disk: DD up to 819200 bytes, HD
  above. A blank disk is a file of zeros of one of these sizes, which
  the Mac offers to initialize.
- **Formatting resizes the image.** Erasing an 800K disk as DOS 720K
  makes the file 737280 bytes; a One-Sided format makes it 409600. The
  SuperDrive, like the real one, does no GCR on HD media.
- **A read-only image** is a locked disk.
- **The GCR tag bytes** of 400K and 800K sectors are not kept; they read
  back as zeros.
- **Speed** is that of the real drive: data moves at the disk's bit rate.
- **Trace:** `-trace "swim_*"` logs register access, drive commands,
  tracks and layout changes.

### Tested

Copies of the VMs of 2026-11-28, with images made on the host by
hfsutils and mtools. "Copy" is a Finder duplicate of a 300 KB file on
the disk, checked by MD5 on the host afterwards.

| Guest | 1.44M HFS | 800K HFS | 400K HFS | 720K DOS | 1.44M DOS | Format |
|---|---|---|---|---|---|---|
| System 7.1 | | | read, copy | | | 400K→800K erase |
| System 7.1.1 Pro | read, copy | read, copy | | read, copy | | |
| System 7.5 | read, copy | read, copy | | read, copy | | |
| System 7.5.1 | read, copy | read, copy | | read, copy | | |
| System 7.5.3 | read, copy | read, copy | | read, copy | | |
| System 7.5.5 | read, copy, locked | read, copy | | read, copy | read, copy | 1.4 MB, 800K, DOS 720K, DOS 1.44 MB |
| Mac OS 7.6 | read, copy | read, copy | | read, copy | | |
| Mac OS 7.6.1 | read, copy | read, copy | | read, copy | | |
| Mac OS 8.0 | read, copy | read, copy | | read, copy | | |
| Mac OS 8.1 | read, copy | read, copy | | read, copy | | |
| A/UX 3.0.1 | read, copy | read, copy | | | | |
| A/UX 3.1 | read, copy | read, copy | | raw `dd`, `tar` | raw `dd` | One-Sided (400K, MFS) |
| A/UX 3.1.1 | read, copy | read, copy | | | | |

- Swapping disks when the Finder asks for another one works.
- Mac OS can't read a single-sided DOS disk: its driver treats every
  MFM DD disk as two-sided, as on a real Mac. The Finders of System 7.1
  to 8.1 with PC Exchange offer no 400K format for a SuperDrive; the
  A/UX Finder does (One-Sided).
- At start-up the ROM looks at a floppy in the drive and ejects it when
  it does not boot from it. Booting from a floppy is not tested yet.
