# IBM 40P (7020-40P, PReP)

`-M 40p` runs Windows NT 3.51 and 4.0, OS/2 Warp, Solaris 2.5.1, AIX 4.1.5,
AIX 4.3.3 and AIX 5.1 for PowerPC. Each system has a folder here with the
`run.sh`, `run.cmd` and `README.txt` of its ready-to-run tarball: `nt351`,
`nt`, `os2`, `solaris`, `aix41`, `aix43` and `aix51`.

QEMU halts the CPU while any of these systems is idle, so an idle VM uses
2-6% of a host core. `-M 40p,idle=off` lets their idle loops spin instead.

## Release

`release/` builds the tarballs: `build-linux.sh amd64|arm64 OUT` and
`build-windows.sh OUT` (Docker), and `build-macos.sh OUT` (Homebrew glib,
pixman, libslirp, sdl2-compat, sdl3, dylibbundler) build self-contained QEMU
binaries into OUT, then `mkrelease.sh nt|nt351|os2|solaris|aix41|aix43|aix51 VMDIR
OUT NAME.tar.lz` packs them with the VM's disk, NVRAM and firmware. SDL is
the display on every platform, as only it shows the front panel in a window
of its own.
