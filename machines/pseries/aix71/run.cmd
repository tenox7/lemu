@echo off
cd /d "%~dp0"
rem SLOF runs nvramrc at every boot; it evaluates the device-tree fixes for AIX
rem in aixrc1, the PCI graphics and USB fixes in aixrc2 and the ES1000 display
rem driver for the firmware in aixrc3 and aixrc4 (see run.sh).
bin\windows-x86_64\qemu-system-ppc64.exe ^
 -name "AIX 7.1 POWER PSERIES" ^
 -M pseries,panel=on,ic-mode=xics,usb=off,max-cpu-compat=power6,cap-hpt-max-page-size=16M ^
 -cpu power7_v2.3 -smp 1 -m 4G ^
 -monitor stdio ^
 -serial null ^
 -vga none ^
 -prom-env input-device=keyboard ^
 -prom-env output-device=screen ^
 -prom-env use-nvramrc?=true ^
 -prom-env "aixrc1=dev /vdevice 1100 encode-int 100 encode-int encode+ s\" interrupt-ranges\" property device-end dev / new-device s\" xicp\" device-name s\" PowerPC-External-Interrupt-Presentation\" encode-string s\" device_type\" property s\" IBM,ppc-xicp\" encode-string s\" compatible\" property 3ffe encode-int 0 encode-int encode+ 0 encode-int encode+ 1000 encode-int encode+ s\" reg\" property 0 encode-int 1 encode-int encode+ s\" ibm,interrupt-server-ranges\" property finish-device device-end dev / 1 encode-int s\" ibm,partition-no\" property s\" aix71\" encode-string s\" ibm,partition-name\" property s\" IBM,0210A71A1\" encode-string s\" system-id\" property device-end dev /vdevice/v-scsi@71000003/disk@8100000000000000 : load drop 0 ; device-end dev /vdevice/v-scsi@71000004/disk@8100000000000000 : load drop 0 ; device-end" ^
 -prom-env "aixrc2=device-end : aixdma 80000000 encode-int 0 encode-int encode+ 0 encode-int encode+ 0 encode-int encode+ 0 encode-int encode+ 40000000 encode-int encode+ s\" ibm,dma-window\" s\" /pci@800000020000000\" find-node set-property 3 encode-int s\" ibm,#dma-address-cells\" s\" /pci@800000020000000\" find-node set-property ; dev / : ibm,client-architecture-support ibm,client-architecture-support aixdma ; device-end aixdma dev /pci@800000020000000/vga@0 0 0 s\" ibm,init1\" property 0 0 s\" ibm,init2\" property 0 0 s\" ibm,pllinit\" property 0 0 s\" ibm,meminit\" property device-end dev /pci@800000020000000/usb-ohci@1 80000000 encode-int 0 encode-int encode+ 0 encode-int encode+ 0 encode-int encode+ 40000000 encode-int encode+ s\" ibm,dma-window\" property device-end" ^
 -prom-env "aixrc3=dev /pci@800000020000000/vga@0 7 4 config-w! 0 0 2000010 1 \" map-in\" get-node parent $call-static value fb 0 0 2000018 1 \" map-in\" get-node parent $call-static value rg : ar! rg + rl!-le ; : ar@ rg + rl@-le ; : p b4 ar! ; 4f0063 200 ar! 8c02a2 204 ar! 1df020c 208 ar! 8201ea 20c ar! 0 224 ar! 50 22c ar! 50 ar@ f8fff0ff and 3000200 or 50 ar! 8000 54 ar! 0 b0 rg + rb! 0 p aa p aa00 p aaaa p aa0000 p aa00aa p aa5500 p aaaaaa p 555555 p 5555ff p 55ff55 p 55ffff p ff5555 p ff55ff p ffff55 p ffffff p ff b0 rg + rb! ffffff p fb 4b000 0 rfill device-end" ^
 -prom-env "aixrc4=dev /pci@800000020000000/vga@0 false value inst? : display-install inst? 0= IF fb to frame-buffer-adr default-font set-font 280 1e0 50 1e 1 fb-install true to inst? THEN ; : display-remove ; ' display-install is-install ' display-remove is-remove s\" display\" device-type 280 encode-int s\" width\" property 1e0 encode-int s\" height\" property 8 encode-int s\" depth\" property 280 encode-int s\" linebytes\" property s\" screen\" get-node node>path set-alias device-end dev / : ibm,client-architecture-support ibm,client-architecture-support \" hvterm\" input s\" hc-cleanup\" s\" /pci@800000020000000/usb-ohci@1\" find-node find-method IF execute THEN ; device-end" ^
 -prom-env "nvramrc=aixrc1 evaluate aixrc2 evaluate aixrc3 evaluate aixrc4 evaluate" ^
 -device spapr-vscsi,id=vscsi0 ^
 -drive if=none,id=cd0,media=cdrom,readonly=on ^
 -device scsi-cd,bus=vscsi0.0,drive=cd0,scsi-id=1,vendor=AIX,product=VOPTA ^
 -device spapr-vscsi,id=vscsi1 ^
 -drive file=aix71.qcow2,if=none,id=hd0,format=qcow2 ^
 -device scsi-hd,bus=vscsi1.0,drive=hd0,scsi-id=1,vendor=AIX,product=VDASD ^
 -boot order=c ^
 -device ati-vga,model=es1000,romfile=,addr=0,guest_hwcursor=true ^
 -device nec-usb-ohci,id=usb0,addr=1 ^
 -device usb-kbd,bus=usb0.0 ^
 -device usb-mouse,bus=usb0.0 ^
 -nic user,model=spapr-vlan,hostfwd=tcp:127.0.0.1:7123-:23,hostfwd=tcp:127.0.0.1:7114-:514 ^
 %*
