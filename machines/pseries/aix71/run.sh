#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")" || exit 1
case "$(uname -s)-$(uname -m)" in
Darwin-arm64) q=bin/macos-arm64/qemu-system-ppc64 ;;
Linux-x86_64) q=bin/linux-x86_64/qemu-system-ppc64 ;;
Linux-aarch64) q=bin/linux-aarch64/qemu-system-ppc64 ;;
*) echo "No QEMU binary for $(uname -sm)" >&2; exit 1 ;;
esac
# SLOF runs nvramrc at every boot. It crashes if nvramrc is longer than about
# 1000 characters, so the Forth is in variables that nvramrc evaluates.
# aixrc1: device-tree fixes AIX needs
AIXRC1=(
	# /vdevice interrupt ranges
	'dev /vdevice 1100 encode-int 100 encode-int encode+ s" interrupt-ranges" property device-end'
	# XICS presentation node with "reg" (AIX 5.3 asserts in init_extint_chrp without it)
	'dev / new-device s" xicp" device-name'
	's" PowerPC-External-Interrupt-Presentation" encode-string s" device_type" property'
	's" IBM,ppc-xicp" encode-string s" compatible" property'
	'3ffe encode-int 0 encode-int encode+ 0 encode-int encode+ 1000 encode-int encode+ s" reg" property'
	'0 encode-int 1 encode-int encode+ s" ibm,interrupt-server-ranges" property finish-device device-end'
	# partition number and name (vscsi client needs them), system-id (machine ID for uname -m)
	'dev / 1 encode-int s" ibm,partition-no" property'
	's" aix71" encode-string s" ibm,partition-name" property'
	's" IBM,0210A71A1" encode-string s" system-id" property device-end'
	# a "load" method on the CD and disk nodes, so AIX sees them as bootable
	'dev /vdevice/v-scsi@71000003/disk@8100000000000000 : load drop 0 ; device-end'
	'dev /vdevice/v-scsi@71000004/disk@8100000000000000 : load drop 0 ; device-end'
)
# aixrc2: fixes for the PCI graphics and USB
AIXRC2=(
	# DMA window of the PCI host bridge in the format AIX reads. SLOF rebuilds the
	# tree when AIX calls client-architecture-support, so set it again after that.
	'device-end : aixdma'
	'80000000 encode-int 0 encode-int encode+ 0 encode-int encode+ 0 encode-int encode+ 0 encode-int encode+ 40000000 encode-int encode+'
	's" ibm,dma-window" s" /pci@800000020000000" find-node set-property'
	'3 encode-int s" ibm,#dma-address-cells" s" /pci@800000020000000" find-node set-property ;'
	'dev / : ibm,client-architecture-support ibm,client-architecture-support aixdma ; device-end aixdma'
	# ES1000 init tables that IBM firmware has on the graphics node (AIX needs them, empty is fine)
	'dev /pci@800000020000000/vga@0 0 0 s" ibm,init1" property 0 0 s" ibm,init2" property'
	'0 0 s" ibm,pllinit" property 0 0 s" ibm,meminit" property device-end'
	# DMA window of the USB controller
	'dev /pci@800000020000000/usb-ohci@1'
	'80000000 encode-int 0 encode-int encode+ 0 encode-int encode+ 0 encode-int encode+ 40000000 encode-int encode+'
	's" ibm,dma-window" property device-end'
)
# aixrc3, aixrc4: a display driver for the ES1000, so the firmware and the AIX boot banner
# show on the screen. 640x480, 8 bits, the mode and palette of the JS21 SLOF driver.
AIXRC3=(
	'dev /pci@800000020000000/vga@0 7 4 config-w!'
	'0 0 2000010 1 " map-in" get-node parent $call-static value fb'
	'0 0 2000018 1 " map-in" get-node parent $call-static value rg'
	': ar! rg + rl!-le ; : ar@ rg + rl@-le ; : p b4 ar! ;'
	'4f0063 200 ar! 8c02a2 204 ar! 1df020c 208 ar! 8201ea 20c ar! 0 224 ar! 50 22c ar!'
	'50 ar@ f8fff0ff and 3000200 or 50 ar! 8000 54 ar!'
	'0 b0 rg + rb! 0 p aa p aa00 p aaaa p aa0000 p aa00aa p aa5500 p aaaaaa p'
	'555555 p 5555ff p 55ff55 p 55ffff p ff5555 p ff55ff p ffff55 p ffffff p'
	'ff b0 rg + rb! ffffff p fb 4b000 0 rfill device-end'
)
AIXRC4=(
	'dev /pci@800000020000000/vga@0 false value inst?'
	': display-install inst? 0= IF fb to frame-buffer-adr default-font set-font'
	'280 1e0 50 1e 1 fb-install true to inst? THEN ;'
	": display-remove ; ' display-install is-install ' display-remove is-remove"
	's" display" device-type 280 encode-int s" width" property 1e0 encode-int s" height" property'
	'8 encode-int s" depth" property 280 encode-int s" linebytes" property'
	's" screen" get-node node>path set-alias device-end'
	# after CAS AIX no longer uses the firmware console: give input back to the serial
	# port and stop the USB controller, so AIX finds it idle (no usbhc0 error)
	'dev / : ibm,client-architecture-support ibm,client-architecture-support " hvterm" input'
	's" hc-cleanup" s" /pci@800000020000000/usb-ohci@1" find-node find-method IF execute THEN ; device-end'
)

