# One build for all kits

These scripts build QEMU once for all the kits: the nine binaries the
kits use, for macOS, Linux and Windows. Each machine's own `release/`
folder then packs its kits from that one build. Without them, each of
the five release folders builds its own QEMU, so a full set of kits
compiles the same code 15 times, with slightly different features.

| Binary | Kits (`<name>-lemu.tar.lz`) | Pack script |
|---|---|---|
| qemu-system-ppc | aix415rs6k, aix433rs6k, aix51rs6k, nt351powerpc, nt40powerpc, os2powerpc, sol251powerpc | `../40p/release/mkrelease.sh` |
| qemu-system-ppc | aix325rs6000 | `../7006/release/mkrelease.sh` |
| qemu-system-ppc | beos11d7bebox, beos11d71bebox, beosdr8bebox, beosprbebox, beospr2bebox, beosr3bebox, beosr4bebox, beosr45bebox, beosr5bebox | `../bebox/release/mkrelease.sh` |
| qemu-system-ppc64 | aix53pseries, aix61pseries, aix71pseries, aix72pseries, aix73pseries | `../pseries/release/mkrelease.sh` |
| qemu-system-alpha | nt31axp, nt351axp, nt40axp, vms15axp | `../jensen/release/pack.sh` |
| qemu-system-mips64el | nt31mips, nt351-jazz, nt351-rm200, nt4-jazz, nt4-rm200, nt4-rm200-smp, nt4srv-rm300-smp4 | `../rm200/release/pack.sh` |
| qemu-system-mips64 | sinixn545 | `../rm200/release/pack.sh` |
| qemu-system-hppa | hpux1020b160l, hpux905-715, hpux907-715, hpux1100b160l, hpux11v1b160l, hpux11v2c3700, hpux11v3a400 | `../hppa/release/mkrelease.sh` |
| qemu-system-i386 | nt31x86, nt351x86, nt40x86, aix13ps2, aix13ps2-tcf, dellunix22, dgux420x86, sinixz542 | `../pc/release/pack.sh` |
| qemu-system-sparc | sunos414 | `../sun4m/release/mkrelease.sh` |
| qemu-system-m68k | aux311q800 | `../q800/release/mkrelease.sh` |
| qemu-system-m68k | ns08computer, ns09computer, ns10computer, ns10acomputer, ns20cube, ns21color, ns22computer, ns30station, ns31turbo, ns32cube, ns33station, ns33turbocolor, os40cubeturbo, os42color | `../next/release/mkrelease.sh` |

Run them on an Apple Silicon Mac with Homebrew and Docker. The Linux and
Windows builds run in Docker.

## Build

Commit first: the scripts build the working tree. Linux and Windows copy
it without .git and pass the git id, so `--version` still names the
commit.

    sh buildall.sh ../.. OUT

- It runs four builds at once: `build-mac.sh`, `build-linux.sh amd64`,
  `build-linux.sh arm64` and `build-windows.sh`. Each can also run alone.
- Each first runs `get-libaaruformat.sh`, which fetches libaaruformat
  (AaruFormat images) into `subprojects/libaaruformat` at the commit
  `subprojects/libaaruformat.wrap` pins, with its submodules. The Docker
  builds fetch nothing. A checkout already there must be at that commit,
  or the build stops: remove it and run again.
- The binaries and their libraries land in `OUT/macos-arm64`,
  `OUT/linux-x86_64`, `OUT/linux-aarch64` and `OUT/windows-x86_64`.
- `OUT/rc` must show exit code 0 for all four. The logs are in
  `OUT/*.log`.
- Each binary has every feature that any of the five machine builds had,
  and the RDP server (`-rdp`, see `ui/rdp/README.md`).
  qemu-system-hppa, qemu-system-i386, qemu-system-sparc and
  qemu-system-m68k get the same set.
- `build-mac.sh` prints the oldest macOS the binaries run on
  (`needs macOS ...`). The kit READMEs must say the same.

## Sign (macOS)

`build-mac.sh` signs the binaries and libraries in `OUT/macos-arm64` with
the Developer ID and has Apple notarize them (`sign-mac.sh`), so macOS
runs a downloaded kit without asking. It needs the network, and Apple
takes a few minutes.

- The identity is in `.env` in this folder: copy `.env.example` and fill
  it in. Git ignores `.env`, and the Linux and Windows builds leave it
  out of their copy of the tree. The key and the notary password stay in
  the keychain.
- `OUT/mac.log` must show `notarize: Accepted` and `check mac: OK`.
- The pack scripts run `sign-mac.sh -c` on the macOS binaries first, and
  stop if they are not signed and notarized.
- To sign binaries from an older build or an unpacked kit (no rebuild):
  `sh sign-mac.sh DIR`, where DIR is `OUT/macos-arm64` or the kit's
  `bin/macos-arm64` (the MIPS kits: `qemu/macos-arm64`). Then pack.
- `SIGN=adhoc` signs ad hoc, as before, for a build that never goes into
  a kit. The pack scripts refuse those binaries.
- A downloaded kit's binaries cannot carry their ticket (only apps,
  disk images and installers can), so the first start looks it up online.
  Offline, macOS may still block them: the kit READMEs keep the
  `xattr -cr .` hint.

## Feature check

