# MorphOS 3.21 and AmigaOS 4.1 Final Edition Update 1 on the Genesi/bPlan Pegasos II (pegasos2)

<!-- Copyright (c) 2026 Natalia Portillo <claunia@claunia.com> -->

**TL;DR** `morphos/run.sh` boots MorphOS 3.21 on `-M pegasos2` through
QEMU's Virtual Open Firmware; `aos41feu1/run.sh` boots AmigaOS 4.1 Final
Edition Update 1 through bPlan's SmartFirmware in the board's flash. Graphics
(Radeon 9250 in the AGP slot, 2D, 3D with transform and lighting, and
command processor), network (the on-board VIA Rhine-II, and the MV64361's
Gigabit Ethernet port under MorphOS) and sound (VIA AC97) work.

## Kit

[morphos/run.sh](morphos/run.sh) and [run.cmd](morphos/run.cmd) start the
kit `morphos321pegasos2-lemu` (see [README.txt](morphos/README.txt));
[release/mkrelease.sh](release/mkrelease.sh) packs it from the one build of
[../release](../release/README.md):

    sh release/mkrelease.sh morphos VMDIR STAGE/ppc morphos321pegasos2-lemu.tar.lz

`VMDIR` holds the disk `morphos.qcow2` and `boot.img`, the boot image from
the root of the MorphOS 3.21 CD.

[aos41feu1/run.sh](aos41feu1/run.sh) and [run.cmd](aos41feu1/run.cmd) start the kit
`aos41feu1pegasos2-lemu` (see [README.txt](aos41feu1/README.txt)):

    sh release/mkrelease.sh aos41feu1 VMDIR STAGE/ppc aos41feu1pegasos2-lemu.tar.lz

