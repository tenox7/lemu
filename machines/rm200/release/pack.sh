#!/bin/sh
# Put the release binaries and the run scripts into an unpacked kit and
# pack it as .tar.lz
# usage: pack.sh KITDIR BINARY RELBIN OUT.tar.lz
#   KITDIR  the kit folder (run.sh, run.cmd, README.txt, disk, firmware);
#           its qemu/ folder is replaced, and run.sh and run.cmd come from
#           machines/<machine>/<guest>, picked by the kit's folder name.
#           A folder still named <name>-qemu (unpacked from a kit made
#           before 2026-11-12) goes into the tarball as <name>-lemu.
#   BINARY  qemu-system-mips64el (NT kits) or qemu-system-mips64 (SINIX)
#   RELBIN  the OUT folder of the build scripts
set -e
[ $# -eq 4 ] || { echo "usage: $0 KITDIR BINARY RELBIN OUT.tar.lz" >&2; exit 1; }
for d in "$1" "$3" "$(dirname "$4")"; do
    [ -d "$d" ] || { echo "$0: no folder $d" >&2; exit 1; }
done
[ -f "$1/run.sh" ] || { echo "$0: $1 has no run.sh, so it is not a kit" >&2; exit 1; }
kit=$(cd "$1" && pwd)
name=$(basename "$kit")
case $name in
*-qemu) store=${name%-qemu}-lemu ;;
*) store=$name ;;
esac
case $store in
nt31mips-lemu) g=jazz/nt31 ;;
nt4-jazz-lemu) g=jazz/nt40 ;;
nt351-jazz-lemu) g=jazz/nt351 ;;
NTRM200) g=rm200/nt40 ;;
nt351-rm200-lemu) g=rm200/nt351 ;;
NTRM200-SMP) g=rm200/nt40smp ;;
NTRM300-SMP4) g=rm200/nt40srv ;;
sinix-rm200) g=rm200/sinix545 ;;
*) echo "$0: no run scripts for kit $name; add it here" >&2; exit 1 ;;
esac
src=$(cd "$(dirname "$0")/../.." && pwd)/$g
bin=$2
rel=$(cd "$3" && pwd)
out=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")
q=$kit/qemu
rm -rf "$q"
mkdir -p "$q/macos-arm64" "$q/linux-x64" "$q/windows-x64"
cp -a "$rel/macos-arm64/$bin" "$rel/macos-arm64/lib" "$q/macos-arm64/"
cp -a "$rel/linux-x64/$bin" "$rel/linux-x64/lib" "$q/linux-x64/"
cp -a "$rel/windows-x64/$bin.exe" "$rel/windows-x64/"*.dll "$q/windows-x64/"
cp "$src/run.sh" "$kit/run.sh"
sed 's/$/\r/' "$src/run.cmd" > "$kit/run.cmd"
cd "$(dirname "$kit")"
COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 \
    --uname root --gname root -s "/^$name/$store/" -cf - "$name" | plzip -9 > "$out.tmp"
lzip -t "$out.tmp"
mv "$out.tmp" "$out"
