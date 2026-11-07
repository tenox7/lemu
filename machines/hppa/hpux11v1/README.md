# HP-UX 11i v1 on the HP B160L: four CPUs

**TL;DR** `-M B160L -smp 4 -accel tcg,thread=multi,tb-size=512` runs HP-UX 11i v1
(B.11.11) on four PA-7300LC CPUs, each on its own host core. Two QEMU changes: the boot
no longer hangs on Apple Silicon (the first kit needed `-d nochain` for that, which made
everything about 3 times slower), and a vCPU that has nothing to do sleeps instead of
spinning. LEMU's firmware (SeaBIOS-hppa 27) is enough.

## Run

[run.sh](run.sh) and [run.cmd](run.cmd) start the kit `hpux11v1b160l-qemu` (see
[README.txt](README.txt)); [../release/mkrelease.sh](../release/mkrelease.sh) packs it.
They use LEMU's firmware and the disk `hpux11v1.qcow2` on SCSI ID 6. The CDE login
comes up about 70 s after the start; root's password is root.

The guest takes its address from DHCP (`DHCP_ENABLE=1`), and its `/etc/hosts` says that
its name, hpux, is 192.168.1.109. The CDE login (dtlogin) opens the display hpux:0, which
is a TCP connection to 192.168.1.109:6000. With another address that connection goes
nowhere and the login never appears. So the kit's user network is 192.168.1.0/24, and
QEMU's DHCP hands out 192.168.1.109 (`dhcpstart`). A bridged network works too when the
LAN's DHCP gives the guest that address.

Check the guest with `ioscan -fnC processor` (four processors, all CLAIMED). With
`-trace hppa_hpux_match`, QEMU says that it found the idle loop and three queue stores.

## What changed in QEMU

- **Hang at boot on Apple Silicon.** A conditional branch that the instruction before it
  may nullify ends a TB with three exits: taken, not taken, and the branch itself
  nullified. The translator emitted chained exit 0 twice. A TB keeps the place of one
  chained exit per number, the last, so the first one was never patched. On an AArch64
  host that one stays a branch to itself, and the vCPU hung early in the boot. On an x86
  host it went to the main loop, which then chained the other one to the wrong TB. Now
  an exit whose number is taken does without chaining (`gen_goto_tb()` in
  `target/hppa/translate.c`). `-d nochain` hid it by turning off all chaining.
- **Idle loop.** HP-UX never halts a CPU: its idle loop (`pset_idle_loop()`) polls, so
  every idle vCPU kept a host core at 100%. As for 10.20 (see
  [../hpux1020/README.md](../hpux1020/README.md)), the translator finds that loop and the
  three stores with which the scheduler (`_setrq()`) queues a thread for a CPU, by their
  instructions. A vCPU halts when its run queue, the real-time queues of its processor set
  and of the kernel daemons, and its requests are empty, and, if it is the one idle CPU
  that looks at the others' queues, no other CPU has a thread to give away. A store to a
  queue wakes the vCPU that waits for that queue (or the one that looks at the others;
  all of them for a real-time queue), and so does an interrupt. After each interrupt the
  loop runs 32 rounds before it halts again, so the rest of the loop (the reboot check,
  the move of threads between CPUs) still runs. It knows one kernel: the one on this
  kit's disk (B.11.11 with the scheduler patches PHKL_30033 and PHKL_30037). Another
  kernel, or this one linked again with other drivers, keeps spinning.
  `-M B160L,hpux-idle=off` turns it off. See `target/hppa/hpux_idle.c`.

## Speed

Apple M1 Ultra. 300 runs of `/usr/bin/true` from a shell loop, alone and four loops at
once, and the host CPU that QEMU uses while the guest waits at the CDE login.

| Setup                       | 1 loop    | 4 loops    | idle            |
|-----------------------------|-----------|------------|-----------------|
| 1 CPU, `hpux-idle=off`      | 1.7 s     |            | 100% of a core  |
| 1 CPU                       | 1.6 s     |            | 2%              |
| 4 CPUs, `hpux-idle=off`     | 1.8-1.9 s | 3.1-3.6 s  | 400%            |
| **4 CPUs (the kit)**        | 1.9 s     | 3.4-4.0 s  | 4%              |

- Four loops at once take about 7% longer when idle vCPUs sleep: a sleeping vCPU must be
  woken before it takes a new thread.
- Four copies of an awk loop take as long as one (2.0 s): the CPUs run in parallel.

## Known guest messages

- At each boot the iCAP agent (`/sbin/rc2.d/S602icod`) fails with a few ERROR lines in
  `/etc/rc.log`: it finds no CIM server, so it changes nothing. It does no harm; all four
  CPUs stay active.
- Host names come from `/etc/hosts` only (`hosts: files` in `/etc/nsswitch.conf`).
  QEMU's DNS (192.168.1.3, from DHCP) works after a change to `hosts: files dns`.
- Mozilla 1.7 (`/opt/mozilla/mozilla`) works. Firefox 1.5 (`/opt/firefox`) does not
  start: `firefox-bin` is a PA-RISC 2.0 program, and the B160L's PA-7300LC is PA-RISC 1.1.
- After `shutdown -h`, HP-UX says "System has halted" and then spins in a busy wait; type
  `quit` in the monitor.
