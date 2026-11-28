# NeXT 68k machines

**TL;DR** LEMU emulates the seven 68k NeXT machines, from the 68030 NeXT
Computer to the NeXTstation Turbo Color, with their own ROMs. Every
release from NeXTSTEP 0.8 to OPENSTEP 4.2 boots, and NeXTSTEP 2.2 and 3.3
install from their CDs. `*/run.sh` starts the kits below.

The device models are ports of the [Previous](https://previous.alternative-system.com/)
emulator's, as QOM devices; `hw/m68k/next-cube.c` is the board of all
seven.

## Machines

| Machine | `-M` | CPU | Default RAM | Screen | ROMs |
|---|---|---|---|---|---|
| NeXT Computer | `next-computer` | 68030, 68882 | 16 MB | 1120x832, 4 greys | 64 KB: 0.8 v31, 1.0 v40/v41, 1.1 v42, 1.2 v43 (`Rev_1.2.bin`) |
| NeXTcube | `next-cube` | 68040 | 64 MB | 1120x832, 4 greys | 128 KB: 1.2 v58 to 2.5 v66 (`Rev_2.5_v66.bin`) |
| NeXTstation | `next-station` | 68040 | 32 MB | 1120x832, 4 greys | 128 KB: 1.2 v58 to 2.5 v66 |
| NeXTstation Color | `next-station-color` | 68040 | 32 MB | 1120x832, 4096 colours | 2.2 v63 to 2.5 v66 |
| NeXTcube Turbo | `next-cube-turbo` | 68040 | 64 MB | 1120x832, 4 greys | 3.0 v70 to 3.3 v74 (`Rev_3.3_v74.bin`) |
| NeXTstation Turbo | `next-station-turbo` | 68040 | 32 MB | 1120x832, 4 greys | 3.0 v70 to 3.3 v74 |
| NeXTstation Turbo Color | `next-station-turbo-color` | 68040 | 32 MB | 1120x832, 4096 colours | 3.0 v70 to 3.3 v74 |

- The ROM is not in this tree: give it with `-bios`. Without `-bios` the
  machine looks for the file in brackets in QEMU's data folder.
- `-m` sets the memory, made of four banks as on the real boards: up to
  16 MB on the NeXT Computer, 32 MB on the colour NeXTstation, 64 MB on
  the other 68040 machines and 128 MB on the Turbo machines. The ROM finds
  the banks itself.

## Kits

| Kit (`<name>-lemu.tar.lz`) | Guest | Machine | ROM | Disk | telnet | Starts at |
|---|---|---|---|---|---|---|
| [ns08computer](ns08computer/run.sh) | NeXTSTEP 0.8 | `next-computer` | `Rev_1.2.bin` | `ns08.qcow2` | 32083 | desktop, as me |
| [ns09computer](ns09computer/run.sh) | NeXTSTEP 0.9 | `next-computer` | `Rev_1.2.bin` | `ns09.qcow2` | 32093 | desktop, as me |
| [ns10computer](ns10computer/run.sh) | NeXTSTEP 1.0 | `next-computer` | `Rev_1.2.bin` | `ns10.qcow2` | 32103 | Workspace, as me |
| [ns10acomputer](ns10acomputer/run.sh) | NeXTSTEP 1.0a | `next-computer` | `Rev_1.2.bin` | `ns10a.qcow2` | 32104 | Workspace, as me |
| [ns20cube](ns20cube/run.sh) | NeXTSTEP 2.0 | `next-cube` | `Rev_2.5_v66.bin` | `ns20.qcow2` | 32203 | Workspace, as me |
| [ns21color](ns21color/run.sh) | NeXTSTEP 2.1 | `next-station-color` | `Rev_2.5_v66.bin` | `ns21.qcow2` | 32213 | Workspace, as me |
| [ns22computer](ns22computer/run.sh) | NeXTSTEP 2.2 | `next-computer` | `Rev_1.2.bin` | `ns22.qcow2` | 32223 | Workspace, as me |
| [ns30station](ns30station/run.sh) | NeXTSTEP 3.0 | `next-station` | `Rev_2.5_v66.bin` | `ns30.qcow2` | 32303 | first-boot setup |
| [ns31turbo](ns31turbo/run.sh) | NeXTSTEP 3.1 | `next-station-turbo` | `Rev_3.3_v74.bin` | `ns31.qcow2` | 32313 | first-boot setup |
| [ns32cube](ns32cube/run.sh) | NeXTSTEP 3.2 | `next-cube` | `Rev_2.5_v66.bin` | `ns32.qcow2` | 32320 | login window (root) |
| [ns33station](ns33station/run.sh) | NeXTSTEP 3.3 | `next-station` | `Rev_2.5_v66.bin` | `ns33.qcow2` | 32323 | login window (root) |
| [ns33turbocolor](ns33turbocolor/run.sh) | NeXTSTEP 3.3 | `next-station-turbo-color` | `Rev_3.3_v74.bin` | `ns33.qcow2` | 32333 | login window (root) |
| [os40cubeturbo](os40cubeturbo/run.sh) | OPENSTEP 4.0 | `next-cube-turbo` | `Rev_3.3_v74.bin` | `os40.qcow2` | 32403 | Workspace, as me |
| [os42color](os42color/run.sh) | OPENSTEP 4.2 | `next-station-color` | `Rev_2.5_v66.bin` | `os42.qcow2` | 32423 | login window (root) |

- The disks are installations made with the Previous emulator, but for
  3.0: Previous's 3.0 disk is empty, so ns30station is a fresh install
  from the NeXTSTEP 3.0 installer disk, made in LEMU (below).
- 0.8 and 0.9 do not answer telnet. The others refuse root logins over
  telnet; on 2.2, me logs in, and on 3.x and 4.x, make a user first.

Each kit also has `nvram.bin` (`-drive if=mtd`), the 64 bytes of NVRAM
that the ROM keeps its settings in. [release/mkrelease.sh](release/mkrelease.sh)
packs a kit from the one build of [../release](../release/README.md):

    sh release/mkrelease.sh ns33station VMDIR STAGE/m68k ns33station-lemu.tar.lz

`VMDIR` holds the disk with the kit's name for it (`ns33.qcow2`), the ROM
and, if the VM has one, `nvram.bin`. It is only read.

## Devices

| Device | Where | Notes |
|---|---|---|
| SCSI | `hw/m68k/next-scsi.c` on QEMU's ESP | 53C90A; the 53C90 (no CONF2) on the NeXT Computer. `scsi-hd`, `scsi-cd` |
| DMA | `hw/dma/next-dma.c` | the 12 channels, chaining, the 68030 and Turbo layouts |
| Floppy | `hw/block/next-fdc.c` | 2.88 MB drive on the 040 machines (`-drive if=floppy`) |
| Optical disk | `hw/block/next-mo.c` | the cubes' magneto-optical drives, below |
| Ethernet | `hw/net/mb8795.c` | thin or twisted pair; `-nic user` adds NeXT's network services, below |
| Sound | `hw/audio/next-sound.c` | 16-bit 44.1 kHz out, mu-law in; `-M ...,audiodev=ID` |
| DSP | `hw/audio/next-dsp.c`, `hw/audio/dsp56k/` | the DSP56001 at 25 MHz with 8K words (`dsp-ram-expansion=on`: 32K) |
| Keyboard, mouse | `hw/input/next-kms.c`; ADB on the Turbo machines | |
| Screen | `hw/display/next-fb.c` | 2-bit grey, or 16-bit colour |
| Clock, NVRAM | `hw/rtc/next-rtc.c` | MC68HC68T1, or the MCCS1850 (`new-rtc=on`, on by default on the Turbo machines) |
| Printer | `hw/char/next-printer.c` | the NeXT 400 dpi laser printer; pages are saved as PBM images |
| Turbo | `hw/m68k/next-tmc.c` | memory controller; `nitro=on` gives the 40 MHz Nitro board |

## Options

| Option | What it does |
|---|---|
| `-M next-...,nextstep-idle=off` | Let Mach's idle loop spin, as on a real NeXT (default: on) |
| `-M next-...,net-services=off` | No bootparam, NetInfo and time servers on the user network |
| `-M next-...,nfs-share=DIR` | Share a host folder by NFS; NeXTSTEP mounts it on `/Net/nfs` |
| `-M next-...,audiodev=ID` | Sound to that audiodev; without it, to the host's default |
| `-M next-...,dsp-ram-expansion=on` | 32K words of DSP memory |
| `-M next-station-turbo...,nitro=on` | The Nitro board |
| `-drive file=nvram.bin,format=raw,if=mtd` | Keep the NVRAM in a 64-byte file; without it the defaults are built at every start |

## Network

- `-nic user` gives the guest 10.0.2.15. NeXTSTEP set to configure itself
  wants more than an address: a bootparam server for its name, and a
  NetInfo server to bind to. `net/next/filter-next.c` answers both on the
  user network, as Previous does, and an NTP server gives the date. The
  guest is called `next` in the domain `home`.
- These servers, 10.0.2.254 to the guest, answer inside QEMU: they have
  no host sockets, so nothing but the guest reaches them, not even the
  folder that `nfs-share` shares.
- In the shared folder the host never follows a link: the guest sees
  links as links and follows them itself, so a link cannot reach a host
  file outside the folder.
- With no answers, NeXTSTEP waits a long time at boot for the network
  (Ctrl-C, or c on 2.2, skips it).
- NeXTSTEP refuses root logins over telnet. NeXTSTEP 2.2 has a `me`
  account with no password; on 3.x and 4.x, make a user first.

## Optical disks

The NeXT Computer and the NeXTcube have the 256 MB magneto-optical drive:

    -drive file=od.raw,if=none,id=mo0,format=raw -global next-mo.drive0=mo0

- The image holds the disk as the drive sees it: sectors of 1296 bytes,
  with their ECC, from track 4096 on. A blank cartridge is
  `truncate -s 326032128 od.raw`; NeXTSTEP formats it.
- `b od` in the ROM monitor boots from it.
  NeXTSTEP 2.2's BuildDisk makes a bootable one in about an hour.
- `drive1` is the second drive. The monitor's `eject` and `change` swap
  cartridges.

## Installing from CD

The ROM monitor comes up when the boot disk is blank. There, `b fd`
boots the floppy and `b sd(N,0,0)` boots the Nth SCSI drive, counted from
the lowest SCSI ID. Give the installer a blank disk at ID 0 and the CD
at ID 6:

    -drive file=hd.qcow2,if=none,id=hd0 -device scsi-hd,scsi-id=0,drive=hd0
    -drive file=cd.iso,if=none,id=cd0,format=raw,media=cdrom -device scsi-cd,scsi-id=6,drive=cd0

- NeXTSTEP 3.0: `nextstep-3.0.iso` is not a CD but the installer's hard
  disk, with 2048-byte sectors. Attach it as the boot disk with
  `-device scsi-hd,scsi-id=0,drive=src,logical_block_size=2048,physical_block_size=2048`
  and a blank disk at ID 1; install onto `sd1a`. About 10 minutes.
- NeXTSTEP 2.2, on a 68040 machine: the CD with its 2.88 MB CDROM_Recover
  floppy (`-drive file=fd.img,format=raw,if=floppy`). `b fd`, then
  Rebuild boot device from CD-ROM. About 5 minutes.
- NeXTSTEP 3.3, on a 68040 machine: `3.3_Moto_Boot_Disk.img`, a 1.44 MB
  floppy, with the 3.3 release CD. `b fd`; the floppy copies a base
  system to the disk, the disk boots into the installer, which copies the
  packages from the CD. About 10 minutes.
- NeXTSTEP 3.3 with the Turbo ROMs (3.x): no floppy, the ROM boots the CD
  itself. With the CD the second drive, `b sd(1,0,0)sdmach rootdev=sd1`;
  without `rootdev`, Mach takes the first disk for its root. ROM 2.5 v66
  boots only from disks.
- 3.3 then restarts into the first-boot setup, 2.2 into the Workspace.

## Idle

- Mach on the NeXT never stops the CPU: its idle loop spins over the run
  queues. LEMU finds that loop in the kernel and halts the CPU there until
  the next interrupt (`target/m68k/nextstep_idle.c`). It knows the loops
  of NeXTSTEP 2.2, 3.x and OPENSTEP 4.x.
- NeXTSTEP leaves the DSP waiting for the host in a loop. The DSP stops in
  such a loop until the host port or its DMA is used.
- On an idle Workspace or login window, QEMU then uses about 15% of a host
  core for NeXTSTEP 2.2 and 35 to 40% for 3.3 and 4.2, where it used more
  than one core. Most of what is left is the clock interrupt, whose stack
  shares a page with kernel code: QEMU checks every write there for
  changed code.

## Tested

2026-11-23 on Linux x86_64, with the kits' `run.sh`:

- ns22computer comes up in the Workspace about 50 s after QEMU starts;
  ns33station, ns33turbocolor and os42color reach their login windows in
  1 to 2 minutes.
- telnet answers on all four ports; on NeXTSTEP 2.2, `me` logs in.
- `/usr/etc/halt -p` (2.2) and Power at the login window (3.3) turn the
  machine off, and QEMU exits.
- NeXTSTEP 3.3 plays mu-law sounds through the DSP, and its sounds and
  beeps through the sound hardware.
- NeXTSTEP 2.2 boots from an optical disk that its BuildDisk made, on the
  NeXT Computer.
- NeXTSTEP 2.2 installs from its CD and floppy on the NeXTstation (ROM
  2.5 v66), and 3.3 from the release CD, with the floppy on the
  NeXTstation and from the CD alone on the NeXTstation Turbo (ROM 3.3
  v74). Each installed disk boots to the Workspace.
- NeXTSTEP 3.0 installs from its installer disk on the NeXTstation, and
  the installed disk boots into the first-boot setup and the Workspace.
- The ten kits from ns08computer to os40cubeturbo come up as the table
  above says, through their run.sh; telnet answers on the eight from 1.0
  on.
- The q800 still boots A/UX 3.0.1, 3.1 and 3.1.1 and System 7.1 to 7.5.3
  (the 68040, interrupt and ESP changes are shared).

## Gaps

- The 0.8 v31 ROM fails its SCC test: it needs the SCC's DMA in loopback.
- The DSP's SSI port (sound in at 44.1 kHz through the DSP) is not
  emulated.
- NeXTdimension is not emulated.

## Making a kit

1. Copy the VM's disk and NVRAM (`cp --reflink` or `cp -c`) and boot the
   copy with the kit's `run.sh` line.
2. Log in, clear root's and the user's shell history, `/usr/adm/wtmp`,
   `/usr/adm/lastlog` and the logs in `/usr/adm`, and keep only `localhost`
   and `next` in `/etc/hosts`.
3. Power off from the login window, so that the file systems are clean.
4. Pack with `release/mkrelease.sh`, which converts the disk to a new
   qcow2 and leaves out what the guest never wrote.
