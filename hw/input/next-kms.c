/*
 * NeXT keyboard, mouse and sound interface (KMS)
 *
 * Copyright (c) 2011 Bryce Lanham
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * Based on the Previous emulator's kms.c and sdlkeymap.c
 * (GPL-2.0-or-later).
 *
 * The keyboard and mouse hang off the MegaPixel Display or the Sound Box,
 * which talks to the computer over one serial link. The computer sends
 * commands with a long word of data; the monitor sends back key and mouse
 * events, sound requests and sampled sound.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/input/next-kms.h"
#include "hw/m68k/next-cube.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "ui/input.h"
#include "standard-headers/linux/input-event-codes.h"

/* Sound byte */
#define SNDOUT_DMA_ENABLE   0x80
#define SNDOUT_DMA_REQUEST  0x40
#define SNDOUT_DMA_UNDERRUN 0x20
#define SNDIN_DMA_ENABLE    0x08
#define SNDIN_DMA_REQUEST   0x04
#define SNDIN_DMA_OVERRUN   0x02

/* Keyboard/Mouse byte */
#define KM_INT              0x80
#define KM_RECEIVED         0x40
#define KM_OVERRUN          0x20
#define NMI_RECEIVED        0x10
#define KMS_INT             0x08
#define KMS_RECEIVED        0x04
#define KMS_OVERRUN         0x02

/* Transmitter byte */
#define KMS_ENABLE          0x02
#define TX_LOOP             0x01

/* Commands from the monitor to the CPU */
#define KMSCMD_CODEC_IN     0xC7
#define KMSCMD_KM_RECV      0xC6
#define KMSCMD_SO_REQ       0x07
#define KMSCMD_SO_UNDR      0x0F

/* Commands from the CPU to the monitor: the sound ones are in the header */
#define KMSCMD_RESET        0xFF
#define KMSCMD_KMPOLL       0xC6
#define KMSCMD_KMREG        0xC5

/* Address byte of keyboard and mouse data */
#define KM_NO_RESPONSE      0x40
#define KM_USER_POLL        0x20
#define KM_INVALID          0x10
#define KM_MASTER           0x10
#define KM_ADDR_MASK        0x0F
#define KM_ADDR             0x0E
#define KM_MOUSE            0x01

#define REV_OLD             0x00
#define REV_NEW             0x01

#define MOUSE_Y             0xFE00
#define MOUSE_RIGHT_UP      0x0100
#define MOUSE_X             0x00FE
#define MOUSE_LEFT_UP       0x0001

#define KBD_KEY_VALID       0x8000
#define KBD_KEY_UP          0x0080

#define KMS_MAGIC_MASK      0x7000FFFF
#define KMS_MAGIC_RESET     0x1000A825  /* keypad * with left alt and cmd */
#define KMS_MAGIC_NMI_L     0x10008826  /* ` with left command */
#define KMS_MAGIC_NMI_R     0x10009026
#define KMS_MAGIC_NMI_LR    0x10009826

#define KM_CMD_MASK         0xE0
#define KM_CMD_READ         0x10
#define KM_CMD_RESET        0x0F
#define KM_CMD_SET_ADDR     0xEF

#define KM_KBD_REV          1
#define KM_KBD_ID           1
#define KM_MOUSE_REV        1
#define KM_MOUSE_ID         2

/* NeXT modifier bits (byte 1 of the key data) */
#define NEXTKEY_MOD_CTRL    0x01
#define NEXTKEY_MOD_LSHIFT  0x02
#define NEXTKEY_MOD_RSHIFT  0x04
#define NEXTKEY_MOD_LCMD    0x08
#define NEXTKEY_MOD_RCMD    0x10
#define NEXTKEY_MOD_LALT    0x20
#define NEXTKEY_MOD_RALT    0x40

#define NEXTKEY_POWER       0x58

