# SunOS 4.1.4 on the SPARCstation 5 (sun4m)

**TL;DR** `sunos414/run.sh` boots SunOS 4.1.4 on `-M SS-5` with LEMU's
OpenBIOS. An idle guest uses 2 to 4% of a host core, the screen can be any
size, and the keyboard and mouse behave like the real ones.

## Kit

[sunos414/run.sh](sunos414/run.sh) and [run.cmd](sunos414/run.cmd) start the
kit `sunos414-lemu` (see [README.txt](sunos414/README.txt));
[release/mkrelease.sh](release/mkrelease.sh) packs it from the one build of
[../release](../release/README.md). They use LEMU's OpenBIOS and the disk
`sunos414.qcow2` at SCSI target 0.

## Options

| Option | What it does |
|---|---|
| `-M SS-5,sunos-idle=off` | Let the idle loop spin, as SunOS does on a real machine (default: on) |
| `-M SS-20,solaris-idle=off` | The same for Solaris 2.5.1 to 9 with one CPU (default: on) |
| `-M SS-20,solaris-smp-idle=off` | The same for Solaris 2.5.1 to 9 with more than one CPU (default: on) |
| `-vga cg3 -g 1920x1080x8` | Screen size: 640x480 or more, a width that 8 divides. See [cgthree](cgthree/README.md) |
| `-m 256` | RAM. SunOS gets 16 MB less than this |

## RAM: give it 16 MB more than SunOS should have

- OpenBIOS keeps the top 16 MB of RAM for itself. `-m 32` leaves SunOS 16 MB
  (`mem = 16252K` at boot), `-m 256` leaves it 240 MB.
- With 16 MB, starting OpenWindows can panic the kernel
  (`BAD TRAP ... Data access exception` in `vuid_set_value`): at the first
  event of a kind the window system asks for 76 bytes without waiting, and
  it does not check that it got them. It is rare: once in 14 starts here.
- The SPARCstation 5 takes 256 MB at most.

## Idle

- SunOS 4 never halts the CPU: it spins in `idlework()`. LEMU finds that
  loop in the kernel and halts the CPU there until the next interrupt
  (`target/sparc/sunos_idle.c`).
- An idle guest uses about 2% of a host core at the text console and 4% with
  OpenWindows, where it used 100%. The clock, disk and network are as fast as
  before.
- It knows the loop of the 4.1.4 kernel. `-trace sparc_sunos_idle_match`
  prints a line when it finds it. It is off with more than one CPU.

## Solaris 2.x

LEMU's sun4m also runs Solaris 2.5.1 to 9 better than QEMU does:

- **Idle, one CPU.** Solaris calls the CPU module's `idle_cpu` hook in its
  idle loop. Only the microSPARC-II with the `pmc` driver stops the CPU
  there; the SS-10 and SS-20 always spin. LEMU finds the call and halts the
  CPU until the next interrupt (`target/sparc/solaris_idle.c`,
  `-trace sparc_solaris_idle_match`). Off with more than one CPU.
- **Idle, more CPUs.** That loop never calls `idle_cpu`, and nothing tells
  an idle CPU that another queued work for it. LEMU halts it at the call of
  `disp_getwork` and wakes it when another CPU lowers its interrupt level
  with work queued, at an interrupt, or after 10 ms
  (`-trace sparc_solaris_idle_mp_match`).
- **More CPUs hung.** A write to `%psr` in a delay slot, as in Solaris's
  `splx`, did not end the translation block, so an interrupt it let in
  waited. Two CPUs running Solaris stopped within minutes (QEMU 8.2 and
  later).
- **Power management (APC).** The registers are 16 bytes, as the firmware
  says, not 1. Solaris 2.6's `pmc` writes several at attach and panicked
  with `kernel write fault at addr=0x6a000002`. Register 0 still halts the
  CPU; the others keep what is written.
- **No-fault mode.** Solaris's window trap handlers on the SS-5 set and
  clear the MMU's no-fault bit twice per trap, and QEMU flushed the whole
  TLB each time: deep calls ran 4 to 10 times slower than on the SS-20.
  LEMU flushes only when the TLB holds an entry made in no-fault mode
  (`-trace sparc_mmu_ctrl_flush`).
- **CD after a SCSI bus reset.** The ESP refused Select with ATN after a
  bus reset, and Solaris 2.5.1 looped on `gross error in esp status` at
  its CD (QEMU 10.1 and later).

