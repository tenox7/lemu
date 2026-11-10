#!/bin/sh
cd "$(dirname "$0")"
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) Q=qemu/macos-arm64/qemu-system-mips64el ;;
Linux-x86_64) Q=qemu/linux-x64/qemu-system-mips64el ;;
*) echo "unsupported host: $(uname -sm)" >&2; exit 1 ;;
esac
exec $Q \
	-name "Windows NT 4.0 Server MIPS RM300" \
	-M sni-rm300,nvram=nvram-smp4.bin,scsi-delay=0 \
	-smp 4 \
	-m 128 \
	-rtc base=localtime \
	-accel tcg,thread=multi \
	-bios 'RM200C-NT-SMP.BIN' \
	-monitor stdio \
	-nic user,model=pcnet,hostfwd=tcp:127.0.0.1:44514-:514,hostfwd=tcp:127.0.0.1:44389-:3389 \
	-drive file=nt40srv-smp4.hda,if=none,id=hd0,format=raw \
	-device scsi-hd,drive=hd0,scsi-id=0 \
	-drive if=none,id=cd0,media=cdrom \
	-device scsi-cd,drive=cd0,scsi-id=5 \
	"$@"
