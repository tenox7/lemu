# AmigaOS 4.1 on the ACube Sam440ep (sam440ep)

<!-- Copyright (c) 2026 Natalia Portillo <claunia@claunia.com> -->

**TL;DR** `aos41/run.sh` boots AmigaOS 4.1 on `-M sam440ep` from a U-Boot
built from source. Graphics (Radeon Mobility M9 with 64 MB, 2D, CP and 3D
for compositing and Warp3D), network (the on-chip EMAC) and sound (CS4281, playback)
work.

## Kit

[aos41/run.sh](aos41/run.sh) and [run.cmd](aos41/run.cmd) start the kit
`aos41sam440ep-lemu` (see [README.txt](aos41/README.txt));
[release/mkrelease.sh](release/mkrelease.sh) packs it from the one build of
[../release](../release/README.md):

    sh release/mkrelease.sh aos41 VMDIR STAGE/ppc aos41sam440ep-lemu.tar.lz

`VMDIR` holds the disk `aos41.qcow2`. The firmware comes from this tree.

## Files

| File | Option | What it is |
|---|---|---|
| `fw/u-boot-sam440ep.bin` | `-bios` | U-Boot for the Sam440ep, 512 KB, from `pc-bios/` (see [firmware](#firmware)) |
| `fw/vgabios-ati.bin` | `ati-vga.romfile=` | QEMU's SeaVGABIOS for the Radeon, which U-Boot runs in its x86 emulator |
| `aos41.qcow2` | `if=ide`, port 0 of the SiI3114 | The disk, 8 GB, with an RDB and one FFS partition (`DOS\07`, 512 byte blocks), DH0 `System` |

## Machine

| Part | Device | AmigaOS driver |
|---|---|---|
| CPU | PowerPC 440EP | |
| SATA | SiI3114 behind the PCI-PCI bridge | `sii3114ide.device` |
| USB | `isp1561-ohci`, with a USB keyboard and mouse | `ohci.usbhcd` |
| Graphics | `ati-vga,model=rv250` at 0c (1002:4c66, the Mobility M9, 64 MB), 2D, CP and R200 3D | `ATIRadeon.chip`, `W3D_R200.library` |
| Ethernet | the two on-chip EMACs (`ppc4xx-emac`) through the MAL and the ZMII bridge, PHYs at 24 and 25 | `ppc440ep_eth.device` |
| Ethernet, optional | `3c905c` in PCI slot 07 (`-nic ...,model=3c905c`) | `eth3com.device` |
| Sound | `cs4281` at 0e, with an ALC655 codec | `cs4281.audio` (AHI) |

The SiI3114 has four ports, and `if=ide` drive index *n* is port *n*
(without an index 1, index 2 moves up to port 1, so `-cdrom` works).
AmigaOS 4.1's `sii3114ide.device` 53.1 uses only ports 0 and 1: it is
built for "4 chips, 2 ports, 2 units/port" and never looks at ports 2 and
3. For a third drive, put a second SATA card in a PCI slot, which
`sii3112ide.device` drives:

    -device sii3112,id=sata2,bus=pci.1,addr=06 \
    -drive if=none,id=cd0,media=cdrom \
    -device ide-cd,drive=cd0,bus=sata2.0

A PCI NIC must not go in slot 06: U-Boot gives that slot IRQ 50 (UIC1 input
18), and the AmigaOS 4.1 kernel never enables any UIC1 input. Slot 07 is
IRQ 29 on UIC0. A `-nic` or `-device` with no slot still lands in 07.

## Firmware

`pc-bios/u-boot-sam440ep.bin` is ACube's GPL U-Boot for the Sam440ep
(`u-boot-2015.d`) with [firmware/u-boot-sam440ep.patch](firmware/u-boot-sam440ep.patch).
[firmware/build.sh](firmware/build.sh) downloads the tarball, checks it and
rebuilds the binary in the AmigaOne's Docker image:

    sh firmware/build.sh

The patch runs the Radeon's own VGA BIOS instead of ATI's M9 BIOS, fixes
the x86 emulator's PCI config data port and stack, sets VESA mode 0x111,
accepts a read delay calibration where every setting works, counts down
the boot menu (`menuboot_delay=5`), stops USB before the kickstart is
loaded, unless it comes from USB, and sets `ethaddr` and `eth1addr` from
the boot EEPROM and saves the environment when they are not set.

## AmigaOS notes

- Install: in Media Toolbox, make DH0 FFS (`DOS\07`) with 512 byte
  blocks. SFS\00 did not format here. The install then takes over an
  hour: FFS flushes the disk after almost every write (about 55,000
  flushes, each a host fsync).
- The post-install procedure runs again at every boot until it is
  completed.
- The Internet wizard: its radio buttons did not take clicks here; the
  keyboard shortcuts work (`t` Ethernet, `m` Manual).
- The network interface is `DEVS:NetInterfaces/EMAC0`:
  `device=ppc440ep_eth.device`, `unit=0`, `configure=dhcp`. The one for
  the 3C905C is in `SYS:Storage/NetInterfaces/ETH3COM`.
- `ppc440ep_eth.device` reads its MAC address from `ethaddr` (`eth1addr`
  for unit 1) in the U-Boot environment saved in the EEPROM at 0x54-0x57,
  not from U-Boot's copy in RAM, and gives up without one. The board puts
  the EMACs' addresses in bytes 0xf0-0xfb of the boot EEPROM at 0x53; our
  U-Boot sets `ethaddr` and `eth1addr` from them and saves the environment
  when they are missing.
- Sound: choose the CS4281 HiFi 16 bit stereo mode for unit 0 in
  Prefs/AHI.
- The Radeon driver writes its CP packets to the CSQ PIO aperture in the
  chip's byte order, so `ati-vga` does not byte-swap that aperture even
  when the register aperture is big-endian.
- On the M9, AmigaOS draws the AmiDock, the pop-up menus and the
  compositing of windows with the R200 3D engine: two textures, the
  register combiners and blending. It writes the texture and stage enables
  to 0x2cc0 and gives texture coordinates in texels (`SE_VTE_CNTL` bit 12).
  With an `rv100` the screen works but the AmiDock never shows.
- Warp3D works through `W3D_R200`: GLQuake and Quake III Arena (both from
  Aminet) run with the MiniGL 2.0 of the 4.1 CD. With only 16 MB, as the
  board had before it got the real 64 MB, `W3D_R200` finds no room for a
  Z buffer at 1024x768x32.
