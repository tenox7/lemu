#!/bin/sh
# One Windows x86_64 build of every kit binary, cross-built with MinGW on
# Fedora 43 in Docker: bundle the DLLs and keymaps into OUT/windows-x86_64.
# The Windows build is not run, so the check reads the configure summary
# and looks for the git id in each binary.
# usage: build-windows.sh SRC OUT
set -e
BINS="qemu-system-i386 qemu-system-ppc qemu-system-ppc64 qemu-system-alpha qemu-system-mips64el qemu-system-mips64 qemu-system-hppa qemu-system-sparc"
if [ "$1" != --container ]; then
    [ $# -eq 2 ] || { echo "usage: $0 SRC OUT" >&2; exit 1; }
    mkdir -p "$2"
    exec docker run --rm \
        -e "PKGVERSION=$(git -C "$1" describe --match 'v*' --dirty 2>/dev/null)" \
        -v "$(cd "$1" && pwd):/src:ro" -v "$(cd "$(dirname "$0")" && pwd):/work:ro" \
        -v "$(cd "$2" && pwd):/out" registry.fedoraproject.org/fedora:43 \
        sh /work/build-windows.sh --container
fi
exec 2>/out/windows-x86_64-err.log
set -x
dnf -y -q install gcc mingw64-gcc mingw64-glib2 mingw64-pixman \
    mingw64-sdl2-compat mingw64-SDL3 mingw64-pkg-config make ninja-build \
    python3 git bzip2 diffutils findutils tar >/dev/null
mkdir -p /b/build
tar -C /src --exclude=./.git --exclude='./build*' -cf - . | tar -C /b -xf -
cd /b/build
# the source is copied without .git, so pass the git id for --version
../configure ${PKGVERSION:+--with-pkgversion=$PKGVERSION} \
    --cross-prefix=x86_64-w64-mingw32- \
    --target-list=i386-softmmu,ppc-softmmu,ppc64-softmmu,alpha-softmmu,mips64el-softmmu,mips64-softmmu,hppa-softmmu,sparc-softmmu \
    --without-default-features --enable-system --enable-tcg --enable-hmp --enable-sdl \
    --enable-dsound --enable-pixman --enable-slirp --enable-fdt=internal --enable-vnc \
    --enable-vvfat --enable-qcow1 --enable-vdi --enable-vmdk --enable-vpc --enable-vhdx \
    --enable-qed --enable-bochs --enable-cloop --enable-dmg --enable-parallels \
    --enable-stack-protector --disable-docs > /out/windows-x86_64-configure.log
exes=
for b in $BINS; do exes="$exes $b.exe"; done
ninja $exes > /out/windows-x86_64-ninja.log

out=/out/windows-x86_64
mb=/usr/x86_64-w64-mingw32/sys-root/mingw/bin
rm -rf "$out"
mkdir -p "$out/share/keymaps"
install -m 755 $exes "$mb/SDL3.dll" "$out/"
for e in $exes; do
    x86_64-w64-mingw32-strip "$out/$e"
done
find ../pc-bios/keymaps -type f ! -name meson.build \
    -exec cp {} "$out/share/keymaps/" \;
todo="$out/SDL3.dll"
for e in $exes; do todo="$todo $out/$e"; done
while [ -n "$todo" ]; do
    next=
    for f in $todo; do
        for d in $(x86_64-w64-mingw32-objdump -p "$f" |
                   sed -n 's/^\tDLL Name: //p'); do
            [ -f "$out/$d" ] && continue
            s=$mb/$d
            [ -f "$s" ] || s=$(find . -name "$d" | head -1)
            [ -n "$s" ] || continue
            cp "$s" "$out/"
            next="$next $out/$d"
        done
    done
    todo=$next
done
set +x
fail=0
for f in 'SDL support' 'DirectSound support' 'slirp support'; do
    grep -q "^ *$f *: YES" /out/windows-x86_64-configure.log ||
        { echo "MISSING: $f"; fail=1; }
done
for e in $exes; do
    if [ -z "$PKGVERSION" ] || ! grep -a -q -F "($PKGVERSION)" "$out/$e"; then
        echo "MISSING: git id in $e"
        fail=1
    fi
done
if [ $fail = 0 ]; then
    echo "check windows: OK"
else
    echo "check windows: FAILED"
    exit 1
fi
