# HP-UX 10.20 on the HP B160L: four CPUs

**TL;DR** `-M B160L -smp 4 -accel tcg,thread=multi,tb-size=512` runs HP-UX 10.20 on four
PA-7300LC CPUs, each on its own host core. A vCPU that has nothing to do sleeps instead of
spinning, and the X server no longer crashes on the fast CPU (it did, and the VUE login
never appeared). LEMU's firmware (SeaBIOS-hppa 27) is enough; SeaBIOS-hppa 26 and
SeaBIOS-hppa 25 with `legacy-fw=on` work too.

## Run

[run.sh](run.sh) and [run.cmd](run.cmd) start the kit `hpux1020b160l-qemu` (see
[README.txt](README.txt)); [../release/mkrelease.sh](../release/mkrelease.sh) packs it.
They use LEMU's firmware (SeaBIOS-hppa 27; the first kit, packed on 2026-11-05, has 26)
and the disk `hpux1020.qcow2` on SCSI ID 6.
To try SeaBIOS-hppa 25 (see Firmware), add `-M legacy-fw=on -bios hppa-firmware-v25.img`.
It is in LEMU's history:
`git show 90ce9a4150:pc-bios/hppa-firmware.img > hppa-firmware-v25.img`

Check the guest with `ioscan -fnC processor` (four processors, all CLAIMED) and
`sar -u -M 2 3` (load per CPU). Run it with a window, or with `-display vnc=...`. With
`-display none` the X server does not start and the VUE login never appears.

## What changed in QEMU

- **TLB purge.** `pdtlb` and `pitlb` are broadcast: the purge must be done on every CPU
  before the instruction ends. QEMU stopped all CPUs for each one. HP-UX 10.20 issues
  about 2 million of them while it boots and runs a short test, and each vCPU spent
  about half of its time waiting. Now only a CPU that holds an entry in the purged range
  is stopped. 97.5% of the purges needed no stop. Each CPU's TLB tree has a small lock
  so another CPU can look at it.
- **EIRR.** `mtctl %cr23` cleared bits with a plain read-modify-write, while IPIs and
  devices set bits from other threads. An IPI could be lost, and HP-UX waits for its
  IPIs. Both sides use atomic operations now. This race did not show up in the tests;
  it was found by reading the code.
- **`legacy-fw=on`.** SeaBIOS-hppa 25 and older want the CPUs at 0xfffb0000 and the
  fw_cfg port at 0xfffa0000. LEMU moved them for version 26. The option puts them back.
  It is off by default, so other machines are not changed.
- **Idle loop.** HP-UX never halts a CPU: its idle loop polls, so every idle vCPU kept a
  host core at 100%. Now the translator finds that loop (and the two stores with which the
  scheduler queues a thread for a CPU) by their instructions, and a vCPU with nothing to
  do sleeps until an interrupt comes or another vCPU gives it work. An idle guest takes
  about 3% of one host core (4% at the VUE login screen) instead of 400%, and the speed
  does not change. It knows one kernel, the B2352B of 9 June 1996 (the HP-UX 10.20
  install); any other kernel keeps spinning. `-M B160L,hpux-idle=off` turns it off. See
  `target/hppa/hpux_idle.c`. `-trace hppa_hpux_match` shows whether the kernel was
  recognized.
- **X server.** On a fast host the VUE login did not appear: the console repeated
  "Starting HP VUE on display hpux:0" and `/var/vue/Xerrors` said "Server for display
  hpux:0 terminated unexpectedly 2048". The X server (`/usr/bin/X11/X`, X11R6 built 12
  September 1999) died with SIGFPE in `calibrate_cr16()`. That function runs a loop of
  200000 rounds six times, takes the time of each with `gettimeofday` in whole
  milliseconds, and divides the timer ticks by the sum. A 160 MHz PA-7300LC needs about
  6 ms for the loop; emulated at full speed it needs 0.8 ms, every round counts as 0 ms,
  and the division by zero traps. (The old setup, one CPU and `-d nochain`, was slow enough
  not to hit it.) Now the translator finds that loop by its instructions and the vCPU
  waits 20 ms at the start of each round: 0.12 s per start of the server, and the server
  learns the timer frequency to within a few percent. `-M B160L,hpux-x11-pace=off` turns
  it off. `-trace hppa_hpux_pace` shows the waits. Without it, 5 of 9 boots (firmware 25
  and 26) never reached the login in 150 s; with it, 20 of 20 did, in 75 to 88 s.
  It knows that one build of the server; another build is left alone. To see the crash,
  or to fix the guest instead of QEMU: after `init 3`, run `/usr/bin/X11/X :0` as root
  and look for "Floating exception(coredump)"; `adb /usr/bin/X11/X /core` and `$r` show
  `$$divU` with a divisor of 0 and the caller `calibrate_cr16`. Raising the loop count
  (the `ldil` at `calibrate_cr16+0x34`, word `0x22980000` to `0x22800020` for 16 million
  rounds) in a copy of the server also cures it.