static const uint8_t next_kms_keymap[KEY_CNT] = {
    [KEY_ESC] = 0x49,
    [KEY_GRAVE] = 0x26,
    [KEY_1] = 0x4A,
    [KEY_2] = 0x4B,
    [KEY_3] = 0x4C,
    [KEY_4] = 0x4D,
    [KEY_5] = 0x50,
    [KEY_6] = 0x4F,
    [KEY_7] = 0x4E,
    [KEY_8] = 0x1E,
    [KEY_9] = 0x1F,
    [KEY_0] = 0x20,
    [KEY_MINUS] = 0x1D,
    [KEY_EQUAL] = 0x1C,
    [KEY_BACKSPACE] = 0x1B,
    [KEY_TAB] = 0x41,
    [KEY_Q] = 0x42,
    [KEY_W] = 0x43,
    [KEY_E] = 0x44,
    [KEY_R] = 0x45,
    [KEY_T] = 0x48,
    [KEY_Y] = 0x47,
    [KEY_U] = 0x46,
    [KEY_I] = 0x06,
    [KEY_O] = 0x07,
    [KEY_P] = 0x08,
    [KEY_LEFTBRACE] = 0x05,
    [KEY_RIGHTBRACE] = 0x04,
    [KEY_BACKSLASH] = 0x03,
    [KEY_A] = 0x39,
    [KEY_S] = 0x3A,
    [KEY_D] = 0x3B,
    [KEY_F] = 0x3C,
    [KEY_G] = 0x3D,
    [KEY_H] = 0x40,
    [KEY_J] = 0x3F,
    [KEY_K] = 0x3E,
    [KEY_L] = 0x2D,
    [KEY_SEMICOLON] = 0x2C,
    [KEY_APOSTROPHE] = 0x2B,
    [KEY_ENTER] = 0x2A,
    [KEY_Z] = 0x31,
    [KEY_X] = 0x32,
    [KEY_C] = 0x33,
    [KEY_V] = 0x34,
    [KEY_B] = 0x35,
    [KEY_N] = 0x37,
    [KEY_M] = 0x36,
    [KEY_COMMA] = 0x2E,
    [KEY_DOT] = 0x2F,
    [KEY_SLASH] = 0x30,
    [KEY_SPACE] = 0x38,
    [KEY_NUMLOCK] = 0x26,
    [KEY_KPEQUAL] = 0x27,
    [KEY_KPSLASH] = 0x28,
    [KEY_KPASTERISK] = 0x25,
    [KEY_KP7] = 0x21,
    [KEY_KP8] = 0x22,
    [KEY_KP9] = 0x23,
    [KEY_KPMINUS] = 0x24,
    [KEY_KP4] = 0x12,
    [KEY_KP5] = 0x18,
    [KEY_KP6] = 0x13,
    [KEY_KPPLUS] = 0x15,
    [KEY_KP1] = 0x11,
    [KEY_KP2] = 0x17,
    [KEY_KP3] = 0x14,
    [KEY_KP0] = 0x0B,
    [KEY_KPDOT] = 0x0C,
    [KEY_KPENTER] = 0x0D,
    [KEY_LEFT] = 0x09,
    [KEY_RIGHT] = 0x10,
    [KEY_UP] = 0x16,
    [KEY_DOWN] = 0x0F,
    /* the keys above the arrows of the NeXT keyboard */
    [KEY_F10] = NEXTKEY_POWER,
    [KEY_DELETE] = NEXTKEY_POWER,
    [KEY_F5] = 0x02,          /* volume down */
    [KEY_END] = 0x02,
    [KEY_F6] = 0x1A,          /* volume up */
    [KEY_HOME] = 0x1A,
    [KEY_F1] = 0x01,          /* brightness down */
    [KEY_PAGEDOWN] = 0x01,
    [KEY_F2] = 0x19,          /* brightness up */
    [KEY_PAGEUP] = 0x19,
};

