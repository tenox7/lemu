#!/bin/sh
cd "$(dirname "$0")"
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) Q=qemu/macos-arm64/qemu-system-mips64el ;;
Linux-x86_64) Q=qemu/linux-x64/qemu-system-mips64el ;;
*) echo "unsupported host: $(uname -sm)" >&2; exit 1 ;;
esac
# -icount: without it the HAL's timer calibration fails, and ntvdm stops at
# logon with "Integer division by zero"
exec $Q \
	-name "Windows NT 4.0 MIPS JAZZ" \
	-M magnum -m 128 \
	-icount shift=3,align=on \
	-bios NTPROM.RAW \
	-global ds1225y.filename=nvram.bin \
	-rtc base=1995-07-08T11:12:13,clock=vm \
	-monitor stdio \
	-nic user \
	-drive file=nt40.qcow2,if=none,id=hd0,format=qcow2 \
	-device scsi-hd,drive=hd0,scsi-id=0 \
	"$@"
