#!/bin/sh
# Make per-binary folders for the kit pack scripts from one build (OUT of
# buildall.sh). i386, ppc, ppc64, alpha, hppa: STAGE/<bin>/<platform> holds only that
# binary and lib/ (macOS, Linux), or that .exe, the DLLs and share/
# (Windows), as cp -c clones. mips: links with the names
# rm200/release/pack.sh wants; it copies only the binary it is given.
# usage: stage.sh OUT STAGE
set -e
[ $# -eq 2 ] || { echo "usage: $0 OUT STAGE" >&2; exit 1; }
ONE=$(cd "$1" && pwd); T=$2
rm -rf "$T"; mkdir -p "$T/mips"
for b in i386 ppc ppc64 alpha hppa; do
    for p in macos-arm64 linux-x86_64 linux-aarch64 windows-x86_64; do
        mkdir -p "$T/$b/$p"
        for f in "$ONE/$p"/*; do
            case ${f##*/} in
            qemu-system-$b|qemu-system-$b.exe) ;;
            qemu-system-*) continue ;;
            esac
            cp -cR "$f" "$T/$b/$p/"
        done
    done
done
ln -s "$ONE/macos-arm64" "$T/mips/macos-arm64"
ln -s "$ONE/linux-x86_64" "$T/mips/linux-x64"
ln -s "$ONE/windows-x86_64" "$T/mips/windows-x64"
for b in i386 ppc ppc64 alpha hppa; do
    for p in macos-arm64 linux-x86_64 linux-aarch64 windows-x86_64; do
        printf '%s/%s: %s files  ' $b $p "$(find "$T/$b/$p" -type f | wc -l | tr -d ' ')"
    done
    echo
done
