# AIX 3.2.5 on the IBM RS/6000 7006-41T

`run.sh` boots AIX from a QEMU build in `qemu/`, with the install CDs and the
firmware in `media/` (see its header). `tools/` has the scripts used to bring
the machine up: console, traces, memory dumps and XCOFF disassembly.

QEMU halts the CPU in AIX's idle loop, so an idle VM uses 6-15% of a host
core instead of a whole one. `-M 7006,idle=off` lets the loop spin instead.

## Release

`release/` builds the ready-to-run `aix325rs6000-lemu.tar.lz`:
`build-linux.sh amd64|arm64 OUT` and `build-windows.sh OUT` (Docker), and
`build-macos.sh OUT` (Homebrew glib, pixman, libslirp, sdl2-compat, sdl3,
dylibbundler) build self-contained QEMU binaries into OUT, then `mkrelease.sh
VMDIR OUT aix325rs6000-lemu.tar.lz` packs them with the VM's disk
(`disk/hd0.qcow2`), NVRAM (`disk/nvram.bin`), firmware (`fw/`) and the
`run.sh`, `run.cmd` and `README.txt` in `aix325/`. SDL is the display on
every platform, as only it shows the operator panel in a window of its own.