`VMDIR` holds the disk `aos41feu1.qcow2` and `flash.bin`, the board's flash
with SmartFirmware and its NVRAM (see [Firmware](#firmware)).

## Files

| File | Option | What it is |
|---|---|---|
| `boot.img` | `-kernel` | MorphOS's boot image (ELF), from the CD; VOF starts it |
| `morphos.qcow2` | `if=ide`, primary master | The disk, 8 GB: DH0 `System` (1 GB) and DH1 `Work`, SFS\0, made by the MorphOS installer |
| `bin/share/qemu/vof.bin`, `dtb/pegasos2.dtb` | (none) | QEMU's Virtual Open Firmware and the device tree it starts from, from `pc-bios/` |
| `flash.bin` | `-drive if=pflash` | AmigaOS kit: the 512 KB flash at 0xfff00000, an AMD Am29F040: SmartFirmware 1.1 and its NVRAM |
| `aos41feu1.qcow2` | `if=ide`, primary master | AmigaOS kit: the disk, 8 GB: `BOOT` (64 MB, FFS, holds `amigaboot.of`) and DH0 `System` (FFS2, 512 byte blocks), made with Media Toolbox and the installer of the Pegasos2InstallCD-53.54 |

## Firmware

AmigaOS boots from Open Firmware: `amigaboot.of` reads the disks through
the firmware and loads the Kickstart modules from `Kickstart/Kicklayout`.
QEMU's VOF has no disk services, so the AmigaOS kit runs bPlan's
SmartFirmware 1.1, the Pegasos II's own. It comes from bPlan's update
`up050404` (in the
[Internet Archive](http://web.archive.org/web/20071021223056/http://www.bplan-gmbh.de/up050404/up050404)):

    tail -c +85581 up050404 | head -c 524288 > flash.bin

With `-drive if=pflash` the board puts that image in a CFI flash that
answers as an Am29F040 (01/a4, the 512 KB part in bPlan's own flash
table), so SmartFirmware keeps its NVRAM there (at 0x10000) and the image
keeps it. With `-bios` the firmware is in ROM and its NVRAM is lost at
each reset. The kit's NVRAM:

    setenv boot-device hd:0
    setenv boot-file amigaboot.of
    setenv auto-boot? true
    setenv auto-boot-timeout 100

`amigaboot.of` must be on the first partition, which SmartFirmware reads
(FFS); copy it from the CD's root after installing. The firmware's console
is the serial port. It runs no VGA BIOS, so the Radeon has `romfile=`.

## Machine

| Part | Device | MorphOS driver |
|---|---|---|
| CPU | MPC7457 (G4) | |
| North bridge | Marvell MV64361 | |
| South bridge | VIA VT8231: IDE, USB, PS/2, serial | `ide.device` |
| Graphics | `ati-vga,model=rv280,vgamem_mb=64` (Radeon 9250, 1002:5960, an R200, 64 MB) in the AGP slot, PCI unit 0 of the MV64361 | Radeon, through the CP's PIO path; TinyGL `r200.library` |
| Ethernet | `via-rhine` (VT6102) at 0d, on board, the default NIC | `via_rhinepci.device` (`eth1`) |
| Gigabit Ethernet | port 1 of the MV64361 (`mv643xx-eth`), PHY at SMI address 9; gets a `-nic` with `model=mv643xx_eth` | `mv6436x_eth.device` (`eth0`) |
| Sound | `via-ac97` of the VT8231 | `pegasos.audio` (AHI) |

The MV64361 has two PCI units. NetBSD (`sys/arch/ofppc/pci/gt_mainbus.c`)
calls unit 0 the AGP bus: it holds the AGP slot, at 66 MHz
(`/pci@c0000000`). Unit 1 is the PCI bus with the VT8231, the Rhine and
the PCI slots (`/pci@80000000`). The chip has no AGP port or GART, so the
AGP card runs as a PCI one there.

The board has both network ports, and both can be used at once:

    -nic user,model=via-rhine -nic user,model=mv643xx_eth

The Gigabit port's interrupt goes to PCI INTA, so IRQ 9 of the VT8231,
whatever the MV64361's own interrupt masks say: neither the firmware nor
the guests set those for it. Linux takes the port on IRQ 9 too
(`arch/powerpc/platforms/chrp/pegasos_eth.c`).

## MorphOS notes

- "Radeon: Failed to allocate CCE DMA memory" is what a real Pegasos II
  prints too. MorphOS's Radeon driver turns on DMA only for a card with a
  PCI Express capability (its own PCI GART) or behind an Apple UniNorth or
  U3 AGP bridge (106b:002d, 0034, 0066, 004b, 0059). Otherwise it feeds the
  command processor through the PIO aperture.
- Gigabit Ethernet: Network prefs list `mv6436x_eth.device` unit 0 as
  `eth0`. Only one of `eth0` and `eth1` can be ticked there; ping and DHCP
  work over either.
- Network prefs: Use and Save only write what changed from the shown
  settings. On the CD, the DHCP entry for `eth1` is shown but not set up
  until something about it is changed. NetStack only gets the interfaces
  that the prefs write; `ENV:sys/Interfaces` by hand and `NetConfig` do not
  add one to a running NetStack.
- 3D goes through TinyGL, which needs an R200: its `r200.library` draws
  the 3D screen blankers (Atlantis, Gears, BlockTube, GLMatrix and the
  others) through the R200's transform and lighting. The kit has a
  Radeon 9250 (`rv280`), the card Pegasos IIs were sold with. TinyGL has no driver for the R100: with
  `model=rv100` (the Radeon 7000) the desktop works, but the 3D blankers
  stay greyed out.
- AHI's speaker picture in Prefs/Audio is not a test button; `C:Play` plays
  a sample.
- Install: First Flight, Installation, New Installation, automatic
  partitioning. It takes about 4 minutes.
- Rebooting: MorphOS reboots by jumping to the firmware's reset vector at
  0xfff00100. With VOF the board puts code there that resets the machine
  through the VT8231's keyboard controller. `system_reset` works too.

## AmigaOS 4.1 Final Edition Update 1 notes

- The disk has the Final Edition with Update 1 installed.
- Drivers: `peg2ide.device`, `ATIRadeon.chip` with the compositing of
  AmigaOS 4.1 and `W3D_R200.library` for Warp3D, `via-ac97.audio` (AHI)
  and the "VIA Rhine II" interface of the Internet wizard
  (`DEVS:NetInterfaces/VIA-RHINE`). Its "Pegasos 2 built-in Ethernet" is
  `via-rhine.device` too: AmigaOS 4.1 has no driver for the MV64361's
  Gigabit port.
- Install: boot the CD with `boot cd amigaboot.of` at the firmware prompt.
  In Media Toolbox make `BOOT` (FFS, 512 byte blocks) and DH0 (FFS2, long
  names, 512 byte blocks), save and reboot, then run the installer and
  format both from it. Afterwards copy `amigaboot.of` from the CD's root to
  `BOOT:`. On a disk with `cache=unsafe` the install takes a few minutes.
- The installer's "Finish" and AmigaOS's reboot power the machine off:
  QEMU closes.
- The Final Edition driver uploads 32 bit textures through a swap surface
  with an upper bound of 0xffffffff, and flips `SURFACE_CNTL` between the
  16 and 32 bit swaps and none for each bitmap it draws, many times for
  each window. `ati-vga` reorders the video memory to follow, but the
  screen only into the swap of its pixels' width: following the 32 bit
  screen into no swap turned its red and blue around for a moment now and
  then (see `hw/display/ati.c`).
- The clock: `battclock.resource` 53.1 of the Final Edition looks for the
  clock chip in the VIA 686 (1106:0686) or the ATI SB600 (1002:438d) only,
  never in the Pegasos II's VT8231 (1106:8231), so the date starts at 1978,
  on a real board too. The VT8231 has the same MC146818 at ports 0x70 and
  0x71, so the kit's `SYS:Kickstart/battclock.resource.kmod` is patched to
  look for 1106:8231 instead of 1106:0686, one instruction at file offset
  0x13c (`li r9,0x686` becomes `ori r9,r11,0x8231`; r11 is 0 there):

      printf '\x61\x69\x82\x31' | dd of=battclock.resource.kmod bs=1 seek=316 conv=notrunc

  `run.sh` gives `-rtc base=localtime`, as AmigaOS keeps local time.