static uint8_t next_kms_modifier(unsigned q)
{
    switch (q) {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        return NEXTKEY_MOD_CTRL;
    case KEY_LEFTSHIFT:
    case KEY_CAPSLOCK:
        return NEXTKEY_MOD_LSHIFT;
    case KEY_RIGHTSHIFT:
        return NEXTKEY_MOD_RSHIFT;
    /* the PC's Windows/Command keys are the NeXT Command keys */
    case KEY_LEFTMETA:
        return NEXTKEY_MOD_LCMD;
    case KEY_RIGHTMETA:
        return NEXTKEY_MOD_RCMD;
    case KEY_LEFTALT:
        return NEXTKEY_MOD_LALT;
    case KEY_RIGHTALT:
        return NEXTKEY_MOD_RALT;
    default:
        return 0;
    }
}

static void next_kms_set_irq(NeXTKMSState *s, int n, bool level)
{
    qemu_set_irq(s->irq[n], level);
}

/* ---- monitor to CPU ---- */

static void next_kms_km_receive(NeXTKMSState *s, uint32_t data)
{
    switch (data & KMS_MAGIC_MASK) {
    case KMS_MAGIC_RESET:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    case KMS_MAGIC_NMI_L:
    case KMS_MAGIC_NMI_R:
    case KMS_MAGIC_NMI_LR:
        s->status_km |= NMI_RECEIVED;
        next_kms_set_irq(s, NEXT_KMS_IRQ_NMI, true);
        return;
    }

    /*
     * The keyboard and mouse keep their events until they are polled
     * again: hold them while the CPU has not read the last one.
     */
    if (s->status_km & KM_RECEIVED) {
        if (s->q_len < NEXT_KMS_QUEUE) {
            s->queue[(s->q_head + s->q_len) % NEXT_KMS_QUEUE] = data;
            s->q_len++;
        } else {
            s->status_km |= KM_OVERRUN;
        }
        return;
    }
    s->kmdata = data;
    s->status_km |= KM_RECEIVED | KM_INT;
    next_kms_set_irq(s, NEXT_KMS_IRQ_KEYMOUSE, true);
}

static void next_kms_km_next(NeXTKMSState *s)
{
    if (s->q_len && !(s->status_km & KM_RECEIVED)) {
        uint32_t data = s->queue[s->q_head];

        s->q_head = (s->q_head + 1) % NEXT_KMS_QUEUE;
        s->q_len--;
        next_kms_km_receive(s, data);
    }
}

static void next_kms_command_out(NeXTKMSState *s, uint8_t command,
                                 uint32_t data)
{
    if (!(s->status_tx & KMS_ENABLE)) {
        return;
    }
    switch (command) {
    case KMSCMD_KM_RECV:
        next_kms_km_receive(s, data);
        break;
    case KMSCMD_SO_REQ:
        s->status_cmd = command;
        s->status_snd |= SNDOUT_DMA_REQUEST;
        if (s->sound_request) {
            s->sound_request(s->sound_opaque);
        }
        break;
    case KMSCMD_SO_UNDR:
        s->status_cmd = command;
        s->status_snd |= SNDOUT_DMA_UNDERRUN;
        next_kms_set_irq(s, NEXT_KMS_IRQ_SOUND_OVRUN, true);
        break;
    case KMSCMD_CODEC_IN:
        s->status_snd |= SNDIN_DMA_REQUEST;
        break;
    }
}

bool next_kms_sound_request(NeXTKMSState *s)
{
    if (!(s->status_tx & KMS_ENABLE)) {
        return false;
    }
    next_kms_command_out(s, KMSCMD_SO_REQ, 0);
    return true;
}

void next_kms_sound_underrun(NeXTKMSState *s)
{
    next_kms_command_out(s, KMSCMD_SO_UNDR, 0);
}

void next_kms_sound_in_overrun(NeXTKMSState *s)
{
    s->status_snd |= SNDIN_DMA_OVERRUN;
    next_kms_set_irq(s, NEXT_KMS_IRQ_SOUND_OVRUN, true);
}

bool next_kms_codec_in(NeXTKMSState *s)
{
    if (!(s->status_tx & KMS_ENABLE)) {
        return false;
    }
    next_kms_command_out(s, KMSCMD_CODEC_IN, 0);
    return true;
}

/* ---- keyboard and mouse ---- */

