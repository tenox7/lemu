# sun4u (Ultra 5/10 class)

**TL;DR** `-M sun4u` with QEMU's OpenBIOS. LEMU fixes the interrupt status
of the built-in IDE controller, which Solaris 10 needs. NetBSD boots from
its CD as on QEMU.

## IDE interrupt status (cmd646)

- The cmd646 latches a drive's interrupt in its status bits (`MRDMODE`,
  mirrored in `CFR` and `ARTTIM23`) until the driver writes 1 to clear
  them. QEMU cleared them as soon as the drive dropped its interrupt line,
  which it does when the driver reads the drive's status.
- Solaris 10's `ata` driver reads the status first and then looks at the
  bits, so it printed `WARNING: Interrupt not seen after set_features` for
  every IDE device.
- LEMU keeps the bits until they are cleared (`hw/ide/cmd646.c`). The PCI
  interrupt line still follows the drive: driving it from the bits instead
  hangs Linux and OpenBSD, which never clear them.

## Testing

- `tests/qtest/sun4u-cmd646-test.c` checks the bits and the line through
  the sabre. In a build with `sparc64-softmmu`:
  `QTEST_QEMU_BINARY=./qemu-system-sparc64 tests/qtest/sun4u-cmd646-test`
- `tests/functional/sparc64/test_netbsd.py` boots the NetBSD 11.1 CD from
  `ide-cd,bus=ide.1,unit=0` to the installer:
  `make check-functional-sparc64`.

## Solaris

Solaris 8 to 10 also need OpenBIOS fixes on sun4u: the virtual memory
list, the CPU's cache properties, the names of the PCI and ebus nodes and
IDE drives under their controller. They are not in LEMU yet: with the
OpenBIOS in `pc-bios/`, Solaris panics early in boot (`no nucleus hblk8 to
allocate`, `BOP_ALLOC failed` or `VAC too big!`).
