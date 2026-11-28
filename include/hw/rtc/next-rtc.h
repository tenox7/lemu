/*
 * NeXT real time clock and NVRAM: MC68HC68T1 and MCCS1850
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_RTC_NEXT_RTC_H
#define HW_RTC_NEXT_RTC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_NEXT_RTC "next-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(NeXTRTC, NEXT_RTC)

#define NEXT_NVRAM_SIZE 32

struct NeXTRTC {
    SysBusDevice parent_obj;

    BlockBackend *blk;
    qemu_irq power_irq;

    /* configuration */
    bool new_chip;          /* MCCS1850 instead of MC68HC68T1 */
    bool turbo;

    /* serial interface */
    int32_t phase;
    uint8_t addr;
    uint8_t val;
    uint8_t data;

    /* time: guest seconds = host seconds + offset */
    int64_t offset;

    /* MC68HC68T1 */
    uint8_t ram[NEXT_NVRAM_SIZE];
    uint8_t time[7];
    uint8_t alarm[3];
    uint8_t status;
    uint8_t clkctrl;
    uint8_t intctrl;

    /* MCCS1850 */
    uint32_t timecntr;
    uint32_t alarmcntr;
    uint8_t nstatus;
    uint8_t control;
    uint8_t ram2[NEXT_NVRAM_SIZE];

    bool nvram_loaded;
};

/* SCR2 drives the serial interface */
void next_rtc_start(NeXTRTC *s);
void next_rtc_reset_interface(NeXTRTC *s);
void next_rtc_write_bit(NeXTRTC *s, bool bit);
bool next_rtc_read_bit(NeXTRTC *s);

/* The power key asks the machine to shut down */
void next_rtc_request_power_down(NeXTRTC *s);
void next_rtc_stop_power_down(NeXTRTC *s);

/*
 * Default NVRAM contents when no saved NVRAM is attached. enet is the
 * Ethernet address from the ROM, simm the ni_simm word.
 */
void next_rtc_set_defaults(NeXTRTC *s, const uint8_t *enet, uint16_t simm);

#endif /* HW_RTC_NEXT_RTC_H */
