#!/bin/sh
cd "$(dirname "$0")"
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) Q=qemu/macos-arm64/qemu-system-mips64el ;;
Linux-x86_64) Q=qemu/linux-x64/qemu-system-mips64el ;;
*) echo "unsupported host: $(uname -sm)" >&2; exit 1 ;;
esac
exec $Q \
	-name "Windows NT 3.51 MIPS RM200" \
	-M sni-rm200,nvram=nvram.bin \
	-m 128 \
	-icount shift=3,align=on \
	-rtc base=localtime \
	-bios 'RM200C-NT-AM29F040@PLCC32.BIN' \
	-monitor stdio \
	-nic user,model=pcnet \
	-drive file=nt351.qcow2,if=none,id=hd0,format=qcow2 \
	-device scsi-hd,drive=hd0,scsi-id=0 \
	-drive if=none,id=cd0,media=cdrom,readonly=on \
	-device scsi-cd,drive=cd0,scsi-id=5 \
	"$@"
