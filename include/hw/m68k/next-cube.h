/*
 * NeXT 68k computers: shared definitions
 *
 * Copyright (c) 2011 Bryce Lanham
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The interrupt bits and register layout come from the Previous emulator
 * (src/includes/sysReg.h) and NetBSD (sys/arch/next68k/include/cpu.h).
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef NEXT_CUBE_H
#define NEXT_CUBE_H

#define TYPE_NEXTFB "next-fb"

/*
 * Models. The values are the system type the ROM finds in SCR1 bits 12-15
 * on all machines without the Turbo chipset.
 */
typedef enum NeXTModel {
    NEXT_MODEL_COMPUTER = 0,      /* NeXT Computer, 68030 */
    NEXT_MODEL_STATION = 1,       /* NeXTstation */
    NEXT_MODEL_CUBE = 2,          /* NeXTcube, 68040 */
    NEXT_MODEL_STATION_COLOR = 3, /* NeXTstation Color */
    NEXT_MODEL_TURBO = 4,         /* all Turbo machines */
} NeXTModel;

/*
 * Interrupt status bits. Devices drive the GPIO input of the system
 * control device with the same number as their bit.
 */
enum {
    NEXT_INT_SOFT1 = 0,      /* level 1 */
    NEXT_INT_SOFT2 = 1,      /* level 2 */
    NEXT_INT_POWER = 2,      /* level 3 from here */
    NEXT_INT_KEYMOUSE = 3,
    NEXT_INT_MONITOR = 4,
    NEXT_INT_VIDEO = 5,
    NEXT_INT_DSP_L3 = 6,
    NEXT_INT_PHONE = 7,      /* floppy */
    NEXT_INT_SOUND_OVRUN = 8,
    NEXT_INT_EN_RX = 9,
    NEXT_INT_EN_TX = 10,
    NEXT_INT_PRINTER = 11,
    NEXT_INT_SCSI = 12,
    NEXT_INT_DISK = 13,      /* MO drive, or video on colour machines */
    NEXT_INT_DSP_L4 = 14,    /* level 4 */
    NEXT_INT_BUS = 15,       /* level 5 */
    NEXT_INT_REMOTE = 16,
    NEXT_INT_SCC = 17,
    NEXT_INT_R2M_DMA = 18,   /* level 6 */
    NEXT_INT_M2R_DMA = 19,
    NEXT_INT_DSP_DMA = 20,
    NEXT_INT_SCC_DMA = 21,
    NEXT_INT_SND_IN_DMA = 22,
    NEXT_INT_SND_OUT_DMA = 23,
    NEXT_INT_PRINTER_DMA = 24,
    NEXT_INT_DISK_DMA = 25,
    NEXT_INT_SCSI_DMA = 26,
    NEXT_INT_EN_RX_DMA = 27,
    NEXT_INT_EN_TX_DMA = 28,
    NEXT_INT_TIMER = 29,
    NEXT_INT_PFAIL = 30,     /* level 7 */
    NEXT_INT_NMI = 31,
    NEXT_NUM_IRQS = 32
};

#define NEXT_INT_L7_MASK     0xC0000000
#define NEXT_INT_L6_MASK     0x3FFC0000
#define NEXT_INT_L5_MASK     0x00038000
#define NEXT_INT_L4_MASK     0x00004000
#define NEXT_INT_L3_MASK     0x00003FFC
#define NEXT_INT_L2_MASK     0x00000002
#define NEXT_INT_L1_MASK     0x00000001

/* Physical address map */
#define NEXT_ROM_BASE        0x00000000
#define NEXT_ROM_DIAG_BASE   0x01000000
#define NEXT_IO_BASE         0x02000000
#define NEXT_IO_BMAP_BASE    0x02100000
#define NEXT_IO_SIZE         0x00020000
#define NEXT_BMAP_BASE       0x020C0000
#define NEXT_BMAP_BASE2      0x820C0000
#define NEXT_TMC_BASE        0x02200000
#define NEXT_NCC_BASE        0x02210000
#define NEXT_RAM_BASE        0x04000000
#define NEXT_VRAM_BASE       0x0B000000
#define NEXT_VRAM_MWF_BASE   0x0C000000
#define NEXT_VRAM_TURBO_BASE 0x0C000000
#define NEXT_RAM_MWF_BASE    0x10000000
#define NEXT_VRAM_COLOR_BASE 0x2C000000

/* Offsets of the devices inside the device space */
#define NEXT_IO_DMA_CSR      0x00000
#define NEXT_IO_DMA_REGS     0x04000
#define NEXT_IO_ENET         0x06000
#define NEXT_IO_INTSTAT      0x07000
#define NEXT_IO_INTMASK      0x07800
#define NEXT_IO_DSP          0x08000
#define NEXT_IO_SCR1         0x0C000
#define NEXT_IO_SCR2         0x0D000
#define NEXT_IO_KMS          0x0E000
#define NEXT_IO_PRINTER      0x0F000
#define NEXT_IO_BRIGHTNESS   0x10000
#define NEXT_IO_MO           0x12000
#define NEXT_IO_SCSI         0x14000
#define NEXT_IO_SCSI_CSR     0x14020
#define NEXT_IO_FLOPPY       0x14100
#define NEXT_IO_HARDCLOCK    0x16000
#define NEXT_IO_SCC          0x18000
#define NEXT_IO_SCC_CLOCK    0x18004
#define NEXT_IO_RAMDAC       0x18100
#define NEXT_IO_COLOR_CSR    0x18180
#define NEXT_IO_EVENTC       0x1A000
#define NEXT_IO_RAMDAC_TURBO 0x1C000
#define NEXT_TMC_BASE        0x02200000

#endif /* NEXT_CUBE_H */