## Speed

Apple M1 Ultra, seconds, lower is better. Tests inside HP-UX: an awk loop of 1.5 million
rounds, `compress` of the 3.9 MB kernel six times, 800 runs of `/usr/bin/true`. "4x" is
four copies at the same time. Every answer was checked.

| Setup                                          | awk 1x | awk 4x | compress 1x | compress 4x | fork 1x | fork 4x |
|------------------------------------------------|--------|--------|-------------|-------------|---------|---------|
| QEMU 11.1.3, 1 CPU, `-d nochain`               | 33.6   | 132.9  | 16.4        | 64.9        | 2.2     | 8.8     |
| LEMU, 1 CPU, `tb-size=512`                     | 10.8   | 42.8   | 5.0         | 19.9        | 0.95    | 3.9     |
| LEMU master, 4 CPUs (before the purge change)  | 10.9   | 11.1   | 5.1         | 6.1         | 4.8     | 10.5    |
| **This, 4 CPUs** (idle vCPUs sleep)            | 10.5   | 10.9   | 4.1         | 5.3         | 1.1     | 1.4     |

- Four copies of the awk loop take as long as one: the CPUs really run in parallel.
- Without the purge change, four CPUs made fork and exec four times slower than one CPU.
- A stress test (6 workers on 4 CPUs: checksums, compress and uncompress, awk, constant
  fork and exec; 720 rounds) had no errors.

## Things that slow it down

- **`-d nochain`** turns off block chaining and costs about 3 times (31 s instead of
  11 s for the awk loop on one CPU). Don't use it.
- **`tb-size=512`.** On Apple Silicon the 1 GB code buffer lands at one of two kinds of
  address, by chance. At `0x300000000` everything runs about 25% slower (the lookup of
  the next block is twice as expensive); at lower addresses it is faster. With the
  default size about 4 starts in 10 get the slow one. With 512 MB, all 9 starts I
  measured got the fast one. Other LEMU guests on a Mac probably have the same lottery.
- **Memory order.** `TCG_MO_ALL` stays. TSO measured no gain (the full barrier before
  each store is what costs), and no barriers at all would be unsafe: HP-UX unlocks a
  spin lock with a plain store.
- **Idle CPUs.** Without `hpux-idle` (default on, see above) each of the four host threads
  stays at 100% even when the guest is idle. A sleeping vCPU is woken at the clock tick
  (100 Hz) and when another CPU queues a thread for it; the scheduler does that without an
  interrupt, so a missed wake-up shows as a tick (10 ms) of delay. A loop of 800 runs of
  `/usr/bin/true` took 8 s instead of 1.4 s before the wake-ups were right. It is the
  test to repeat after a change.

## Firmware

Same QEMU, same disk, same four CPUs; only the firmware differs.

| SeaBIOS-hppa | 1 CPU                  | 4 CPUs                                                       |
|--------------|------------------------|--------------------------------------------------------------|
| 25 (`legacy-fw=on`) | works, VUE login | works, VUE login                                        |
| 26 (LEMU, the first kit) | works, VUE login | works, VUE login                                  |
| 27 (upstream QEMU)         | works, VUE login | works, VUE login                                      |

Version 27 used to panic on four CPUs ("Unexpected interrupt while PSW-Q = 0", then
"Processor 1 did not start within expected time period"). The firmware was not the cause:
QEMU's trap for a disabled FPU was. HP-UX 10.20 clears CCR when it starts a CPU and then calls
`bzero()`, which uses FP stores, in real mode with PSW.Q = 0. An interruption with Q = 0 saves
neither the instruction address nor IIR, so the handler of HP-UX cannot tell what trapped.
QEMU takes that trap only while PSW.Q is set now (`target/hppa/cpu.c`). The guest clock is correct
with 25 and 26 (`sleep 5` takes 5 s). Version 26 once looked broken, because the VUE
login did not appear and the "Starting HP VUE" message repeated every 25 s. That was the
X server crash above, not the firmware: with `hpux-x11-pace` (the default) it logs in at
once. Before that fix, about 1 boot in 8 on a host with other VMs running, and half of
the boots on a quiet one, did not reach the login with either firmware; the idle code was
not involved (`hpux-idle=off` had them too).
