# One build for all kits

These scripts build QEMU once for all the kits: the six binaries the
kits use, for macOS, Linux and Windows. Each machine's own `release/`
folder then packs its kits from that one build. Without them, each of
the five release folders builds its own QEMU, so a full set of kits
compiles the same code 15 times, with slightly different features.

| Binary | Kits (`<name>-qemu.tar.lz`) | Pack script |
|---|---|---|
| qemu-system-ppc | aix415rs6k, aix433rs6k, aix51rs6k, nt40powerpc, os2powerpc, sol251powerpc | `../40p/release/mkrelease.sh` |
| qemu-system-ppc | aix325rs6000 | `../7006/release/mkrelease.sh` |
| qemu-system-ppc64 | aix53pseries, aix61pseries, aix71pseries, aix72pseries | `../pseries/release/mkrelease.sh` |
| qemu-system-alpha | nt31axp, nt40axp | `../jensen/release/pack.sh` |
| qemu-system-mips64el | nt31mips, nt4-jazz, nt4-rm200, nt4-rm200-smp, nt4srv-rm300-smp4 | `../rm200/release/pack.sh` |
| qemu-system-mips64 | sinixn545 | `../rm200/release/pack.sh` |
| qemu-system-hppa | hpux1020b160l, hpux905-715, hpux907-715 | `../hppa/release/mkrelease.sh` |

Run them on an Apple Silicon Mac with Homebrew and Docker. The Linux and
Windows builds run in Docker.

## Build

Commit first: the scripts build the working tree. Linux and Windows copy
it without .git and pass the git id, so `--version` still names the
commit.

    sh buildall.sh ../.. OUT

- It runs four builds at once: `build-mac.sh`, `build-linux.sh amd64`,
  `build-linux.sh arm64` and `build-windows.sh`. Each can also run alone.
- The binaries and their libraries land in `OUT/macos-arm64`,
  `OUT/linux-x86_64`, `OUT/linux-aarch64` and `OUT/windows-x86_64`.
- `OUT/rc` must show exit code 0 for all four. The logs are in
  `OUT/*.log`.
- Each binary has every feature that any of the five machine builds had.
  qemu-system-hppa gets the same set.
- `build-mac.sh` prints the oldest macOS the binaries run on
  (`needs macOS ...`). The kit READMEs must say the same.

## Feature check

`features.sh`, the same as in `rm200/release` and `jensen/release`, lists
what each binary has in `OUT/macos-*.features` and
`OUT/linux-*.features`.

- MIPS and Alpha: run the machine's `check.sh` on each list, for example
  `sh ../rm200/release/check.sh macos qemu-system-mips64el OUT/macos-qemu-system-mips64el.features`.
- PowerPC has no saved lists yet. Run `features.sh` on the binaries of
  the last kits and compare: `comm -23 OLD NEW` must print nothing.
- The Windows build is not run. `build-windows.sh` checks the configure
  summary and the git id instead.

## Stage and pack

    sh stage.sh OUT STAGE

`stage.sh` makes `STAGE` new. `STAGE/ppc`, `STAGE/ppc64`, `STAGE/alpha`
and `STAGE/hppa` each hold only their own binary. `STAGE/mips` has the
folder names that `rm200/release/pack.sh` wants. Give the pack scripts
these folders in place of their own build, for example:

    sh ../40p/release/mkrelease.sh nt VMDIR STAGE/ppc nt40powerpc-qemu.tar.lz
    sh ../7006/release/mkrelease.sh VMDIR STAGE/ppc aix325rs6000-qemu.tar.lz
    sh ../pseries/release/mkrelease.sh aix53 VMDIR STAGE/ppc64 aix53pseries-qemu.tar.lz
    sh ../jensen/release/pack.sh nt40 SRCDIR STAGE/alpha nt40axp-qemu.tar.lz
    sh ../rm200/release/pack.sh KITDIR qemu-system-mips64el STAGE/mips nt4-rm200-qemu.tar.lz
    sh ../hppa/release/mkrelease.sh hpux1020 VMDIR STAGE/hppa hpux1020b160l-qemu.tar.lz
    sh ../hppa/release/mkrelease.sh hpux905 VMDIR STAGE/hppa hpux905-715-qemu.tar.lz
    sh ../hppa/release/mkrelease.sh hpux907 VMDIR STAGE/hppa hpux907-715-qemu.tar.lz

- `VMDIR`, `SRCDIR` and `KITDIR` hold the guest files: disk, NVRAM and
  firmware. See each machine's notes.
- Pack one kit at a time: each pack runs `plzip -9`, which uses all
  cores.

## Test

- Boot each kit on the Mac. Move the mouse, click something, check that
  the monitor answers, and look at every screen, not only the main one.
- Linux: `../rm200/release/ltest.sh`, `../jensen/release/ltest.sh` and,
  for the PowerPC kits, `ltest-ppc.sh amd64|arm64 KITCOPY BINARY`. Use
  copies of the unpacked kits: a boot writes to the disk.
- The Windows build is not run.