# AIX scans virtual SCSI from target 1 (LUN 0x81), so CD and disk use scsi-id=1.
# Keep id=hd0: QEMU sends it as the disk's unique ID, and AIX boots only from that ID.
# Keep the graphics in PCI slot 0 and USB in slot 1: aixrc2 uses those node names.
# guest_hwcursor=true: QEMU draws the AIX mouse cursor into the screen. With the host's
# pointer as the cursor instead, the mouse jumps back and forth.
# The AIX console is the screen, so the serial port goes nowhere (-serial null)
# and the terminal holds the QEMU monitor.
exec "$q" \
 -name "AIX 7.1 POWER PSERIES" \
 -M pseries,panel=on,ic-mode=xics,usb=off,max-cpu-compat=power6,cap-hpt-max-page-size=16M \
 -cpu power7_v2.3 -smp 1 -m 4G \
 -monitor stdio \
 -serial null \
 -vga none \
 -prom-env input-device=keyboard \
 -prom-env output-device=screen \
 -prom-env 'use-nvramrc?=true' \
 -prom-env "aixrc1=${AIXRC1[*]}" \
 -prom-env "aixrc2=${AIXRC2[*]}" \
 -prom-env "aixrc3=${AIXRC3[*]}" \
 -prom-env "aixrc4=${AIXRC4[*]}" \
 -prom-env 'nvramrc=aixrc1 evaluate aixrc2 evaluate aixrc3 evaluate aixrc4 evaluate' \
 -device spapr-vscsi,id=vscsi0 \
 -drive if=none,id=cd0,media=cdrom,readonly=on \
 -device scsi-cd,bus=vscsi0.0,drive=cd0,scsi-id=1,vendor=AIX,product=VOPTA \
 -device spapr-vscsi,id=vscsi1 \
 -drive file=aix71.qcow2,if=none,id=hd0,format=qcow2 \
 -device scsi-hd,bus=vscsi1.0,drive=hd0,scsi-id=1,vendor=AIX,product=VDASD \
 -boot order=c \
 -device ati-vga,model=es1000,romfile=,addr=0,guest_hwcursor=true \
 -device nec-usb-ohci,id=usb0,addr=1 \
 -device usb-kbd,bus=usb0.0 \
 -device usb-mouse,bus=usb0.0 \
 -nic user,model=spapr-vlan,hostfwd=tcp:127.0.0.1:7123-:23,hostfwd=tcp:127.0.0.1:7114-:514 \
 "$@"
