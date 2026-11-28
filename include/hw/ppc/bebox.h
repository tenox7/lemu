/*
 * BeBox motherboard logic
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_BEBOX_H
#define HW_PPC_BEBOX_H

#define TYPE_BEBOX_GLUE "bebox-glue"
#define TYPE_BEBOX_IO "bebox-io"

#define BEBOX_GLUE_BASE 0x7ffff000

/* bebox-glue gpio inputs: interrupt source bits, bit 0 is the LSB */
enum {
    BEBOX_IRQ_GEEKPORT = 2,
    BEBOX_IRQ_ADC = 3,
    BEBOX_IRQ_IR = 4,
    BEBOX_IRQ_8259 = 5,
    BEBOX_IRQ_ISA15 = 6,
    BEBOX_IRQ_IDE = 7,          /* ISA IRQ 14 */
    BEBOX_IRQ_MOUSE = 8,        /* ISA IRQ 12 */
    BEBOX_IRQ_ISA11 = 9,
    BEBOX_IRQ_ISA10 = 10,
    BEBOX_IRQ_ISA9 = 11,
    BEBOX_IRQ_PARALLEL = 12,    /* ISA IRQ 7 */
    BEBOX_IRQ_FLOPPY = 13,      /* ISA IRQ 6 */
    BEBOX_IRQ_ISA5 = 14,
    BEBOX_IRQ_RTC = 15,         /* ISA IRQ 8 */
    BEBOX_IRQ_KEYBOARD = 16,    /* ISA IRQ 1 */
    BEBOX_IRQ_SOUND = 17,
    BEBOX_IRQ_PCI3 = 18,
    BEBOX_IRQ_PCI2 = 19,
    BEBOX_IRQ_PCI1 = 20,
    BEBOX_IRQ_SCSI = 21,
    BEBOX_IRQ_MIDI2 = 22,
    BEBOX_IRQ_MIDI1 = 23,
    BEBOX_IRQ_SERIAL4 = 24,
    BEBOX_IRQ_SERIAL3 = 25,
    BEBOX_IRQ_SERIAL2 = 26,     /* ISA IRQ 3 */
    BEBOX_IRQ_SERIAL1 = 27,     /* ISA IRQ 4 */
};

#endif
