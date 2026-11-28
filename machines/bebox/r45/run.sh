#!/bin/sh
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-ppc ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-ppc ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-ppc ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# SDL on macOS drops the click that focuses a window
export SDL_MOUSE_FOCUS_CLICKTHROUGH=1
exec "$q" \
 -name "BeOS R4.5 POWERPC BEBOX" \
 -M bebox -m 32 \
 -drive if=pflash,file=flash.bin,format=raw \
 -monitor stdio \
 -drive file=beos.qcow2,if=none,id=hd0,format=qcow2 \
 -device ide-hd,drive=hd0,bus=ide.0,unit=0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device ide-cd,drive=cd0,bus=ide.0,unit=1 \
 -nic none -netdev user,id=n0 -device tulip-21041,netdev=n0,addr=0e \
 -serial null -serial null -serial null -serial vc \
 "$@"
