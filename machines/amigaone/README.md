# AmigaOS 4.0 on the AmigaOne (amigaone)

<!-- Copyright (c) 2026 Natalia Portillo <claunia@claunia.com> -->

**TL;DR** `aos40/run.sh` boots AmigaOS 4.0 on `-M amigaone` from a U-Boot
built from source. Graphics (Radeon 7000), network (3Com 3C920) and sound
(VIA AC97, playback and recording) work.

## Kit

[aos40/run.sh](aos40/run.sh) and [run.cmd](aos40/run.cmd) start the kit
`aos40amigaone-lemu` (see [README.txt](aos40/README.txt));
[release/mkrelease.sh](release/mkrelease.sh) packs it from the one build of
[../release](../release/README.md):

    sh release/mkrelease.sh aos40 VMDIR STAGE/ppc aos40amigaone-lemu.tar.lz

`VMDIR` holds the disk `aos40.qcow2`. The firmware comes from this tree.

## Files

| File | Option | What it is |
|---|---|---|
| `fw/u-boot-amigaone.bin` | `-bios` | U-Boot for the AmigaOne, 512 KB, from `pc-bios/` (see [firmware](#firmware)) |
| `fw/vgabios-ati.bin` | `ati-vga,romfile=` | QEMU's SeaVGABIOS for the Radeon, which U-Boot runs in its x86 emulator |
| `aos40.qcow2` | `ide-hd`, primary master | The disk, 8 GB, with an RDB: the second level booter `slb_v2` 1.16 and one SFS partition, DH0 `System` |

## Machine

| Part | Device | AmigaOS driver |
|---|---|---|
| CPU | MPC7457 (G4) | |
| North bridge | Articia S | |
| South bridge | VIA 82C686B: IDE, USB, PS/2, serial | `a1ide.device`, `uhci.usbhcd` |
| Graphics | `ati-vga,model=rv100,vgamem_mb=64` (Radeon 7000, 64 MB), 2D, CP and 3D | `ATIRadeon.chip`, `W3D_Radeon.library` |
| Ethernet | `3c905c` in slot 6, the on-board 3C920 | `eth3com.device` |
| Sound | `via-ac97` of the 686B | `via-ac97.audio` (AHI) |

## Firmware

`pc-bios/u-boot-amigaone.bin` is ACube's U-Boot (`roms/u-boot-sam460ex`,
commit 1e5f4a1) with the AmigaOneG3SE board of old mainline U-Boot put
back. The changes are [firmware/u-boot-amigaone.patch](firmware/u-boot-amigaone.patch);
[firmware/build.sh](firmware/build.sh) rebuilds the binary in Docker:

    sh firmware/build.sh

To change the firmware, edit a work tree made the same way (git archive of
the submodule, then the patch), and write the patch again from it with
`git diff`. Never commit in the submodule.

What the board does:

- `boot1=cdrom`, `boot2=ide`: a bootable CD wins over the disk.
- `boota_timeout=5`: the second level booter boots its first entry after
  5 seconds. Without it, `slb_v2` waits for a key.
- The console is the VGA card with the PS/2 keyboard (`stdout=vga`,
  `stdin=ps2kbd`). `setenv stdout serial; setenv stdin serial` moves it to
  the serial port. There, `slb_v2` prints a text menu and ignores
  `boota_timeout`.
- `setenv os4_commandline serial debuglevel=5` before `boota` sends the
  kernel's debug output to the serial port.
- No environment is kept: each boot starts from the default one (U-Boot
  says "bad CRC, using default environment").

Things that were wrong and are fixed in the patch:

- PCI io BARs come from the low io window (bus 0x2000 up), not from bus
  0x800000: the VGA BIOS in the x86 emulator only has 16 bit ports.
- `BE_int86` puts the x86 stack at 0x2fffe: at the top of the first
  megabyte the board maps the BIOS date and model bytes, and INT 10h saw
  the wrong registers.
- The VESA mode is 0x111 (640x480, 16 bits): SeaVGABIOS for the Radeon
  has no 8 bit modes.
- DBAT2 maps 0x90000000, where the Radeon's registers are.

## AmigaOS notes

- Warp3D works: GLQuake and Quake III Arena (both from Aminet) run. Give
  the Radeon 64 MB as `run.sh` does. With the 16 MB default,
  `W3D_Radeon` moves textures in and out of video memory every frame and
  some come out as noise.
- GLQuake needs `AmigaInput.library`, which the 4.0 CD lacks; copy it
  from the 4.1 CD to `LIBS:`. Without it, 4.0's `lowlevel.library`
  crashes when the game starts.
- The installer does not copy the AC97 driver, even when "VIA686B
  onboard" is chosen: copy `Contrib/AHI/VIA-AC97/via-ac97.audio` to
  `DEVS:AHI/` and `VIA-AC97` to `DEVS:AudioModes/`.
- The network interface is `DEVS:NetInterfaces/eth0`:
  `device=DEVS:Networks/eth3com.device`, `unit=0`, `configure=dhcp`.
  The "Add new network interface" window of Prefs/Internet does not close
  with Use or Cancel here, so the file was written from the Shell.
