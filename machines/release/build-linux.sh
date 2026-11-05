#!/bin/sh
# One Linux build of every kit binary on AlmaLinux 9 (glibc 2.34, Python 3.12)
# in Docker: bundle the libraries into OUT/linux-x86_64 (OUT/linux-aarch64
# for arm64) and list each binary's features.
# Feature set = everything the 40p, 7006, pseries, jensen and rm200 builds had;
# qemu-system-hppa (HP B160L) gets the same.
# usage: build-linux.sh amd64|arm64 SRC OUT
set -e
BINS="qemu-system-ppc qemu-system-ppc64 qemu-system-alpha qemu-system-mips64el qemu-system-mips64 qemu-system-hppa"
if [ "$1" != --container ]; then
    [ $# -eq 3 ] || { echo "usage: $0 amd64|arm64 SRC OUT" >&2; exit 1; }
    mkdir -p "$3"
    exec docker run --rm --platform "linux/$1" \
        -e "PKGVERSION=$(git -C "$2" describe --match 'v*' --dirty 2>/dev/null)" \
        -v "$(cd "$2" && pwd):/src:ro" -v "$(cd "$(dirname "$0")" && pwd):/work:ro" \
        -v "$(cd "$3" && pwd):/out" almalinux:9 sh /work/build-linux.sh --container
fi
out=/out/linux-$(uname -m)
dnf -y -q install epel-release dnf-plugins-core
dnf config-manager --set-enabled crb
dnf -y -q install gcc-toolset-13-gcc gcc-toolset-13-binutils make ninja-build \
    python3.12 glib2-devel pixman-devel SDL2-devel libslirp-devel zlib-devel \
    pulseaudio-libs-devel alsa-lib-devel \
    diffutils findutils patchelf bzip2 tar
. /opt/rh/gcc-toolset-13/enable
mkdir -p /b/build
tar -C /src --exclude=./.git --exclude='./build*' -cf - . | tar -C /b -xf -
cd /b/build
# the source is copied without .git, so pass the git id for --version
../configure ${PKGVERSION:+--with-pkgversion=$PKGVERSION} --python=/usr/bin/python3.12 \
    --target-list=ppc-softmmu,ppc64-softmmu,alpha-softmmu,mips64el-softmmu,mips64-softmmu,hppa-softmmu \
    --without-default-features --enable-system --enable-tcg --enable-hmp --enable-sdl \
    --enable-pixman --enable-slirp --enable-pa --enable-alsa --enable-malloc-trim --enable-membarrier \
    --enable-fdt=internal --enable-vnc --enable-vvfat --enable-qcow1 --enable-vdi --enable-vmdk \
    --enable-vpc --enable-vhdx --enable-qed --enable-bochs --enable-cloop --enable-dmg --enable-parallels \
    --enable-stack-protector --disable-docs > $out-configure.log
ninja $BINS > $out-ninja.log

rm -rf "$out"
mkdir -p "$out/lib"
for b in $BINS; do
    install -m 755 -s $b "$out/"
done
for l in $(ldd $BINS | awk '$3 ~ /^\// {print $3}' | sort -u); do
    case ${l##*/} in
    ld-linux*|libc.so*|libm.so*|libdl.so*|librt.so*|libpthread.so*|\
    libresolv.so*|libgcc_s.so*|libz.so*|libX11.so*|libxcb.so*|libXau.so*)
        continue ;;
    esac
    cp -L "$l" "$out/lib/"
done
for b in $BINS; do
    patchelf --force-rpath --set-rpath '$ORIGIN/lib' "$out/$b"
done
for l in "$out"/lib/*; do
    patchelf --force-rpath --set-rpath '$ORIGIN' "$l"
done
for b in $BINS; do
    sh /work/features.sh "$out/$b" > "$out-$b.features"
done
echo "build linux-$(uname -m): OK"
