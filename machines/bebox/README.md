# Be Inc. BeBox (dual PowerPC 603)

`-M bebox` in `qemu-system-ppc` is the BeBox: two 603e CPUs (`-smp 1` for
one), an MPC105 host bridge, an 82378 PCI/ISA bridge, an 82091AA Super I/O
(floppy, serial 1 and 2, parallel, IDE), the 8042 keyboard controller, a
TL16C554 (serial 3 and 4, MIDI 1 and 2), a CS4231A codec, a 53C810 SCSI
controller and a Cirrus GD5434 in PCI slot 1. RAM is 8 to 256 MiB, 32 MiB
by default.

It runs Be's own boot ROM. It is not in the tree: build a flash image from
the two ROM files with `tools/mkflash.py` and give it as a pflash drive. The
guest can rewrite it, as Be's ROM updater does, so work on a copy.

    tools/mkflash.py bootnub.rom bootmain.rom flash.bin
    qemu-system-ppc -M bebox -drive if=pflash,format=raw,file=flash.bin ...

`bootnub.rom` (16 KiB) is the nub that starts both CPUs and sets up the
MPC105; `bootmain.rom` is boot ROM 1.1d7 (April 1996). Together they fit the
BeBox's 128 KiB Am29F010. BeOS R4 needs the newer boot ROM it carries as
`/beos/etc/bebox_bootmain.image` (November 1998, 315 KiB), which only fits
the 512 KiB Am29F040 that Be's flash driver also takes:

    tools/bfs.py beos-r4.iso@160186s get /beos/etc/bebox_bootmain.image main.image
    tools/mkflash.py bootnub.rom main.image flash-r4.bin

BeOS R3 and R4.5 carry their own (377 and 312 KiB), also for the 512 KiB
chip; mkflash drops the "BOOT" tag in front of R4.5's:

    tools/bfs.py beos-r3.iso@32856s get /beos/etc/bebox_bootmain.image main.image
    tools/mkflash.py bootnub.rom main.image flash-r3.bin

BeOS R5 brings no boot ROM; R4.5's boots it. Preview Release 2 carries
its own (368 KiB, October 1997) in the BFS volume of its CD, see below.
The first Preview Release brings none; PR2's boots it. Developer
Release 8, BeOS 1.1d7 and BeOS 1.1d7.1 boot with ROM 1.1d7 (bootnub.rom
and bootmain.rom).

The flash chip follows the image size: 128 KiB is an Am29F010, 512 KiB an
Am29F040.

## CDs

The BeOS R4 CD and later ones have several tracks: ISO 9660 and HFS for other systems,
then BFS volumes for x86 and PowerPC. The boot ROM finds the PowerPC volume
from the table of contents. `tools/bin2iso.py` turns a MODE1/2352 `.cue` and
`.bin` dump into a 2048-byte image and prints the track starts for the
`tracks` property of `scsi-cd`:

    tools/bin2iso.py "BeOS Release 4.cue" beos-r4.iso      # tracks=0:3402:160186
    -drive if=none,id=cd0,media=cdrom,readonly=on,format=raw,file=beos-r4.iso
    -device scsi-cd,drive=cd0,scsi-id=3,tracks=0:3402:160186

The R3 PowerPC CD has one track: give it without `tracks`. Its BFS
volume starts at 2048-byte sector 32856, for `tools/bfs.py`. The boot ROM
also boots CDs on IDE (`ide-cd`, `bus=ide.0,unit=1`).

The Preview Release 2 CD is a Mac CD with one track and an Apple
partition map; its BFS volume starts at 512-byte block 101245, byte
51837440:

    tools/bin2iso.py "BeOS Preview Release 2.cue" beos-pr2.iso
    tools/bfs.py beos-pr2.iso@51837440 get /beos/etc/bebox_bootmain.image main.image

The first Preview Release CD is laid out the same way, with its BFS
volume at 512-byte block 43245, byte 22141440.

`tools/bfs.py` lists and copies files from a BFS volume, read-only.

## Debugging

The kernel debugger and the boot ROM's debug output use serial 4, the
fourth `-serial`, at 19200 baud.

## Installing BeOS

Boot the R3 or R4 CD with a blank disk (1 GiB is plenty), agree to the
licence, pick the disk, let the installer initialize it and click Begin.
A SCSI disk on the 53C810 and an IDE disk on the 82091AA both work:

    -drive if=none,id=hd0,format=qcow2,file=beos.qcow2
    -device scsi-hd,drive=hd0,scsi-id=0          # SCSI
    -device ide-hd,drive=hd0,bus=ide.0,unit=0    # or IDE

