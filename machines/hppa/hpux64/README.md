# 64-bit HP-UX 11i v1 and v2 on the HP C3700

**TL;DR** `-M C3700` runs 64-bit HP-UX 11i v1 (B.11.11) and 11i v2 (B.11.23) on one
PA-8700 CPU. Both install from their CDs, boot from disk to the console login, and run
Java and CDE on an emulated HP VISUALIZE-EG PCI card. Nine QEMU changes and eight firmware
changes make this work. There is no kit yet.

## Run

[run.sh](run.sh) `11.11` or `11.23` starts LEMU's build (`build/qemu-system-hppa`, or
`$QEMU`) with the disk `hpux1111.qcow2` or `hpux1123.qcow2` on SCSI ID 6. The console is
the first serial port, in the terminal (`-serial mon:stdio`). Ctrl-A c goes to the QEMU
monitor and back, Ctrl-A x quits QEMU. Halt HP-UX first with `reboot -h`. A window shows the
graphics card (`-device artist-pci`, see Graphics). `GFX=0 ./run.sh 11.11` runs without the
card and the window (`-nographic`). `CONSOLE=graphics ./run.sh 11.11` makes the card the
system console.

The network is QEMU's user network. The install leaves the LAN card without an address,
and `/etc/hosts` gives the host name 127.0.0.1. Then DCE `rpcd` stops at boot ("Start
RPC daemon if needed ... FAIL"), and on 11.11 `samd`, which needs `rpcd`, respawns all
the time. Give the guest QEMU's address 10.0.2.15, change the host name's line in
`/etc/hosts` to 10.0.2.15, and reboot:

    ch_rc -a -p 'IP_ADDRESS[0]=10.0.2.15' -p 'SUBNET_MASK[0]=255.255.255.0' -p 'INTERFACE_STATE[0]=up' /etc/rc.config.d/netconf
    ch_rc -a -p 'ROUTE_DESTINATION[0]=default' -p 'ROUTE_GATEWAY[0]=10.0.2.2' -p 'ROUTE_COUNT[0]=1' /etc/rc.config.d/netconf

Then `telnet 127.0.0.1 11113` (11.11) or `11233` (11.23) reaches the guest.

## Graphics

`-device artist-pci` adds an HP VISUALIZE-EG (A4977A): the Artist chip of the B160L on a
PCI card, with an STI ROM (`pc-bios/hppa-sti-artist.rom`). HP-UX claims it as "PCI Display
(103c1005)" (driver `graph3`), runs the ITE on it, and X and CDE use it. The keyboard and
mouse are the C3700's USB ones. The firmware and the system console stay on the serial
port.

The system console stays on the serial port, unless the firmware gets
`-fw_cfg opt/console,string=graphics` (`CONSOLE=graphics` in run.sh). Then the firmware
draws its text on the card (its own STI code, the regions in BAR 0) and names the card as
the console in PAGE0. HP-UX says "System Console is on the ITE": the rc messages and the
console login are on the screen, with the USB keyboard. The firmware cannot read the USB
keyboard, so its keyboard stays on the serial port, and the serial port gets a copy of the
firmware text. After the kernel starts, the serial port has no login. Seen once on 11.23: the
first X server start died, and dtlogin started it again.

A disk installed without the card needs three changes. Make them once, with the card:

- 11.23 only: the kernel has no graphics drivers. Add them and reboot:
  `kcmodule graph3=static gvid=static gvid_core=static ite=static`.
  11.11 has them already.
- At boot without graphics, `/usr/dt/config/dtrc.d/20_graph_conf` comments out the
  `Local` line in `/etc/dt/config/Xservers`, and it does not put it back. Then dtlogin
  starts no X server. Fix: `cp /usr/dt/config/Xservers /etc/dt/config/Xservers`.
- The first boot with the card asks for the USB keyboard language on the serial console
  (26 is US English). The answer goes to `/etc/kbdlang`.

## Install

- Make a disk (`qemu-img create -f qcow2 hpux1111.qcow2 20G`), put the install CD on
  SCSI ID 2 (`-drive if=scsi,bus=0,index=2,media=cdrom,readonly=on,format=raw,file=...`)
  and boot it with `-boot d`.
- Ignite runs on the serial console. At "No user interface", type 2 Enter, Enter, u, o.
  At the warning about data on the disk, type n. Change CDs in the QEMU monitor
  (`info block`, then `change`).
- The first boot from disk on 11.11 runs `hpux reflash.6750` once (a firmware tool), which
  resets the machine. The next boot runs `hpux`.
- In swconfig, the cimserver MOF build takes about 20 minutes. It makes many TLB misses;
  it does not hang.
- root has no password after the install.

## What changed in QEMU

- **SuperIO interrupts** (`hw/isa/pc87560.c`). The PIC in the NS87560 now knows edge and
  level interrupts (ELCR, config register 67h). A poll acknowledges an edge interrupt, and
  ICW1 resets the edge detector. Before, IRQ 6 came again and again in `superio_isr`.
- **SCSI SCRIPTS, three fixes** (`hw/scsi/lsi53c895a.c`, two of them as properties that
  the C3700 and A400 turn on):
  - A register-to-register move "SFBR to SFBR" (bit 23) used the wrong operand. The c8xx
    driver's idle loop then started an empty job for target 0.
  - `select-continue`: a second SELECT while the chip's own selection runs goes on, and a
    selection timeout stops SCRIPTS only at a phase compare. Before: "Already reselected"
    loops and "lsp is NULL on Selection Time-out".
  - `wait-complete`: SCRIPTS wait while a command without data (a tagged SYNCHRONIZE
    CACHE) is not complete. Before, the bus showed a false DATA IN phase, and the first
    boot from disk on 11.23 panicked with "lsp is NULL on phase mismatch".
- **PCI interrupt transactions** (`hw/pci-host/astro.c`). A PCI write to
  0xfee00000 + (id << 12) + (eid << 4) is an interrupt for that CPU. The c8xx SCRIPTS
  send one after each command. Before, the 11.23 kernel waited for ever.
- **Protection IDs** (`target/hppa`). On PA 2.0, a write is allowed when any PID that
  matches allows it, not only the first match. HP-UX 11.23 loaded the same PID twice, once
  write-disabled, and a copy to user space looped in the protection ID trap handler.
- **TCG deposit** (`tcg/tcg.c`, also on master). On AArch64 hosts both inputs of a deposit
  may be constant zero. When the optimizer did not fold it, the register allocator gave
  `out_rri` the constant as a register number, and the result took bits of host register
  x0. This broke the stubs that the HotSpot JVM writes, and `ServCtlMgr` on 11.11 hung.
  Now two constant inputs give one `movi`.
- **artist-pci** (`hw/display/artist.c`). The Artist (`TYPE_ARTIST`) in a PCI card,
  vendor HP, device 0x1005. BAR 0 (32 MB) holds the registers and, at 16 MB, the frame
  buffer, as on the B160L. The expansion ROM holds the STI ROM. The ROM also shows at the
  start of BAR 0: the STI region list puts region 0 (the ROM) there, and HP-UX finds the
  card's display by that address.
- **Power button in snapshots** (`target/hppa/machine.c`). The firmware's soft power word
  (`env.power_btn`) was not saved. loadvm resets the machine first, and 0 there means
  "button pressed", so HP-UX powered off after each loadvm.
- Debug aid: the trace event `hppa_tlb_ar_fault` shows access-rights faults.

## Firmware

LEMU's SeaBIOS-hppa 27 with eight changes, from github.com/tenox7/seabios-hppa branch
`hpux11` (offered upstream as hdeller/seabios-hppa pull request 9):

1. The console path names SuperIO serial port 1, not the SuperIO. Else HP-UX finds no
   console tty, and Ignite stops with "Cannot open /dev/console".
2. The IODC of the PCI host bridge (Elroy) does block I/O. Else HP-UX limits swap and
   dump to the first 4 GB, and lvlnboot stops with "Swap logical volume size beyond the
   IODC max address".
3. IODC disk I/O uses the drive in the layers, not always the boot drive.
4. PDC_RDR (144) gives PDC_BAD_PROC without a warning. 11.11 calls it again and again.
5. Big PCI BARs (the card's 32 MB) go to the LMMIO space, each at the next free,
   size-aligned address. Before, the PCI window gave the small BARs no space.
6. The STI code builds for 64 bits too: 64-bit HP-UX calls the ROM's 64-bit routines
   (alternate code type PA-RISC 64) with 64-bit pointers. HP-UX copies each routine to
   RAM on its own, so a routine calls no code outside of it.
7. `make parisc` also builds `out/hppa-sti-artist.rom` (`scripts/buildstirom.py`): a PCI
   ROM header, the region map and the STI ROM with 32- and 64-bit code. It keeps the
   Artist's graphics ID: HP-UX has its own routines for the real EG's ID, and they need
   STI regions that the device does not have.
8. `opt/console=graphics` also works with the PCI card on the Astro machines: the STI
   console on the card, as above.

## HP-UX 11i v3 (B.11.31) on the A400

11.31 on PA-RISC supports only zx1, REO and PAT machines. `-M A400,pluto=on` shows the
A400's Astro as a Pluto (zx1) SBA. The MCOE June 2007 DVDs (2 discs) install and boot to
login:

```
qemu-system-hppa -M A400,pluto=on -m 2048 -smp 1 -nographic \
  -drive if=scsi,bus=0,index=6,file=hpux1131.qcow2,format=qcow2 \
  -drive if=scsi,bus=0,index=2,media=cdrom,file=hp-ux-11.31-mcoe-06.2007-dvd1.iso \
  -boot d -nic user,model=tulip
```

At about fileset 1125 of 1197 Ignite asks for media 2: in the monitor,
`change scsi0-cd2 hp-ux-11.31-mcoe-06.2007-dvd2.iso raw read-only`, then answer 1.

With `pluto=on`:
- The firmware gives the SBA the Pluto IODC (hv 0x880). The IOC registers are also at
  0x2000, as on a Pluto. IBASE and IMASK start as a 1 GB IOVA window at 1 GB, and the I/O
  PDIR index is the page inside that window.
- The SCSI card is a 53C875 (`lsi53c875`): the 11.31 c8xx driver takes the 53C875, 53C896
  and 53C1010-66, but not the 53C895 or 53C895A.
- The Diva serial card has the UART interrupt status byte at BAR 0 + 0x1b. 11.31 asio0_intr
  reads it.
- fcnv from a doubleword reads fr0 as zero (`target/hppa`, for all machines). Before, it
  read the FP status: the aC++ library sized a string from it, `operator new` threw in a
  static constructor, and evmd aborted at boot (S020evm FAILED).
- PAT firmware (branch `hpux1131`): 32-bit LMMIO ranges in the I/O view, -7 (PDC_NE_CELL_MOD)
  at the end of the module list, PDC_PAT_IO 0, the hint table and the hard-fail calls, and
  PDC_IODC_READ of a PCI function by its config address (else lvlnboot stops with "IODC
  max address", as on 11.23 before fix 2).

### CDE on 11.31

The 11.31 PA X server has no Artist driver. It has the XFree86 4.2 radeon driver, and the
gvid driver takes a Radeon 7500. Add the card (the console stays on the serial port):

```
  -device ati-vga,model=rv200,x-linear-aper-size=0x4000000,guest_hwcursor=true \
  -serial mon:stdio
```

- `model=rv200` is 1002:5157, emulated as the RV100. gvid maps 64 MB from every memory BAR
  of this card, so the aperture is 64 MB too.
- `guest_hwcursor=true`: QEMU draws the mouse pointer into the screen. Without it, SDL and
  cocoa show a host cursor and move the host pointer to where X put its pointer. X doubles
  fast moves, so the pointer jumps. The AIX ES1000 VMs use it for the same reason.
- X uses a 32 bpp big endian frame buffer (SURFACE_CNTL swaps), sends images through
  HOST_DATA with DP_DATATYPE HOST_BIG_ENDIAN_EN, and has a mono hardware cursor that it
  writes through the swapping aperture.
- The Elroys are Mercury LBAs (hv 0x783) at SBA + 0x20000 + rope * 0x2000, as on an
  rp34x0. gvid looks for them there.
- The firmware puts the small BARs right above the big ones. The kernel refuses gvid's
  64 MB map of the register BAR if it overlaps the frame buffer or the I/O ports.
- The PLL registers (CLOCK_CNTL_INDEX and DATA) work. The X server waits for
  PPLL_REF_DIV.
- USB is a NEC OHCI: 11.31 does not take the Apple one. Kernel modules: `hcd`, `hub`,
  `hid`, `usbd`, `gvid`, `gvid_core` (kcmodule ...=static).
- Install CDE-English from DVD 1: Ignite leaves it out.
- A boot without a graphics card makes `/usr/dt/config/dtrc.d/20_graph_conf` comment out
  the Local line in `/etc/dt/config/Xservers`. It does not put it back. Before the next
  boot with the card: `cp /usr/dt/config/Xservers /etc/dt/config/Xservers`.

### Graphics console on 11.31

With the card ROM `hppa-sti-radeon.rom` and `opt/console=graphics`, the Radeon is the system
console: the firmware text, the HP-UX boot and rc messages and the console login are on the
screen, with the USB keyboard. CDE starts as before, and the console text comes back when X
stops.

```
  -device ati-vga,model=rv200,x-linear-aper-size=0x4000000,guest_hwcursor=true,romfile=hppa-sti-radeon.rom \
  -fw_cfg opt/console,string=graphics -serial mon:stdio
```

- The ROM is QEMU's x86 VGA BIOS (`vgabios-ati.bin`), then an STI image for the card
  (SeaBIOS-hppa `stirom.c`, `STI_RADEON`: 1280x1024, 8 bits). X POSTs the card with the x86
  BIOS (`gvid_him_cons`). `make parisc-qemu-install` in `roms/seabios-hppa` builds it. The
  64-bit firmware has the same STI image in it, for its own console.
- gvid takes a card with an STI ROM only if bit 0x04 of `ext_bus_support` (byte 0x37 of the
  STI header) is set. Else graph3 gets the card, and graph3 does not know the Radeon.
- Without an STI ROM, gvid gives the console the kernel's own VGA STI image. Its CRC check
  fails, and HP-UX panics ("CRC check of STI rom failed").
- STI fonts: `next_font` is an offset from the first font. With an offset from the current
  font, HP-UX panicked in `sti_get_fontdata`.
- On a card with an STI ROM, gvid does not load a HIM for X (EACCES), unless the kernel
  variable `gvid_him_override` is set. X then stops with "Failed to load gvid_him_cons". The
  rc script [gvidhim](gvidhim) sets it before dtlogin starts. Install it as
  `/sbin/init.d/gvidhim` (mode 555, owner bin:bin) and link it as `/sbin/rc3.d/S980gvidhim`.
  It writes the running kernel only:
  `echo 'gvid_him_override/W 1' | adb -w /stand/vmunix /dev/kmem`. On a card without an STI
  ROM the variable has no effect.
- After the boot loader starts, the firmware's own messages ("Unimplemented PDC_PAT_...") go
  to the serial port only, so they do not write over the HP-UX console.

## Not working yet

- HP-UX 11.31: PDC_PAT_IO 13 and PDC_PAT_COMPLEX 0 and 13 are not there.
- A400 without `pluto`: PAT is not complete (`pat_init_cell` and others).
- With the graphics console, the firmware's boot menu reads only the serial port (no USB
  keyboard in the 64-bit firmware).
- Crash dumps through IODC: ENTRY_IO on the Elroy returns BAD_OPTION.
- Only one CPU is tested.