The qtests `sun4m-solaris-idle-test`, `sun4m-solaris-smp-idle-test`,
`sun4m-pil-delay-slot-test`, `sun4m-apc-test` and `sun4m-mmu-nofault-test`
check these, and `am53c974-test` (i386) the ESP.

## Keyboard and mouse

Both are on one serial chip (`hw/char/escc.c`): the keyboard on channel A,
the mouse on channel B. What LEMU fixed:

- **Freeze and jumping pointer.** When a keyboard byte and a mouse byte
  waited together, the chip named the wrong one. SunOS then threw a mouse
  byte away (the pointer jumped) or took the interrupt for ever (the guest
  hung until the next key, for example while Shift was held and the mouse
  moved).
- **Caps Lock and Num Lock.** Every second press was lost.
- **Key repeat.** The host's key repeat was passed on, on top of SunOS's
  own, so a key held a little too long typed twice.
- **Mouse packets.** A full queue cut a packet in half.

`tests/qtest/sun4m-escc-test.c` checks all of these.

What SunOS does by itself, which a script that moves the mouse must know:

- **The first 48 mouse packets do nothing.** When the window system opens
  the mouse, the kernel tries line speeds, and at each one it throws the
  first packets away: 48 at 9600 baud, where it starts (`msopen`, `msrserv`
  and `msrput` in the kernel). The emulated mouse has no line speed, so
  SunOS stays at 9600. The pointer follows from packet 49 on. A hand on the
  mouse loses a moment once; a script has to send 48 moves first.
- **The pointer speeds up.** OpenWindows doubles some moves (`xset q`:
  acceleration 2/1, threshold 15), so moves out and back may not end where
  they began. `xset m 1 1` turns it off, and then they do.

The emulated keyboard is a Type 5 with the US layout. Stop (L1) and the
other keys on its left side have no key on a PC or Mac keyboard.

## Screen

- `-vga cg3` is the frame buffer for SunOS 4: 8 bits, any size. OpenWindows
  and SunView use the whole screen.
- The text console is drawn by OpenBIOS in the top part of a big screen (the
  top 409 lines at 1920x1080). SunOS wants 34 rows; it gets 51 there.
- SunOS 4.1.4 boots with up to about 6 MB of pixels: 3200x1800 works,
  3200x2000 resets at boot.

## CD-ROM

- SunOS 4 reads a CD in blocks of 512 bytes and looks for the drive at SCSI
  target 6 (`sr0`). QEMU's CD drive has blocks of 2048 bytes by default, and
  SunOS then fails with `I/O error`.
- This works, as in `sunos414/run.sh`:
  `-drive file=cd.iso,if=none,id=cd,media=cdrom,readonly=on,format=raw
  -device scsi-cd,drive=cd,scsi-id=6,physical_block_size=512`,
  then `mount -rt hsfs /dev/sr0 /cdrom` in SunOS.
- SunOS says `sr0: Unrecongized Vendor 'QEMU'` at boot and uses the drive.

## Old programs and the FPU state

- Some old static SunOS programs save the FPU state in their signal handler
  with `st %fsr` and `ld %fsr` words that have rd 4 (c92ba0e0, c90ba0e0, from
  an old libc's `_sigtramp`). Real V7 and V8 chips ignore rd there.
- QEMU 8.2 and later raised an illegal instruction for them, so these
  programs died with SIGILL at their first signal. One is `islandLIC`, the
  licence daemon of Island Productivity Series 3.0 (1992). LEMU takes any rd
  on sparc32 (`fsr_rd_ok` in `target/sparc/translate.c`).

## Testing a change

Work on a copy of the disk, with user networking, never bridged:

```sh
cp -c sunos414.qcow2 /tmp/test.qcow2
qemu-system-sparc -name test -M SS-5 -m 256 -vga cg3 -display none -monitor none \
    -drive file=/tmp/test.qcow2,if=scsi,bus=0,unit=0,format=qcow2 \
    -nic user,model=lance,net=192.168.1.0/24,host=192.168.1.10,hostfwd=tcp:127.0.0.1:41423-192.168.1.123:23
```

- The guest keeps its fixed address, so the user network is given the
  guest's subnet (`net=`) and the forward names the guest's address.
- Log in with the MCP tools: `serial-connect` to port 41423, user `root`, no
  password, shell `csh`.
- `vm-stats` shows whether the CPU halts when idle; `debug-profile` shows
  where it spins.
- Kernel symbols: `nm -n /vmunix` in the guest, then `debug-symbols`.
