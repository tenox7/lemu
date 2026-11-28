/*
 * NeXT real time clock and NVRAM: MC68HC68T1 and MCCS1850
 *
 * Copyright (c) 2011 Bryce Lanham
 * Copyright (c) 2024 Mark Cave-Ayland
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the Previous emulator's rtcnvram.c (GPL-2.0-or-later).
 *
 * The CPU talks to the chip one bit at a time through SCR2: chip enable,
 * clock and data. The first 8 bits are the address, bit 7 set for a
 * write; then 8 data bits follow, and the address goes up by one after
 * each byte. The older MC68HC68T1 keeps time in BCD registers, the
 * MCCS1850 in a 32-bit seconds counter, and has 32 more bytes of RAM.
 * Status bit 7 tells them apart.
 *
 * The NVRAM holds the boot command, the SIMM layout, the Ethernet address,
 * volume and brightness. Attach a 64-byte file with -drive if=mtd to keep
 * it; without one the defaults are built at every start.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/rtc/next-rtc.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"
#include "system/rtc.h"
#include "system/runstate.h"

#define RTC_ADDR_WRITE  0x80
#define RTC_ADDR_CLOCK  0x20
#define RTC_ADDR_MASK   0x7F
#define RTC_ADDR_NEWRAM 0x40

/* MC68HC68T1 */
#define RTC_INT_SENSE   0x04
#define RTC_START       0x80
#define RTC_POWERDOWN   0x40

/* MCCS1850 */
#define NRTC_NEWCHIP    0x80
#define NRTC_FIRSTUP    0x10
#define NRTC_INT        0x08
#define NRTC_INT_LBAT   0x04
#define NRTC_INT_ALARM  0x02
#define NRTC_INT_PDOWN  0x01
#define NRTC_CTRL_0     0x4D
#define NRTC_START      0x80
#define NRTC_POWERDOWN  0x40
#define NRTC_ENABLEALRM 0x10
#define NRTC_CLRALARM   0x08
#define NRTC_CLRFTU     0x04
#define NRTC_LBE        0x02
#define NRTC_CLRPDOWN   0x01

/* NeXT software keeps dates between these */
#define NEXT_START_SEC   552225600LL   /* Thu Jul  2 12:00:00 1987 UTC */
#define NEXT_LIMIT_SEC   2145916800LL  /* Fri Jan  1 00:00:00 2038 UTC */

/* NVRAM byte 17 */
#define NEW_CLOCK_CHIP   0x80
#define USE_CONSOLE_SLOT 0x20

static int64_t next_rtc_host_now(void)
{
    struct tm tm;

    qemu_get_timedate(&tm, 0);
    return mktimegm(&tm);
}

static int64_t next_rtc_get_time(NeXTRTC *s)
{
    return next_rtc_host_now() + s->offset;
}

static void next_rtc_set_time(NeXTRTC *s, int64_t t)
{
    s->offset = t - next_rtc_host_now();
}

static void next_rtc_check_time(NeXTRTC *s)
{
    int64_t t = next_rtc_get_time(s);

    if (t >= NEXT_LIMIT_SEC || t < 0) {
        next_rtc_set_time(s, NEXT_START_SEC);
    }
}

static void next_rtc_save(NeXTRTC *s)
{
    uint8_t buf[2 * NEXT_NVRAM_SIZE];

    if (!s->blk) {
        return;
    }
    memcpy(buf, s->ram, NEXT_NVRAM_SIZE);
    memcpy(buf + NEXT_NVRAM_SIZE, s->ram2, NEXT_NVRAM_SIZE);
    if (blk_pwrite(s->blk, 0, sizeof(buf), buf, 0) < 0) {
        qemu_log_mask(LOG_UNIMP, "next-rtc: cannot write the NVRAM file\n");
    }
}

/* ---- MC68HC68T1 ---- */

static uint8_t to_bcd(int val)
{
    return (((val / 10) % 10) << 4) | (val % 10);
}