R3 mounts a CD put in after boot with `mountvolume -r -all` in a
Terminal.

BeOS reads and writes the IDE disk and the IDE CD with DMA: the drive's
DMA request is wired to ISA DMA channel 5, and the 82378 moves the data
with its scatter-gather lists.

R5 installs the same way, with the R4.5 boot flash. With the optional
software it takes about 270 MB: give it a 2 GiB disk.

Preview Release 2's installer only lists initialized disks: click Begin,
let it open DriveSetup, select the disk and Setup > Initialize > Be File
System (press and hold through the menus), mount it, and close
DriveSetup. PR2 shows a wrong date, in December 1901 (and in 1916 with
`-rtc base=1999-11-21`).

The first Preview Release installer has no way to DriveSetup: with no
BFS volume it crashes in the kernel debugger when you click Begin.
Initialize the disk with the PR2 CD's DriveSetup first; the Preview
Release installer then offers to initialize it again, with its own
format, and installs. It shows the same date in 1901.

Developer Release 8 is on an old Be file system volume that
`tools/bfs.py` does not read (Aaru extracts it). Its installer
initializes the disk itself: pick Initialize in the left pop-up menu and
the SCSI disk in the other, click Begin, then switch back to Install
From and install onto NewDisk. BeOS 1.1d7 (April 1996) and 1.1d7.1 (May
1996) install the same way. It has no PCI network driver: give it an ISA NE2000,
`-device ne2k_isa,netdev=n0,iobase=0x300,irq=5`, the port and IRQ its
Network preferences offer.

`1.1d7/`, `1.1d7.1/`, `dr8/`, `pr/`, `pr2/`, `r3/`, `r4/`, `r45/` and
`r5/` have the `run.sh`, `run.cmd` and `README.txt` of ready-to-run kits:
R3, R4.5 and R5 on IDE with a DEC 21041, R4 on SCSI with a PCI NE2000,
DR8, the Preview Release and Preview Release 2 on SCSI with a DEC 21041,
1.1d7 and 1.1d7.1 on SCSI with an ISA NE2000.

## Release

`release/mkrelease.sh` packs one kit from the one build of
`machines/release` (see its README): the kit's folder name, a folder
with its `beos.qcow2` and `flash.bin`, `STAGE/ppc`, and the file to
write.

    sh release/mkrelease.sh r45 VMDIR STAGE/ppc beosr45bebox-lemu.tar.lz

Before packing, check that the binaries have what the kits use:
`sh release/check.sh macos OUT/macos-qemu-system-ppc.features`, and
`linux` with the Linux lists. `release/required.txt` lists the features.

## Network

The BeBox has no network port: use a PCI card in a free slot. BeOS R4
and later take QEMU's `ne2k_pci`. BeOS R3 and earlier have no PCI NE2000
driver, but their `tulip` driver takes a DEC 21041, `tulip-21041`:

    -nic none -netdev user,id=n0 -device tulip-21041,netdev=n0,addr=0e

Give BeOS a static address (10.0.2.15, mask 255.255.255.0, gateway
10.0.2.2, DNS 10.0.2.3 with `-netdev user`). Its DHCP client sends no
DHCP requests here with either card, only ARP requests for 0.0.128.0.

## Sound

The CS4231A plays through QEMU's default audio backend, or the one named
with `-M bebox,audiodev=ID`. BeOS's driver plays on DMA 6 and records on
DMA 7; QEMU's codec only plays, so there is no recording.

BeOS R5's audio output drops a buffer that comes even a little late.
Under plain TCG the guest's clock does not match the work the emulated
CPUs get done (the kernel measures them at 715 MHz), every buffer comes a
few hundred microseconds late, and R5 plays silence. With
`-icount shift=3,sleep=on` the guest's clock follows the instructions it
runs, about 125 MHz, and R5 plays sound. The R5 kit runs that way.

The clock keeps the last two digits of the year, as the bq3285 does.
Disks installed with an older LEMU, whose clock said 2046, have file
dates past 2038, and R4.5's media kit then finds no file readers: no
sound file plays. Fix such a disk once in a Terminal:

    find /boot -xdev -print0 | xargs -0 touch -c

## Known problems

- Now and then a BeOS Restart panics in the kernel debugger at the end
  of the shutdown, before the boot ROM runs: reset the VM
  (`system_reset` in the monitor).
- Sound has short dropouts now and then: the guest's mixer is late
  with a buffer, as both CPUs share one host thread and BeOS's idle
  loop never halts them.
- With `-smp 1` the BeOS kernel does not get going; the BeBox always
  has two CPUs.
