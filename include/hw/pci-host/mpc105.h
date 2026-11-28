/*
 * Motorola MPC105 "Eagle" PCI bridge/memory controller
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PCI_HOST_MPC105_H
#define HW_PCI_HOST_MPC105_H

#define TYPE_MPC105_PCI_HOST_BRIDGE "mpc105-pcihost"
#define TYPE_MPC105_PCI_DEVICE "mpc105"

/*
 * The host has one interrupt output per PCI device number 12 to 15
 * (sysbus IRQs 0 to 3), so the board can route each to its own line.
 * Its gpio output 0 follows PICR1[FLASH_WR_EN].
 */
#define MPC105_PCI_IRQ_FIRST_DEV 12
#define MPC105_PCI_NUM_IRQS 4

#endif