static int from_bcd(uint8_t bcd)
{
    return ((bcd & 0xF0) >> 4) * 10 + (bcd & 0xF);
}

static uint8_t to_bcd_hour(NeXTRTC *s, int val)
{
    if (s->time[2] & 0x80) {
        if ((val % 12) == 0) {
            val += 12;
        }
        if (val > 12) {
            return to_bcd(val - 12) | 0xA0;
        }
        return to_bcd(val) | 0x80;
    }
    return to_bcd(val);
}

static int from_bcd_hour(uint8_t bcd)
{
    if (bcd & 0x80) {
        if ((bcd & 0x1F) == 0x12) {
            bcd &= 0xA0;
        }
        if (bcd & 0x20) {
            return from_bcd(bcd & 0x1F) + 12;
        }
        return from_bcd(bcd & 0x1F);
    }
    return from_bcd(bcd);
}

static void oldrtc_get_time(NeXTRTC *s)
{
    time_t t = next_rtc_get_time(s);
    struct tm tm;

    gmtime_r(&t, &tm);
    s->time[0] = to_bcd(tm.tm_sec);
    s->time[1] = to_bcd(tm.tm_min);
    s->time[2] = to_bcd_hour(s, tm.tm_hour);
    s->time[3] = to_bcd(tm.tm_wday + 1);
    s->time[4] = to_bcd(tm.tm_mday);
    s->time[5] = to_bcd(tm.tm_mon + 1);
    /* two BCD digits of years since 1900, 2000 and later overflow */
    s->time[6] = (((tm.tm_year / 10) & 0xF) << 4) | (tm.tm_year % 10);
}

static void oldrtc_set_time(NeXTRTC *s)
{
    struct tm tm = { 0 };

    tm.tm_sec = from_bcd(s->time[0]);
    tm.tm_min = from_bcd(s->time[1]);
    tm.tm_hour = from_bcd_hour(s->time[2]);
    tm.tm_mday = from_bcd(s->time[4]);
    tm.tm_mon = from_bcd(s->time[5]) - 1;
    tm.tm_year = ((s->time[6] >> 4) & 0xF) * 10 + (s->time[6] & 0xF);
    next_rtc_set_time(s, mktimegm(&tm));
}

static uint8_t oldrtc_get_clock(NeXTRTC *s, uint8_t addr)
{
    uint8_t val = 0;

    switch (addr & RTC_ADDR_MASK) {
    case 0x20 ... 0x26:
        val = s->time[(addr & RTC_ADDR_MASK) - 0x20];
        break;
    case 0x30:
        val = s->status & 0x5F;
        s->status &= RTC_INT_SENSE;
        break;
    case 0x31:
        val = s->clkctrl;
        break;
    case 0x32:
        val = s->intctrl;
        break;
    }
    return val;
}

static void oldrtc_put_clock(NeXTRTC *s, uint8_t addr, uint8_t val)
{
    uint8_t changed;

    switch (addr & RTC_ADDR_MASK) {
    case 0x20 ... 0x26: {
        static const uint8_t mask[7] = { 0xff, 0xff, 0xbf, 0x07,
                                         0xff, 0xff, 0xff };
        int i = (addr & RTC_ADDR_MASK) - 0x20;

        s->time[i] = val & mask[i];
        if (s->clkctrl & RTC_START) {
            oldrtc_set_time(s);
        }
        break;
    }
    case 0x28:
    case 0x29:
        s->alarm[(addr & RTC_ADDR_MASK) - 0x28] = val;
        break;
    case 0x2A:
        s->alarm[2] = val & 0x3F;
        break;
    case 0x31:
        changed = val ^ s->clkctrl;
        s->clkctrl = val;
        if ((changed & RTC_START) && (val & RTC_START)) {
            oldrtc_set_time(s);
        }
        break;
    case 0x32:
        s->intctrl = val;
        if (val & RTC_POWERDOWN) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
        break;
    }
}

/* ---- MCCS1850 ---- */

