#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Make pc-bios/QEMU,cgthree.bin from cgthree.fs with toke, the FCode tokenizer of fcode-utils.
# Uses toke if it is installed, else a Debian container (Docker).
cd "$(dirname "$0")" || exit 1
out=../../../pc-bios/QEMU,cgthree.bin
if command -v toke >/dev/null; then
    toke -o cgthree.bin cgthree.fs || exit 1
else
    docker run --rm -v "$PWD":/work -w /work debian:12 sh -c '
        apt-get update -qq >/dev/null && apt-get install -y -qq fcode-utils >/dev/null &&
        toke -o cgthree.bin cgthree.fs' || exit 1
fi
mv cgthree.bin "$out" && ls -l "$out"
