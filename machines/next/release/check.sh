#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Check a feature list from ../../release/features.sh against required.txt.
# usage: check.sh PLATFORM FEATURES.txt   (PLATFORM is macos or linux)
d=$(cd "$(dirname "$0")" && pwd)
[ $# -eq 2 ] || { echo "usage: $0 PLATFORM FEATURES.txt" >&2; exit 1; }
plat=$1 new=$2
fail=0
while read -r where kind name why; do
    case $where in ''|'#'*) continue ;; esac
    case $where in all|"$plat") ;; *) continue ;; esac
    if ! grep -qx "$kind $name" "$new"; then
        echo "MISSING: $kind $name $why"
        fail=1
    fi
done < "$d/required.txt"
[ $fail -eq 0 ] && echo "check $plat: OK"
exit $fail
