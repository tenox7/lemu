# SunOS 4.1.4 on the SPARCstation 5 (sun4m)

**TL;DR** `sunos414/run.sh` boots SunOS 4.1.4 on `-M SS-5` with LEMU's
OpenBIOS. An idle guest uses 2 to 4% of a host core, the screen can be any
size, and the keyboard and mouse behave like the real ones.

## Options

| Option | What it does |
|---|---|
| `-M SS-5,sunos-idle=off` | Let the idle loop spin, as SunOS does on a real machine (default: on) |
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