static void km_internal_poll(NeXTKMSState *s, uint8_t addr)
{
    int device = addr & KM_ADDR_MASK;

    if (device == 0xF) {
        return;
    }
    for (int i = 28; i > 4; i -= 4) {
        if (((s->km_mask >> i) & KM_ADDR_MASK) == device) {
            next_kms_command_out(s, KMSCMD_KM_RECV,
                                 s->km_data[device] | ((uint32_t)addr << 24));
            return;
        }
    }
}

static void km_user_poll(NeXTKMSState *s, uint8_t addr)
{
    uint32_t data = s->km_data[addr & KM_ADDR_MASK];

    addr = (addr & ~KM_ADDR_MASK) | KM_USER_POLL;
    next_kms_command_out(s, KMSCMD_KM_RECV, data | ((uint32_t)addr << 24));
}

static void km_no_response(NeXTKMSState *s)
{
    uint8_t addr = KM_USER_POLL | KM_NO_RESPONSE | KM_INVALID;

    next_kms_command_out(s, KMSCMD_KM_RECV, (uint32_t)addr << 24);
}

static void km_version(NeXTKMSState *s, uint8_t addr)
{
    uint32_t data = (uint32_t)s->rev << 16;

    data |= ((addr & KM_MOUSE) ? KM_MOUSE_REV : KM_KBD_REV) << 8;
    data |= (addr & KM_MOUSE) ? KM_MOUSE_ID : KM_KBD_ID;
    addr = (addr & ~KM_ADDR_MASK) | KM_USER_POLL;
    next_kms_command_out(s, KMSCMD_KM_RECV, data | ((uint32_t)addr << 24));
}

static void km_reset(NeXTKMSState *s)
{
    s->km_addr = 0;
    s->km_mask = 0;
    memset(s->km_data, 0, sizeof(s->km_data));
}

static void km_access(NeXTKMSState *s, uint32_t data)
{
    uint8_t command = data >> 24;
    uint8_t cmddata = data >> 16;
    uint8_t device_cmd = (command & KM_CMD_MASK) >> 5;
    uint8_t device_addr = command & KM_ADDR_MASK;

    if (command == KM_CMD_RESET) {
        km_reset(s);
    } else if (command == KM_CMD_SET_ADDR) {
        s->km_addr = s->rev == REV_OLD ? cmddata & KM_ADDR : 0;
    } else if (command & KM_CMD_READ) {
        switch (device_cmd) {
        case 0:
            km_user_poll(s, device_addr);
            break;
        case 7:
            km_version(s, device_addr);
            return;
        }
    }
    km_no_response(s);
}

static void next_kms_key(NeXTKMSState *s, uint8_t keycode, bool down)
{
    uint8_t addr = s->km_addr | KM_MASTER;
    uint16_t data;

    if (keycode == NEXTKEY_POWER) {
        if (s->power_key) {
            s->power_key(s->power_opaque, down);
        }
        return;
    }
    data = (s->modifiers << 8) | keycode | KBD_KEY_VALID;
    if (!down) {
        data |= KBD_KEY_UP;
    }
    s->km_data[addr & KM_ADDR_MASK] = data;
    km_internal_poll(s, addr);
}

static void next_kms_mouse(NeXTKMSState *s, int x, int y)
{
    uint8_t addr = s->km_addr | KM_MOUSE;
    bool right = x >= 0;
    bool down = y >= 0;
    uint16_t data = 0;

    x = MIN(abs(x), 0x3F);
    y = MIN(abs(y), 0x3F);
    if (right && x > 0) {
        x = (0x40 - x) | 0x40;
    }
    if (down && y > 0) {
        y = (0x40 - y) | 0x40;
    }
    data |= (x << 1) & MOUSE_X;
    data |= (y << 9) & MOUSE_Y;
    data |= s->button_left ? 0 : MOUSE_LEFT_UP;
    data |= s->button_right ? 0 : MOUSE_RIGHT_UP;
    s->km_data[addr & KM_ADDR_MASK] = data;
    km_internal_poll(s, addr);
}