static void newrtc_check_interrupt(NeXTRTC *s)
{
    if (((s->nstatus & NRTC_INT_ALARM) && (s->control & NRTC_ENABLEALRM)) ||
        ((s->nstatus & NRTC_INT_LBAT) && (s->control & NRTC_LBE)) ||
        (s->nstatus & NRTC_FIRSTUP) || (s->nstatus & NRTC_INT_PDOWN)) {
        s->nstatus |= NRTC_INT;
        qemu_irq_raise(s->power_irq);
    } else {
        s->nstatus &= ~NRTC_INT;
        qemu_irq_lower(s->power_irq);
    }
}

static uint8_t newrtc_get_clock(NeXTRTC *s, uint8_t addr)
{
    switch (addr & RTC_ADDR_MASK) {
    case 0x20 ... 0x23:
        return s->timecntr >> (8 * (0x23 - (addr & RTC_ADDR_MASK)));
    case 0x24 ... 0x27:
        return s->alarmcntr >> (8 * (0x27 - (addr & RTC_ADDR_MASK)));
    case 0x30:
        return (s->nstatus & 0x3F) | NRTC_NEWCHIP;
    case 0x31:
        return s->control & ~NRTC_CTRL_0;
    }
    return 0;
}

static void newrtc_put_clock(NeXTRTC *s, uint8_t addr, uint8_t val)
{
    int shift;
    uint8_t changed;

    switch (addr & RTC_ADDR_MASK) {
    case 0x20 ... 0x23:
        shift = 8 * (0x23 - (addr & RTC_ADDR_MASK));
        s->timecntr = (s->timecntr & ~(0xffu << shift)) |
                      ((uint32_t)val << shift);
        if (s->control & NRTC_START) {
            next_rtc_set_time(s, s->timecntr);
        }
        break;
    case 0x24 ... 0x27:
        shift = 8 * (0x27 - (addr & RTC_ADDR_MASK));
        s->alarmcntr = (s->alarmcntr & ~(0xffu << shift)) |
                       ((uint32_t)val << shift);
        break;
    case 0x31:
        changed = val ^ s->control;
        s->control = val;
        if (val & NRTC_POWERDOWN) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            return;
        }
        if ((changed & NRTC_START) && (val & NRTC_START)) {
            next_rtc_set_time(s, s->timecntr);
        }
        if (val & NRTC_CLRFTU) {
            s->nstatus &= ~NRTC_FIRSTUP;
        }
        if (val & NRTC_CLRALARM) {
            s->nstatus &= ~NRTC_INT_ALARM;
        }
        if (val & NRTC_CLRPDOWN) {
            s->nstatus &= ~NRTC_INT_PDOWN;
        }
        if (!(val & NRTC_LBE)) {
            s->nstatus &= ~NRTC_INT_LBAT;
        }
        newrtc_check_interrupt(s);
        break;
    }
}

/* ---- serial interface ---- */

static uint8_t next_rtc_read_byte(NeXTRTC *s, uint8_t addr)
{
    if (addr & RTC_ADDR_CLOCK) {
        return s->new_chip ? newrtc_get_clock(s, addr)
                           : oldrtc_get_clock(s, addr);
    }
    if (s->new_chip && (addr & RTC_ADDR_NEWRAM)) {
        return s->ram2[addr & 0x1F];
    }
    return s->ram[addr & 0x1F];
}

static void next_rtc_write_byte(NeXTRTC *s, uint8_t addr, uint8_t val)
{
    if (addr & RTC_ADDR_CLOCK) {
        if (s->new_chip) {
            newrtc_put_clock(s, addr, val);
        } else {
            oldrtc_put_clock(s, addr, val);
        }
        return;
    }
    if (s->new_chip && (addr & RTC_ADDR_NEWRAM)) {
        s->ram2[addr & 0x1F] = val;
    } else {
        s->ram[addr & 0x1F] = val;
    }
    next_rtc_save(s);
}

