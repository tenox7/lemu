#!/bin/sh
# One macOS arm64 build of every kit binary: SDL (the default display) and
# Cocoa, Core Audio, vmnet. Bundles the Homebrew dylibs into
# OUT/macos-arm64/lib and lists each binary's features.
# Feature set = everything the 40p, 7006, pseries, jensen and rm200 builds had;
# qemu-system-hppa (HP B160L) and qemu-system-i386 (PC) get the same.
# usage: build-mac.sh SRC OUT
set -e
[ $# -eq 2 ] || { echo "usage: $0 SRC OUT" >&2; exit 1; }
R=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
O=$OUT/macos-arm64 B=$OUT/build-mac
BINS="qemu-system-i386 qemu-system-ppc qemu-system-ppc64 qemu-system-alpha qemu-system-mips64el qemu-system-mips64 qemu-system-hppa"
export MACOSX_DEPLOYMENT_TARGET=15.0
mkdir -p $B && cd $B
$SRC/configure --target-list=i386-softmmu,ppc-softmmu,ppc64-softmmu,alpha-softmmu,mips64el-softmmu,mips64-softmmu,hppa-softmmu \
  --without-default-features --enable-system --enable-tcg --enable-hmp \
  --enable-sdl --enable-cocoa --enable-coreaudio --enable-vmnet --enable-slirp --enable-pixman \
  --enable-fdt=internal --enable-vnc --enable-vvfat --enable-qcow1 --enable-vdi --enable-vmdk \
  --enable-vpc --enable-vhdx --enable-qed --enable-bochs --enable-cloop --enable-dmg --enable-parallels \
  --disable-docs --disable-werror --disable-capstone > configure.log
ninja $BINS > ninja.log
rm -rf $O && mkdir -p $O/lib && cp $BINS $O/ && cd $O
todo=$BINS
while [ -n "$todo" ]; do
  f=${todo%% *}; [ "$f" = "$todo" ] && todo= || todo=${todo#* }
  for d in $(otool -L "$f" | awk 'NR>1 {print $1}' | grep '^/opt/homebrew'); do
    n=$(basename $d)
    [ -e lib/$n ] || { cp $d lib/ && chmod u+w lib/$n && todo="$todo lib/$n"; todo=${todo# }; }
    install_name_tool -change $d @executable_path/lib/$n "$f" 2>/dev/null
  done
done
# sdl2-compat loads SDL3 at run time from its own directory
install -m 644 "$(brew --prefix sdl3)/lib/libSDL3.0.dylib" lib/libSDL3.dylib
for l in lib/*; do install_name_tool -id @executable_path/$l $l 2>/dev/null; codesign -f -s - $l 2>/dev/null; done
for b in $BINS; do
  codesign -f -s - --entitlements $SRC/accel/hvf/entitlements.plist $b 2>/dev/null
done
if otool -L $BINS lib/*.dylib | grep -q '/opt/homebrew'; then
  echo "MISSING: a Homebrew library is still linked from /opt/homebrew"
  exit 1
fi
# the newest macOS any of the files needs; README.txt says which macOS the kit runs on
for f in $BINS lib/*.dylib; do
  otool -l $f | awk '/LC_BUILD_VERSION/ {v=1} v && $1 == "minos" {print $2; exit}'
done | sort -t. -k1,1n -k2,2n | tail -1 | sed 's/^/needs macOS /'
for b in $BINS; do
  sh $R/features.sh $O/$b > $OUT/macos-$b.features
done
echo "build mac: OK"