static void next_kms_event(DeviceState *dev, QemuConsole *src,
                           QemuInputEvent *evt)
{
    NeXTKMSState *s = NEXT_KMS(dev);
    unsigned q;
    uint8_t mod;

    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY:
        q = evt->key.key;
        if (q >= KEY_CNT) {
            break;
        }
        mod = next_kms_modifier(q);
        if (mod) {
            if (evt->key.down) {
                s->modifiers |= mod;
            } else {
                s->modifiers &= ~mod;
            }
            /* a modifier alone is sent without a key */
            next_kms_key(s, 0, evt->key.down);
        } else if (next_kms_keymap[q]) {
            next_kms_key(s, next_kms_keymap[q], evt->key.down);
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            s->button_left = evt->btn.down;
        } else if (evt->btn.button == INPUT_BUTTON_RIGHT) {
            s->button_right = evt->btn.down;
        } else {
            break;
        }
        s->buttons_changed = true;
        break;
    case INPUT_EVENT_KIND_REL:
        if (evt->rel.axis == INPUT_AXIS_X) {
            s->dx += evt->rel.value;
        } else if (evt->rel.axis == INPUT_AXIS_Y) {
            s->dy += evt->rel.value;
        }
        break;
    default:
        break;
    }
}

static void next_kms_sync(DeviceState *dev)
{
    NeXTKMSState *s = NEXT_KMS(dev);

    while (s->dx || s->dy || s->buttons_changed) {
        int x = MAX(MIN(s->dx, 0x3F), -0x3F);
        int y = MAX(MIN(s->dy, 0x3F), -0x3F);

        s->dx -= x;
        s->dy -= y;
        s->buttons_changed = false;
        next_kms_mouse(s, x, y);
    }
}

static const QemuInputHandler next_kms_handler = {
    .name  = "NeXT keyboard and mouse",
    .mask  = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN |
             INPUT_EVENT_MASK_REL,
    .event = next_kms_event,
    .sync  = next_kms_sync,
};

/* ---- CPU to monitor ---- */

static void next_kms_reset_link(NeXTKMSState *s)
{
    s->status_snd = 0;
    s->status_km = 0;
    s->status_tx = 0;
    s->status_cmd = 0;
    s->command = 0;
    s->data = 0;
    s->kmdata = 0;
    if (s->sound_command) {
        s->sound_command(s->sound_opaque, 0, 0);
    }
    km_reset(s);
    s->q_head = s->q_len = 0;
    for (int i = 0; i < NEXT_KMS_NUM_IRQS; i++) {
        next_kms_set_irq(s, i, false);
    }
}

static void next_kms_command_in(NeXTKMSState *s, uint8_t command,
                                uint32_t data)
{
    if (!(s->status_tx & KMS_ENABLE)) {
        return;
    }
    switch (command) {
    case KMSCMD_RESET:
        if (data == 0xFFFFFFFF) {
            next_kms_reset_link(s);
        }
        return;
    case KMSCMD_KMPOLL:
        s->km_mask = data;
        return;
    case KMSCMD_KMREG:
        km_access(s, data);
        return;
    case KMSCMD_VOLCTRL:
        if (s->rev == REV_OLD) {
            break;
        }
        /* fall through */
    case KMSCMD_ASNDOUT:
    case KMSCMD_CTRLOUT:
        if (s->sound_command) {
            s->sound_command(s->sound_opaque, command, data);
        }
        return;
    }

    /* commands without data: start and stop sound */
    if ((command & KMSCMD_SIO_MASK) == KMSCMD_SND_OUT) {
        if (!(command & KMSCMD_SIO_ENABLE)) {
            s->status_snd &= ~(SNDOUT_DMA_UNDERRUN | SNDOUT_DMA_REQUEST);
        }
    } else if ((command & KMSCMD_SIO_MASK) == KMSCMD_SND_IN) {
        if (!(command & KMSCMD_SIO_ENABLE)) {
            s->status_snd &= ~(SNDIN_DMA_OVERRUN | SNDIN_DMA_REQUEST);
        }
    } else {
        return;
    }
    if (s->sound_command) {
        s->sound_command(s->sound_opaque, command, 0);
    }
}

