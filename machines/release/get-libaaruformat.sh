#!/bin/sh
# Gets libaaruformat (AaruFormat images) into SRC/subprojects, at the commit
# subprojects/libaaruformat.wrap pins, with its submodules. The Docker
# builds copy the tree and fetch nothing, so this runs on the host first.
# A checkout that is already there must be at that commit.
# usage: get-libaaruformat.sh SRC
#
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
set -e
[ $# -eq 1 ] || { echo "usage: $0 SRC" >&2; exit 1; }
W=$1/subprojects/libaaruformat.wrap L=$1/subprojects/libaaruformat
URL=$(sed -n 's/^url *= *//p' "$W")
REV=$(sed -n 's/^revision *= *//p' "$W")
if [ ! -d "$L" ]; then
    git init -q "$L"
    git -C "$L" fetch -q --depth 1 "$URL" "$REV"
    git -C "$L" -c advice.detachedHead=false checkout -q FETCH_HEAD
    git -C "$L" submodule -q update --init --recursive --depth 1
fi
HEAD=$(git -C "$L" rev-parse HEAD)
[ "$HEAD" = "$REV" ] ||
    { echo "$L is at $HEAD, not $REV: remove it and run again" >&2; exit 1; }
