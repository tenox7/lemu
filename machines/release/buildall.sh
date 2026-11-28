#!/bin/sh
# Build every kit binary for the four platforms at once: macOS arm64,
# Linux x86_64 and aarch64, Windows x86_64. The logs go to OUT/*.log and
# the exit codes to OUT/rc.
# usage: buildall.sh SRC OUT
[ $# -eq 2 ] || { echo "usage: $0 SRC OUT" >&2; exit 1; }
SRC=$1 OUT=$2 D=$(cd "$(dirname "$0")" && pwd)
mkdir -p $OUT; : > $OUT/rc
# The builds run at once: get libaaruformat once, before them
sh $D/get-libaaruformat.sh $SRC || exit 1
(sh $D/build-mac.sh $SRC $OUT > $OUT/mac.log 2>&1; echo "mac $?" >> $OUT/rc) &
(sh $D/build-linux.sh amd64 $SRC $OUT > $OUT/amd64.log 2>&1; echo "amd64 $?" >> $OUT/rc) &
(sh $D/build-linux.sh arm64 $SRC $OUT > $OUT/arm64.log 2>&1; echo "arm64 $?" >> $OUT/rc) &
(sh $D/build-windows.sh $SRC $OUT > $OUT/win.log 2>&1; echo "win $?" >> $OUT/rc) &
wait
cat $OUT/rc