static void next_rtc_next_addr(NeXTRTC *s)
{
    if (s->new_chip) {
        switch (s->addr) {
        case 0x7F:
            s->addr = 0x00;
            break;
        case 0xFF:
            s->addr = 0x80;
            break;
        default:
            s->addr++;
            break;
        }
    } else {
        switch (s->addr) {
        case 0x1F:
        case 0x9F:
            s->addr = 0x00;
            break;
        case 0x32:
        case 0xB2:
            s->addr = 0x20;
            break;
        default:
            s->addr++;
            break;
        }
    }
}

void next_rtc_write_bit(NeXTRTC *s, bool bit)
{
    s->phase++;
    if (s->phase <= 8) {
        s->addr = (s->addr << 1) | bit;
    } else {
        if (s->phase == 9 && !(s->addr & RTC_ADDR_WRITE)) {
            s->val = next_rtc_read_byte(s, s->addr);
        }
        if (s->addr & RTC_ADDR_WRITE) {
            s->val = (s->val << 1) | bit;
        } else {
            bit = (s->val >> (16 - s->phase)) & 1;
        }
        if (s->phase == 16) {
            if (s->addr & RTC_ADDR_WRITE) {
                next_rtc_write_byte(s, s->addr, s->val);
            }
            next_rtc_next_addr(s);
            s->phase -= 8;
        }
    }
    s->data = bit;
}

bool next_rtc_read_bit(NeXTRTC *s)
{
    return s->data;
}

void next_rtc_start(NeXTRTC *s)
{
    /* the time is latched while the interface is active */
    if (s->new_chip) {
        if (s->control & NRTC_START) {
            s->timecntr = next_rtc_get_time(s);
        }
    } else if (s->clkctrl & RTC_START) {
        oldrtc_get_time(s);
    }
}

void next_rtc_reset_interface(NeXTRTC *s)
{
    s->phase = 0;
    s->addr = 0;
    s->val = 0;
    s->data = s->turbo ? 0 : 1;
}

void next_rtc_request_power_down(NeXTRTC *s)
{
    if (s->new_chip) {
        s->nstatus |= NRTC_INT_PDOWN;
        newrtc_check_interrupt(s);
    } else {
        /* The old chip's interrupt is not connected: the keyboard does it */
        qemu_irq_raise(s->power_irq);
    }
}

void next_rtc_stop_power_down(NeXTRTC *s)
{
    if (!s->new_chip) {
        qemu_irq_lower(s->power_irq);
    }
}

static uint16_t next_rtc_checksum(const uint8_t *ram)
{
    unsigned sum = 0;

    for (int i = 0; i < 30; i += 2) {
        sum += (ram[i] << 8) | ram[i + 1];
        if (sum >= 0x10000) {
            sum -= 0x10000;
            sum += 1;
        }
    }
    return 0xFFFF - sum;
}

void next_rtc_set_defaults(NeXTRTC *s, const uint8_t *enet, uint16_t simm)
{
    uint32_t config;
    uint16_t sum;

    if (s->nvram_loaded) {
        /* keep the saved NVRAM, but say which clock chip there is */
        if (s->new_chip) {
            s->ram[17] |= NEW_CLOCK_CHIP;
        } else {
            s->ram[17] &= ~NEW_CLOCK_CHIP;
        }
    } else {
        memset(s->ram, 0, sizeof(s->ram));
        memset(s->ram2, 0, sizeof(s->ram2));

        /* reset = 9, allow eject, full brightness */
        config = 0x94000000 | (0x3D << 14);
        stl_be_p(&s->ram[0], config);
        memcpy(&s->ram[4], enet, 6);
        stw_be_p(&s->ram[10], simm);
        /*
         * Power-on test on, verbose, with the SCSI and monitor tests:
         * without POT_ON the ROM skips setting up the machine.
         */
        s->ram[14] = 0x4B;
        s->ram[17] = USE_CONSOLE_SLOT | (s->new_chip ? NEW_CLOCK_CHIP : 0);
        /* boot from SCSI */
        s->ram[18] = 's';
        s->ram[19] = 'd';
    }
    sum = next_rtc_checksum(s->ram);
    s->ram[30] = sum >> 8;
    s->ram[31] = sum;
    next_rtc_save(s);
}

