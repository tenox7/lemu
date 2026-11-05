# HP-UX 9.05 on the HP 9000/715

**TL;DR** `-machine 715` runs HP-UX 9.05 on one PA-7100LC with the graphics console and
HP VUE 3.0, a SCSI disk and the LAN, and it installs from the HP CDs. An idle guest uses about
2% of a host core. Snapshots (`savevm`, `-loadvm`, the MCP `vm-snapshot` tool) work.

## Run

[run.sh](run.sh) is an example: there is no kit yet. It needs `hpux905.qcow2` next to it
(HP-UX 9.05 on SCSI ID 6). The firmware is LEMU's SeaBIOS-hppa 27. Log in as `root` at the
VUE login (about 100 s after the start); root has no password. `./run.sh -loadvm NAME`
starts from a snapshot made with `savevm NAME` in the monitor.

## Install

The CDs are the HP-UX 9.05 Install disc and Core OS disc for the Series 700 (SCSI ID 2).
Install on the serial console: `-machine 715,graphics=off -serial mon:stdio` (Ctrl-A c
switches between the console and the monitor), an empty qcow2 disk on SCSI ID 6
(`-drive if=scsi,bus=0,index=6,file=hpux905.qcow2,format=qcow2`) and the install CD as a SCSI
CD-ROM on ID 2 (`-drive if=scsi,bus=0,index=2,media=cdrom,readonly=on,format=raw,file=install.iso`).

1. At the firmware prompt: `bo FWSCSI.2.0`, `y` to interact with IPL, and at `ISL>`
   `hpux boot disc(;0):INSTALL`. The installer partitions the disk, writes the boot files
   and reboots.
2. The second boot starts `/etc/inst.update` from inittab. When it asks for the media, put
   the Core CD in the drive (monitor: `change scsi0-cd2 core.iso raw`), press Return, select
   all filesets and load them (165 filesets, about 150 MB, 1.5 minutes). The system reboots.
3. `set_parms` asks for host name, address, mask, gateway, time zone and the date. It takes
   only the years 1970 to 1999: use 1998, which has the calendar of 2026.
4. Switch to the graphics console (drop `graphics=off`). The first boot asks for the PS/2
   keyboard language: press Return. `init 3` starts VUE (or set `initdefault` to 3).

## What changed in QEMU

HP-UX 9.05 needed these on top of the 10.20 work. The graphics console and the I/O space
came from QEMU's "hppa-715-2" series (Helge Deller, Keith Monahan).

- **NCR710.** A JUMP without a test is always true, ISTAT writes keep DSTAT and SSTAT0,
  and SIGP ends an idle WAIT RESELECT. Without this the vCPU spun for ever in the script
  interpreter while the kernel found the disk.
- **i82596 LAN.** The PORT command is the low four bits, the address the rest; the chip
  interrupts when the command unit goes idle (HP-UX sends a frame as one EL command and
  waits for it); frames in flexible mode have their first bytes in the TCB; NSAI = 0
  inserts the source address. The 7006 (AIX 3.2.5) uses the same model.
- **BLR.** `/etc/update` runs a `blr` with reserved bits set; the decoder ignores them.
- **X server.** `calibrate_cr16()` of the 9.05 X server divides by zero on a fast host.
  `hpux-x11-pace` knows its loop as well as the one of 10.20.
- **Idle loop.** `loop_till_process_found_to_run()` of the kernel (A.09.05, B2352A) adds up
  four run queue counts and polls EIRR bit 28. The translator finds it and the vCPU sleeps
  until an interrupt comes. 93% of a host core becomes 2%, with the same timings.
  `-machine 715,hpux-idle=off` turns it off. See `target/hppa/hpux_idle.c`.
- **Snapshots.** The i82596 did not save its SCP and the CPU did not re-arm the interval
  timer after a load, so a restored guest had no LAN and no clock.
- **Artist.** A write to one of the 16 VRAM buffers that the model does not have aborted
  QEMU (HP PEX Drive does it); it is ignored now and logged with `-d unimp`.
- **FPU.** The trap for a disabled FPU is taken only while PSW.Q is set, as HP-UX 10.20
  starts its other CPUs with the FPU off and Q = 0. `fmpyadd` and `fmpysub` read both operand
  sets before they write either result: the HP compiler lets the add take the register that
  the multiply writes, and wants the old value.
- **Artist colour maps.** The X server gives a window with its own colour map (an 8 bit
  TrueColor window, as the Battalion game opens) a second bank of 256 colours at 0x800 of
  the colour map memory, and tags the pixels with a window id through the control plane.
  The display now looks such pixels up in the second bank. Before, those windows had wrong
  colours and vertical streaks.

## Notes

- The hardware clock is out of range for HP-UX 9.05 ("time-of-day register time is
  out-of-range"): it goes on from the time of its last shutdown. The question "Is the date
  correct?" at boot can be switched off with `TIMEOUT=0` in `/etc/rc`.
- On a serial-only VM at run level 3 the console repeats "X-server can not be started": use
  telnet, or run level 2.
- `shutdown -h -y 0` halts the system; `quit` in the monitor stops QEMU.