`features.sh`, the same as in `rm200/release` and `jensen/release`, lists
what each binary has in `OUT/macos-*.features` and
`OUT/linux-*.features`.

- MIPS and Alpha: run the machine's `check.sh` on each list, for example
  `sh ../rm200/release/check.sh macos qemu-system-mips64el OUT/macos-qemu-system-mips64el.features`.
- NeXT: `sh ../next/release/check.sh macos OUT/macos-qemu-system-m68k.features`
  (and `linux` with the Linux lists) checks what the NeXT kits need.
- BeBox: `sh ../bebox/release/check.sh macos OUT/macos-qemu-system-ppc.features`
  (and `linux` with the Linux lists) checks what the BeBox kits need.
- The other PowerPC kits have no saved lists yet. Run `features.sh` on
  the binaries of the last kits and compare: `comm -23 OLD NEW` must
  print nothing.
- The Windows build is not run. `build-windows.sh` checks the configure
  summary and the git id instead.

## Stage and pack

    sh stage.sh OUT STAGE

`stage.sh` makes `STAGE` new. `STAGE/i386`, `STAGE/ppc`, `STAGE/ppc64`, `STAGE/alpha`,
`STAGE/hppa`, `STAGE/sparc` and `STAGE/m68k` each hold only their own binary. `STAGE/mips` has the
folder names that `rm200/release/pack.sh` wants. Give the pack scripts
these folders in place of their own build, for example:

    sh ../40p/release/mkrelease.sh nt VMDIR STAGE/ppc nt40powerpc-lemu.tar.lz
    sh ../7006/release/mkrelease.sh VMDIR STAGE/ppc aix325rs6000-lemu.tar.lz
    sh ../bebox/release/mkrelease.sh r45 VMDIR STAGE/ppc beosr45bebox-lemu.tar.lz
    sh ../pseries/release/mkrelease.sh aix53 VMDIR STAGE/ppc64 aix53pseries-lemu.tar.lz
    sh ../jensen/release/pack.sh nt40 SRCDIR STAGE/alpha nt40axp-lemu.tar.lz
    sh ../pc/release/pack.sh nt31 VMDIR STAGE/i386 nt31x86-lemu.tar.lz
    sh ../pc/release/pack.sh nt351 VMDIR STAGE/i386 nt351x86-lemu.tar.lz
    sh ../pc/release/pack.sh nt40 VMDIR STAGE/i386 nt40x86-lemu.tar.lz
    sh ../pc/release/pack.sh aix13 VMDIR STAGE/i386 aix13ps2-lemu.tar.lz
    sh ../pc/release/pack.sh aix13tcf VMDIR STAGE/i386 aix13ps2-tcf-lemu.tar.lz
    sh ../pc/release/pack.sh dellunix VMDIR STAGE/i386 dellunix22-lemu.tar.lz
    sh ../pc/release/pack.sh dgux VMDIR STAGE/i386 dgux420x86-lemu.tar.lz
    sh ../pc/release/pack.sh sinixz VMDIR STAGE/i386 sinixz542-lemu.tar.lz
    sh ../rm200/release/pack.sh KITDIR qemu-system-mips64el STAGE/mips nt4-rm200-lemu.tar.lz
    sh ../hppa/release/mkrelease.sh hpux1020 VMDIR STAGE/hppa hpux1020b160l-lemu.tar.lz
    sh ../hppa/release/mkrelease.sh hpux905 VMDIR STAGE/hppa hpux905-715-lemu.tar.lz
    sh ../hppa/release/mkrelease.sh hpux907 VMDIR STAGE/hppa hpux907-715-lemu.tar.lz
    sh ../hppa/release/mkrelease.sh hpux1100 VMDIR STAGE/hppa hpux1100b160l-lemu.tar.lz
    sh ../hppa/release/mkrelease.sh hpux11v1 VMDIR STAGE/hppa hpux11v1b160l-lemu.tar.lz
    sh ../hppa/release/mkrelease.sh hpux11v2 VMDIR STAGE/hppa hpux11v2c3700-lemu.tar.lz
    sh ../hppa/release/mkrelease.sh hpux11v3 VMDIR STAGE/hppa hpux11v3a400-lemu.tar.lz
    sh ../sun4m/release/mkrelease.sh sunos414 VMDIR STAGE/sparc sunos414-lemu.tar.lz
    sh ../q800/release/mkrelease.sh aux311 VMDIR STAGE/m68k aux311q800-lemu.tar.lz
    sh ../next/release/mkrelease.sh ns33station VMDIR STAGE/m68k ns33station-lemu.tar.lz

- `VMDIR`, `SRCDIR` and `KITDIR` hold the guest files: disk, NVRAM and
  firmware. See each machine's notes.
- Pack one kit at a time: each pack runs `plzip -9`, which uses all
  cores. The SunOS pack script names the kit's folder after the file, and
  packs with gzip when the name ends in `.tar.gz`.

## Test

- Boot each kit on the Mac. Move the mouse, click something, check that
  the monitor answers, and look at every screen, not only the main one.
- Linux: `../rm200/release/ltest.sh`, `../jensen/release/ltest.sh` and,
  for the PowerPC, HP-UX, SunOS and A/UX kits,
  `ltest-ppc.sh amd64|arm64 KITCOPY BINARY`. Use
  copies of the unpacked kits: a boot writes to the disk.
- The Windows build is not run.