static void next_rtc_reset_hold(Object *obj, ResetType type)
{
    NeXTRTC *s = NEXT_RTC(obj);

    s->clkctrl = RTC_START;
    s->control = NRTC_START;
    next_rtc_check_time(s);
    next_rtc_reset_interface(s);
}

static void next_rtc_realize(DeviceState *dev, Error **errp)
{
    NeXTRTC *s = NEXT_RTC(dev);
    uint8_t buf[2 * NEXT_NVRAM_SIZE];

    if (s->blk) {
        int64_t len = blk_getlength(s->blk);

        if (len < (int64_t)sizeof(buf)) {
            error_setg(errp, "next-rtc: the NVRAM file must have %zu bytes",
                       sizeof(buf));
            return;
        }
        if (!blk_supports_write_perm(s->blk)) {
            error_setg(errp, "next-rtc: the NVRAM file is read only");
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (blk_pread(s->blk, 0, sizeof(buf), buf, 0) < 0) {
            error_setg(errp, "next-rtc: cannot read the NVRAM file");
            return;
        }
        memcpy(s->ram, buf, NEXT_NVRAM_SIZE);
        memcpy(s->ram2, buf + NEXT_NVRAM_SIZE, NEXT_NVRAM_SIZE);
        /* an empty file gets the defaults */
        s->nvram_loaded = !buffer_is_zero(buf, sizeof(buf));
    }
    /*
     * The clock has been running all along: with "first up" the ROM
     * would take it for a new battery, stop it and set it to 0.
     */
    s->nstatus = 0;
}

static void next_rtc_init(Object *obj)
{
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &NEXT_RTC(obj)->power_irq);
}

static const VMStateDescription next_rtc_vmstate = {
    .name = "next-rtc",
    .version_id = 4,
    .minimum_version_id = 4,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(phase, NeXTRTC),
        VMSTATE_UINT8(addr, NeXTRTC),
        VMSTATE_UINT8(val, NeXTRTC),
        VMSTATE_UINT8(data, NeXTRTC),
        VMSTATE_INT64(offset, NeXTRTC),
        VMSTATE_UINT8_ARRAY(ram, NeXTRTC, NEXT_NVRAM_SIZE),
        VMSTATE_UINT8_ARRAY(time, NeXTRTC, 7),
        VMSTATE_UINT8_ARRAY(alarm, NeXTRTC, 3),
        VMSTATE_UINT8(status, NeXTRTC),
        VMSTATE_UINT8(clkctrl, NeXTRTC),
        VMSTATE_UINT8(intctrl, NeXTRTC),
        VMSTATE_UINT32(timecntr, NeXTRTC),
        VMSTATE_UINT32(alarmcntr, NeXTRTC),
        VMSTATE_UINT8(nstatus, NeXTRTC),
        VMSTATE_UINT8(control, NeXTRTC),
        VMSTATE_UINT8_ARRAY(ram2, NeXTRTC, NEXT_NVRAM_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static const Property next_rtc_properties[] = {
    DEFINE_PROP_BOOL("new-chip", NeXTRTC, new_chip, false),
    DEFINE_PROP_BOOL("turbo", NeXTRTC, turbo, false),
    DEFINE_PROP_DRIVE("drive", NeXTRTC, blk),
};

static void next_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NeXT RTC and NVRAM";
    dc->realize = next_rtc_realize;
    dc->vmsd = &next_rtc_vmstate;
    device_class_set_props(dc, next_rtc_properties);
    rc->phases.hold = next_rtc_reset_hold;
}

static const TypeInfo next_rtc_info = {
    .name = TYPE_NEXT_RTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTRTC),
    .instance_init = next_rtc_init,
    .class_init = next_rtc_class_init,
};

static void next_rtc_register_types(void)
{
    type_register_static(&next_rtc_info);
}

type_init(next_rtc_register_types)
