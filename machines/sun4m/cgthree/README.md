# Sun cg3 frame buffer: screens of any size

**TL;DR** `-vga cg3 -g 1920x1080x8` (or any other size) now works. The stock
card did 1024x768 only: 1152x900 crashed OpenBIOS at power-on.

## What sets the size

- **QEMU.** `-g WIDTHxHEIGHTx8` sizes the card's video RAM: 640x480 or more,
  8 MB of pixels at most, and a width that 8 divides. A real cg3 has 1 MB.
- **The card's FCode ROM,** `pc-bios/QEMU,cgthree.bin`. OpenBIOS runs it at
  power-on. It gives the guest the `width`, `height` and `linebytes`
  properties, and the guest's driver takes the size from them.
- **SunOS 4.1.4** maps the frame buffer by itself, so OpenWindows and SunView
  use the whole screen. It boots with up to about 6 MB of pixels: 3200x1800
  and 3072x1920 work, 3200x2000 resets at boot.

## What the ROM here changes

`cgthree.fs` is OpenBIOS's `drivers/cgthree.fs` with one change. OpenBIOS has
768 KB of address space to map a frame buffer in, the size of 1024x768, and
the stock ROM mapped that much and then drew on the whole screen. Now:

- The firmware's text console uses the lines at the top of the screen that
  fit in 768 KB: all 768 at 1024x768, 682 at 1152x900, 409 at 1920x1080.
- SunOS writes its console through the firmware, so its text console has the
  same lines: 85 rows at 1152x900 and 51 at 1920x1080, where SunOS wants 34.
- The properties still give the real size.

## Rebuild the ROM

```sh
machines/sun4m/cgthree/build.sh
```

It runs `toke` from fcode-utils, from the host if it is installed, else in a
Debian container, and writes `pc-bios/QEMU,cgthree.bin`. The unchanged
`drivers/cgthree.fs` gives the ROM that QEMU ships, byte for byte.

## Sizes that were tested

SunOS 4.1.4 with OpenWindows 3: 1024x768, 1152x900, 1920x1080, 2560x1600,
3200x1800. SunView: 1920x1080.