static void next_kms_write_byte(NeXTKMSState *s, int n, uint8_t val)
{
    switch (n) {
    case 0:
        s->status_snd &= ~(SNDOUT_DMA_ENABLE | SNDIN_DMA_ENABLE);
        s->status_snd |= val & (SNDOUT_DMA_ENABLE | SNDIN_DMA_ENABLE);
        if (val & SNDOUT_DMA_UNDERRUN) {
            s->status_snd &= ~(SNDOUT_DMA_UNDERRUN | SNDOUT_DMA_REQUEST);
            next_kms_set_irq(s, NEXT_KMS_IRQ_SOUND_OVRUN, false);
        }
        if (val & SNDIN_DMA_OVERRUN) {
            s->status_snd &= ~(SNDIN_DMA_OVERRUN | SNDIN_DMA_REQUEST);
            next_kms_set_irq(s, NEXT_KMS_IRQ_SOUND_OVRUN, false);
        }
        break;
    case 1:
        if (val & KM_OVERRUN) {
            s->status_km &= ~(KM_RECEIVED | KM_OVERRUN | KM_INT);
            next_kms_set_irq(s, NEXT_KMS_IRQ_KEYMOUSE, false);
            next_kms_km_next(s);
        }
        if (val & NMI_RECEIVED) {
            s->status_km &= ~NMI_RECEIVED;
            next_kms_set_irq(s, NEXT_KMS_IRQ_NMI, false);
        }
        if (val & KMS_OVERRUN) {
            s->status_km &= ~(KMS_RECEIVED | KMS_OVERRUN | KMS_INT);
            next_kms_set_irq(s, NEXT_KMS_IRQ_MONITOR, false);
        }
        break;
    case 2:
        if ((s->status_tx & KMS_ENABLE) && !(val & KMS_ENABLE)) {
            next_kms_reset_link(s);
        }
        s->status_tx &= ~(KMS_ENABLE | TX_LOOP);
        s->status_tx |= val & (KMS_ENABLE | TX_LOOP);
        break;
    case 3:
        s->command = val;
        break;
    }
}

static uint8_t next_kms_read_byte(NeXTKMSState *s, int n)
{
    switch (n) {
    case 0:
        return s->status_snd;
    case 1:
        return s->status_km;
    case 2:
        return s->status_tx;
    default:
        return s->status_cmd;
    }
}

static uint64_t next_kms_read(void *opaque, hwaddr addr, unsigned size)
{
    NeXTKMSState *s = opaque;
    unsigned off = addr & 0xf;
    uint64_t val = 0;

    if (off < 4) {
        for (unsigned i = 0; i < size; i++) {
            val = (val << 8) | next_kms_read_byte(s, (off + i) & 3);
        }
        return val;
    }
    if (off < 8) {
        return extract32(s->data, (8 - off - size) * 8, size * 8);
    }
    if (off < 12) {
        val = extract32(s->kmdata, (12 - off - size) * 8, size * 8);
        s->status_km &= ~(KM_RECEIVED | KM_INT);
        next_kms_set_irq(s, NEXT_KMS_IRQ_KEYMOUSE, false);
        next_kms_km_next(s);
        return val;
    }
    return 0;
}

static void next_kms_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    NeXTKMSState *s = opaque;
    unsigned off = addr & 0xf;

    if (off < 4) {
        for (unsigned i = 0; i < size; i++) {
            next_kms_write_byte(s, (off + i) & 3,
                                val >> (8 * (size - 1 - i)));
        }
    } else if (off == 4 && size == 4) {
        if (s->status_tx & TX_LOOP) {
            next_kms_command_out(s, s->command, val);
        } else {
            next_kms_command_in(s, s->command, val);
        }
    } else if (off < 8) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "next-kms: %u byte write to the data register\n",
                      size);
    }
}

static const MemoryRegionOps next_kms_ops = {
    .read = next_kms_read,
    .write = next_kms_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
    .endianness = DEVICE_BIG_ENDIAN,
};

void next_kms_set_sound(NeXTKMSState *s, NeXTKMSSoundCommand command,
                        NeXTKMSSoundRequest request, void *opaque)
{
    s->sound_command = command;
    s->sound_request = request;
    s->sound_opaque = opaque;
}

void next_kms_set_power_key(NeXTKMSState *s, NeXTKMSPowerKey fn,
                            void *opaque)
{
    s->power_key = fn;
    s->power_opaque = opaque;
}

static void next_kms_reset_hold(Object *obj, ResetType type)
{
    NeXTKMSState *s = NEXT_KMS(obj);

    s->rev = s->turbo ? REV_NEW : REV_OLD;
    s->modifiers = 0;
    s->button_left = s->button_right = false;
    s->dx = s->dy = 0;
    next_kms_reset_link(s);
}

static void next_kms_realize(DeviceState *dev, Error **errp)
{
    NeXTKMSState *s = NEXT_KMS(dev);

    /* on the Turbo machines the keyboard and mouse are on ADB */
    if (s->input) {
        s->hs = qemu_input_handler_register(dev, &next_kms_handler);
    }
}

static void next_kms_unrealize(DeviceState *dev)
{
    NeXTKMSState *s = NEXT_KMS(dev);

    g_clear_pointer(&s->hs, qemu_input_handler_unregister);
}

static void next_kms_init(Object *obj)
{
    NeXTKMSState *s = NEXT_KMS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mem, obj, &next_kms_ops, s, "next.kms",
                          0x1000);
    sysbus_init_mmio(sbd, &s->mem);
    for (int i = 0; i < NEXT_KMS_NUM_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static const VMStateDescription next_kms_vmstate = {
    .name = "next-kms",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(status_snd, NeXTKMSState),
        VMSTATE_UINT8(status_km, NeXTKMSState),
        VMSTATE_UINT8(status_tx, NeXTKMSState),
        VMSTATE_UINT8(status_cmd, NeXTKMSState),
        VMSTATE_UINT8(command, NeXTKMSState),
        VMSTATE_UINT32(data, NeXTKMSState),
        VMSTATE_UINT32(kmdata, NeXTKMSState),
        VMSTATE_UINT8(rev, NeXTKMSState),
        VMSTATE_UINT32(km_addr, NeXTKMSState),
        VMSTATE_UINT32_ARRAY(km_data, NeXTKMSState, 16),
        VMSTATE_UINT32(km_mask, NeXTKMSState),
        VMSTATE_UINT8(modifiers, NeXTKMSState),
        VMSTATE_BOOL(button_left, NeXTKMSState),
        VMSTATE_BOOL(button_right, NeXTKMSState),
        VMSTATE_UINT32(q_head, NeXTKMSState),
        VMSTATE_UINT32(q_len, NeXTKMSState),
        VMSTATE_UINT32_ARRAY(queue, NeXTKMSState, NEXT_KMS_QUEUE),
        VMSTATE_END_OF_LIST()
    },
};

static const Property next_kms_properties[] = {
    DEFINE_PROP_BOOL("turbo", NeXTKMSState, turbo, false),
    DEFINE_PROP_BOOL("input", NeXTKMSState, input, true),
};

static void next_kms_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->desc = "NeXT keyboard, mouse and sound interface";
    dc->vmsd = &next_kms_vmstate;
    dc->realize = next_kms_realize;
    dc->unrealize = next_kms_unrealize;
    device_class_set_props(dc, next_kms_properties);
    rc->phases.hold = next_kms_reset_hold;
}

static const TypeInfo next_kms_info = {
    .name          = TYPE_NEXT_KMS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NeXTKMSState),
    .instance_init = next_kms_init,
    .class_init    = next_kms_class_init,
};

static void next_kms_register_types(void)
{
    type_register_static(&next_kms_info);
}

type_init(next_kms_register_types)
