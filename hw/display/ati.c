/*
 * QEMU ATI SVGA emulation
 *
 * Copyright (c) 2019 BALATON Zoltan
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

/*
 * WARNING:
 * This is very incomplete and only enough for Linux console and some
 * unaccelerated X output at the moment.
 * Currently it's little more than a frame buffer with minimal functions,
 * other more advanced features of the hardware are yet to be implemented.
 * We only aim for Rage 128 Pro (and some RV100) and 2D only at first,
 * No 3D at all yet (maybe after 2D works, but feel free to improve it)
 */

#include "qemu/osdep.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "vga-access.h"
#include "hw/core/qdev-properties.h"
#include "vga_regs.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "trace.h"

#define ATI_DEBUG_HW_CURSOR 0

#ifdef CONFIG_PIXMAN
#define DEFAULT_X_PIXMAN 3
#else
#define DEFAULT_X_PIXMAN 0
#endif

static const struct {
    const char *name;
    uint16_t dev_id;
} ati_model_aliases[] = {
    { "rage128p", PCI_DEVICE_ID_ATI_RAGE128_PF },
    { "rv100", PCI_DEVICE_ID_ATI_RADEON_QY },
    { "rv200", PCI_DEVICE_ID_ATI_RADEON_QW },
    { "es1000", PCI_DEVICE_ID_ATI_RADEON_ES1000 },
    { "rv250", PCI_DEVICE_ID_ATI_RADEON_LF },
    { "rv280", PCI_DEVICE_ID_ATI_RADEON_9250 },
};

enum { VGA_MODE, EXT_MODE };

static void ati_vga_set_offset(VGACommonState *vga, uint32_t offs)
{
    int bypp = DIV_ROUND_UP(vga->vbe_regs[VBE_DISPI_INDEX_BPP], BITS_PER_BYTE);

    if (!bypp ||
        vga->vbe_regs[VBE_DISPI_INDEX_YRES] *
        vga->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] * bypp + offs >
        vga->vbe_size) {
        return;
    }
    vga->vbe_start_addr = offs / 4;
}

static int ati_nonsurf_xm(ATIVGAState *s);

/* the display reads the visible buffer in the order it is kept in */
static bool ati_big_endian_fb(ATIVGAState *s)
{
    if (!ati_is_r100_r200(s)) {
        /* the Radeon has the aperture swap controls in SURFACE_CNTL */
        return s->regs.config_cntl & (APER_0_ENDIAN | APER_1_ENDIAN) ||
               s->regs.surface_cntl & (NONSURF_AP0_SWP_16BPP |
                                       NONSURF_AP0_SWP_32BPP |
                                       NONSURF_AP1_SWP_16BPP |
                                       NONSURF_AP1_SWP_32BPP);
    }
    return (s->vis_pin ? s->vis_xm : ati_nonsurf_xm(s)) != 0;
}

/*
 * Byte swapping of the frame buffer apertures. QEMU keeps the video memory
 * as the CPU sees it through the apertures, while the engines see it little
 * endian; the swap is a byte address mask within each dword: 1 swaps the
 * bytes of each 16 bit word, 3 those of each 32 bit word. The swap of
 * aperture 0 counts, or that of aperture 1 if 0 does not swap.
 */
static int ati_swp_xm(uint32_t v)
{
    if (v & (NONSURF_AP0_SWP_32BPP | NONSURF_AP0_SWP_16BPP)) {
        return v & NONSURF_AP0_SWP_32BPP ? 3 : 1;
    }
    if (v & (NONSURF_AP1_SWP_32BPP | NONSURF_AP1_SWP_16BPP)) {
        return v & NONSURF_AP1_SWP_32BPP ? 3 : 1;
    }
    return 0;
}

/* the swap outside the surfaces */
static int ati_nonsurf_xm(ATIVGAState *s)
{
    int xm = ati_swp_xm(s->regs.surface_cntl);
    int ap;

    for (ap = 0; !xm && ap < 2; ap++) {
        switch ((s->regs.config_cntl >> (ap * 2)) & 3) {
        case 1:
            xm = 1;
            break;
        case 2:
            xm = 3;
            break;
        }
    }
    return xm;
}

/*
 * Surface n covers [*lo, *hi] of the video memory with its own swap
 * (SURFACE_INFO uses the bits of SURFACE_CNTL). The bounds are offsets in
 * the video memory, in bits 26 to 10, so in 1 KB steps: AmigaOS 4.0 gives
 * GPU addresses, whose top bits the surface does not look at, and AmigaOS
 * 4.1 Final Edition offsets with an upper bound of 0xffffffff. A surface
 * with its upper bound below its lower one is off.
 */
static bool ati_surface(ATIVGAState *s, int n, uint32_t *lo, uint32_t *hi,
                        int *xm)
{
    uint32_t base = SURFACE0_LOWER_BOUND + n * 0x10;
    uint32_t lower = s->mmregs[base / 4] & 0x07fffc00;
    uint32_t upper = s->mmregs[(base + 4) / 4] | 0x3ff;

    upper &= 0x07ffffff;
    if (!s->mmregs[(base + 4) / 4] || upper <= lower ||
        lower >= s->vga.vram_size) {
        return false;
    }
    *lo = lower;
    *hi = MIN(upper, s->vga.vram_size - 1);
    *xm = ati_swp_xm(s->mmregs[(base + 8) / 4]);
    return true;
}

int ati_vram_xm(ATIVGAState *s, uint32_t off)
{
    uint32_t lo, hi;
    int n, xm;

    if (!ati_is_r100_r200(s)) {
        /* all of the memory is kept in the byte order of the display */
        return s->vga.big_endian_fb ? 3 : 0;
    }
    for (n = 0; n < ATI_SURFACES; n++) {
        if (ati_surface(s, n, &lo, &hi, &xm) && off >= lo && off <= hi) {
            return xm;
        }
    }
    if (s->vis_pin && off >= s->vis_lo && off <= s->vis_hi) {
        return s->vis_xm;
    }
    return ati_nonsurf_xm(s);
}

/* reorders the bytes of the dwords in [lo, hi] by the byte address mask */
static void ati_vram_permute(ATIVGAState *s, uint32_t lo, uint32_t hi,
                             int mask)
{
    uint32_t *p, *end, v;

    lo &= ~3;
    if (!mask || lo >= s->vga.vram_size || hi < lo) {
        return;
    }
    hi = MIN(hi | 3, s->vga.vram_size - 1);
    p = (uint32_t *)(s->vga.vram_ptr + lo);
    end = (uint32_t *)(s->vga.vram_ptr + hi + 1);
    /* the same on hosts of either byte order */
    switch (mask) {
    case 1: /* the bytes of each 16 bit word */
        for (; p < end; p++) {
            v = *p;
            *p = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
        }
        break;
    case 2: /* the 16 bit words */
        for (; p < end; p++) {
            v = *p;
            *p = (v << 16) | (v >> 16);
        }
        break;
    default: /* all four bytes */
        for (; p < end; p++) {
            *p = bswap32(*p);
        }
        break;
    }
}

/*
 * The CPU sees [lo, hi] with another swap: reorder the bytes of its dwords
 * from the order of swap 'from' to that of swap 'to'.
 */
static void ati_vram_reswap(ATIVGAState *s, uint32_t lo, uint32_t hi,
                            int from, int to)
{
    if (from == to || lo >= s->vga.vram_size) {
        return;
    }
    ati_vram_permute(s, lo, hi, from ^ to);
    hi = MIN(hi | 3, s->vga.vram_size - 1);
    memory_region_set_dirty(&s->vga.vram, lo & ~3, hi + 1 - (lo & ~3));
}

/* the part of the video memory that the display shows */
static bool ati_visible(ATIVGAState *s, uint32_t *lo, uint32_t *hi, int *bpp)
{
    VGACommonState *vga = &s->vga;
    uint32_t len;

    if (s->mode != EXT_MODE ||
        !(vga->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_ENABLED)) {
        return false;
    }
    *bpp = vga->vbe_regs[VBE_DISPI_INDEX_BPP];
    *lo = vga->vbe_start_addr * 4;
    len = vga->vbe_line_offset * vga->vbe_regs[VBE_DISPI_INDEX_YRES];
    if (!len || *lo >= vga->vram_size) {
        return false;
    }
    *hi = MIN(*lo + len, vga->vram_size) - 1;
    return true;
}

/* whether the display can show pixels of bpp bits kept with swap xm */
static bool ati_xm_displayable(int xm, int bpp)
{
    switch (bpp) {
    case 32:
        return xm == 0 || xm == 3;
    case 15:
    case 16:
        return xm == 0 || xm == 1;
    default:
        return true;
    }
}

/* the swap whose width is that of the pixels: 32 or 16 bit */
static int ati_xm_of_depth(int bpp)
{
    switch (bpp) {
    case 32:
        return 3;
    case 15:
    case 16:
        return 1;
    default:
        return 0;
    }
}

/*
 * The visible buffer goes back to the order of the memory around it, as
 * the display is about to show another one.
 */
static void ati_vis_unpin(ATIVGAState *s)
{
    if (s->vis_pin) {
        s->vis_pin = false;
        ati_vram_reswap(s, s->vis_lo, s->vis_hi, s->vis_xm,
                        ati_nonsurf_xm(s));
    }
}

/*
 * The swap outside the surfaces changes: the memory that no surface
 * covers is reordered to what the CPU now sees, so that the CPU and the
 * engines agree whichever swap was on when it was written. Drivers flip it
 * between bitmaps: AmigaOS writes 16 bit bitmaps with the 16 bit swap and
 * 32 bit ones with the 32 bit swap, and turns it off for 8 bit ones, many
 * times for each window it opens.
 * The visible buffer only follows into the swap of its pixels' width (the
 * 32 bit one for 32 bit pixels, the 16 bit one for 16 bit ones), and out
 * of an order the display cannot show (32 bit pixels with the 16 bit swap,
 * 16 bit ones with the 32 bit swap) into one it can; otherwise it keeps
 * its order. AmigaOS 4.1 still draws its 32 bit screen as with the 32 bit
 * swap while it is off: following it there turned the screen's red and
 * blue around for a moment now and then, and drew checkboxes wrongly.
 */
static void ati_nonsurf_reswap(ATIVGAState *s, int from, int to)
{
    uint32_t lo[ATI_SURFACES + 1], hi[ATI_SURFACES + 1];
    uint32_t start = 0, end, vlo = 0, vhi = 0;
    int n, i, k = 0, xm, bpp = 0, cur, target;
    bool vis;

    if (from == to || !ati_is_r100_r200(s)) {
        return;
    }
    for (n = 0; n < ATI_SURFACES; n++) {
        if (ati_surface(s, n, &lo[k], &hi[k], &xm)) {
            k++;
        }
    }
    vis = ati_visible(s, &vlo, &vhi, &bpp);
    if (s->vis_pin && (!vis || vlo != s->vis_lo || vhi != s->vis_hi)) {
        /* should not happen: the pin goes when the display changes */
        ati_vram_reswap(s, s->vis_lo, s->vis_hi, s->vis_xm, from);
        s->vis_pin = false;
    }
    if (vis) {
        lo[k] = vlo;
        hi[k] = vhi;
        k++;
    }
    while (start < s->vga.vram_size) {
        /* skip the surfaces (and the visible buffer) that cover start */
        for (i = 0; i < k; i++) {
            if ((lo[i] & ~3) <= start && hi[i] >= start) {
                start = (hi[i] | 3) + 1;
                i = -1;
            }
        }
        if (start >= s->vga.vram_size) {
            break;
        }
        /* reorder up to the next one */
        end = s->vga.vram_size;
        for (i = 0; i < k; i++) {
            if ((lo[i] & ~3) > start) {
                end = MIN(end, lo[i] & ~3);
            }
        }
        ati_vram_permute(s, start, end - 1, from ^ to);
        start = end;
    }
    if (vis) {
        cur = s->vis_pin ? s->vis_xm : from;
        if (to == ati_xm_of_depth(bpp) ||
            (!ati_xm_displayable(cur, bpp) && ati_xm_displayable(to, bpp))) {
            target = to;
        } else {
            target = cur;
        }
        ati_vram_reswap(s, vlo, vhi, cur, target);
        s->vis_pin = target != to;
        s->vis_lo = vlo;
        s->vis_hi = vhi;
        s->vis_xm = target;
        s->vga.big_endian_fb = target != 0;
    }
    trace_ati_nonsurf_swap(from, to, vis, vlo, vhi, s->vis_pin,
                           s->vga.big_endian_fb);
}

static inline void ati_reg_write_offs(uint32_t *reg, int offs,
                                      uint64_t data, unsigned int size);

/*
 * The R200 still takes the R100 registers of texture units 0 to 2 (filter,
 * format, offset, size and pitch of non power of two textures), as the
 * MorphOS Radeon driver sets them; they write the R200 ones. PP_CNTL and
 * the viewport have second addresses.
 */
static uint32_t ati_r200_alias(uint32_t addr)
{
    int n;

    /* PP_CNTL, also written at 0x2cc0 by AmigaOS 4.1 and TinyGL */
    if (addr == 0x1c38) {
        return 0x2cc0;
    }
    if (addr == 0x2cc0) {
        return 0x1c38;
    }
    /*
     * SE_VPORT_XSCALE to _ZOFFSET: Mesa writes them at 0x1d98, TinyGL at
     * 0x2098
     */
    if (addr >= 0x1d98 && addr <= 0x1dac) {
        return addr + 0x300;
    }
    if (addr >= 0x2098 && addr <= 0x20ac) {
        return addr - 0x300;
    }
    if (addr >= 0x1c54 && addr < 0x1c54 + 3 * 0x18) {
        n = (addr - 0x1c54) / 0x18;
        switch ((addr - 0x1c54) % 0x18) {
        case 0: /* PP_TXFILTER */
            return 0x2c00 + n * 0x20;
        case 4: /* PP_TXFORMAT */
            return 0x2c04 + n * 0x20;
        case 8: /* PP_TXOFFSET */
            return 0x2d00 + n * 0x18;
        }
        return 0;
    }
    if (addr >= 0x1d04 && addr < 0x1d04 + 3 * 8) {
        n = (addr - 0x1d04) / 8;
        /* PP_TEX_SIZE and PP_TEX_PITCH */
        return (addr & 4 ? 0x2c0c : 0x2c10) + n * 0x20;
    }
    return 0;
}

/*
 * A surface register changes: the old surface's memory goes back to the
 * swap outside the surfaces, the new one's takes its own swap.
 */
static void ati_surface_write(ATIVGAState *s, hwaddr addr, uint64_t data,
                              unsigned int size)
{
    int n = (addr - SURFACE0_LOWER_BOUND) / 0x10;
    int nxm = ati_nonsurf_xm(s);
    uint32_t lo, hi;
    int xm;

    if (ati_surface(s, n, &lo, &hi, &xm)) {
        ati_vram_reswap(s, lo, hi, xm, nxm);
    }
    ati_reg_write_offs(&s->mmregs[addr / 4], addr & 3, data, size);
    if (ati_surface(s, n, &lo, &hi, &xm)) {
        ati_vram_reswap(s, lo, hi, nxm, xm);
    }
}

static void ati_vga_switch_mode(ATIVGAState *s)
{
    ati_vis_unpin(s);
    DPRINTF("%d -> %d\n",
            s->mode, !!(s->regs.crtc_gen_cntl & CRTC2_EXT_DISP_EN));
    if (s->regs.crtc_gen_cntl & CRTC2_EXT_DISP_EN) {
        /* Extended mode enabled */
        s->mode = EXT_MODE;
        if (s->regs.crtc_gen_cntl & CRTC2_EN) {
            /* CRT controller enabled, use CRTC values */
            /* FIXME Should these be the same as VGA CRTC regs? */
            uint32_t offs = s->regs.crtc_offset & 0x07ffffff;
            int stride = (s->regs.crtc_pitch & 0x7ff) * 8;
            int bpp = 0;
            int h, v;

            if (s->regs.crtc_h_total_disp == 0) {
                s->regs.crtc_h_total_disp = ((640 / 8) - 1) << 16;
            }
            if (s->regs.crtc_v_total_disp == 0) {
                s->regs.crtc_v_total_disp = (480 - 1) << 16;
            }
            h = ((s->regs.crtc_h_total_disp >> 16) + 1) * 8;
            v = (s->regs.crtc_v_total_disp >> 16) + 1;
            switch (s->regs.crtc_gen_cntl & CRTC_PIX_WIDTH_MASK) {
            case CRTC_PIX_WIDTH_4BPP:
                bpp = 4;
                break;
            case CRTC_PIX_WIDTH_8BPP:
                bpp = 8;
                break;
            case CRTC_PIX_WIDTH_15BPP:
                bpp = 15;
                break;
            case CRTC_PIX_WIDTH_16BPP:
                bpp = 16;
                break;
            case CRTC_PIX_WIDTH_24BPP:
                bpp = 24;
                break;
            case CRTC_PIX_WIDTH_32BPP:
                bpp = 32;
                break;
            default:
                qemu_log_mask(LOG_UNIMP, "Unsupported bpp value\n");
                return;
            }
            DPRINTF("Switching to %dx%d %d %d @ %x\n", h, v, stride, bpp, offs);
            vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
            vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_DISABLED);
            s->vga.big_endian_fb = ati_big_endian_fb(s);
            /* reset VBE regs then set up mode */
            s->vga.vbe_regs[VBE_DISPI_INDEX_XRES] = h;
            s->vga.vbe_regs[VBE_DISPI_INDEX_YRES] = v;
            s->vga.vbe_regs[VBE_DISPI_INDEX_BPP] = bpp;
            /* enable mode via ioport so it updates vga regs */
            vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
            vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_ENABLED |
                VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM |
                (s->regs.dac_cntl & DAC_8BIT_EN ? VBE_DISPI_8BIT_DAC : 0));
            /* now set offset and stride because enable resets these */
            if (stride) {
                vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_VIRT_WIDTH);
                vbe_ioport_write_data(&s->vga, 0, stride);
            }
            ati_vga_set_offset(&s->vga, offs);
        }
    } else {
        /* VGA mode enabled */
        s->mode = VGA_MODE;
        vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
        vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_DISABLED);
    }
}

/*
 * Radeon cursor modes other than mono: 64x64 pixels of 32 bits.
 * Mode 1, which AIX uses on the ES1000: the low 24 bits are a colour and
 * the top 8 an AND mask, as in the mono mode: a mask of 0 shows the
 * colour, a mask of 0xff XORs it into the screen. The cursors of AIX 5.3
 * only have opaque pixels and 0xff000000, which is clear.
 * Modes 2 and 3 are ARGB, blended into the screen by their alpha: 2 with
 * the colour already multiplied by it (X.org's cursors), 3 without
 * (AmigaOS 4.1 and MorphOS, whose clear pixels are white with alpha 0).
 */
static uint32_t ati_cursor_mode(ATIVGAState *s)
{
    uint32_t mode = s->regs.crtc_gen_cntl & CRTC_CUR_MODE_MASK;

    if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
        return 0;
    }
    /* the ARGB modes only on the R100 and R200, the others show mono */
    if (!ati_is_r100_r200(s) && mode != CRTC_CUR_MODE_24BPP_AND) {
        return 0;
    }
    return mode;
}

static bool ati_cursor_24bpp(ATIVGAState *s)
{
    return ati_cursor_mode(s) == CRTC_CUR_MODE_24BPP_AND;
}

static bool ati_cursor_argb(ATIVGAState *s)
{
    uint32_t mode = ati_cursor_mode(s);

    return mode == CRTC_CUR_MODE_ARGB_PREMULT || mode == CRTC_CUR_MODE_ARGB;
}

static bool ati_cursor_32bpp(ATIVGAState *s)
{
    return ati_cursor_24bpp(s) || ati_cursor_argb(s);
}

/* a pixel as the display reads it, through the swap of its memory */
static uint32_t ati_cursor_pixel(ATIVGAState *s, uint32_t off)
{
    const uint8_t *p = s->vga.vram_ptr;
    int xm = ati_vram_xm(s, off);
    uint32_t v = 0;

    for (int i = 0; i < 4; i++) {
        v |= (uint32_t)p[(off + i) ^ xm] << (i * 8);
    }
    return v;
}

/* an ARGB cursor pixel without the alpha multiplied in, as the UIs take */
static uint32_t ati_cursor_argb_pixel(ATIVGAState *s, uint32_t px)
{
    uint32_t a = px >> 24, v = a << 24;

    if (!a) {
        return 0;
    }
    if (ati_cursor_mode(s) == CRTC_CUR_MODE_ARGB) {
        return px;
    }
    for (int sh = 0; sh < 24; sh += 8) {
        v |= MIN(((px >> sh) & 0xff) * 255 / a, 255) << sh;
    }
    return v;
}

/* blends an ARGB cursor pixel into a screen pixel */
static uint32_t ati_cursor_blend(ATIVGAState *s, uint32_t px, uint32_t dst)
{
    uint32_t a = px >> 24, v = 0xff000000;
    bool premult = ati_cursor_mode(s) == CRTC_CUR_MODE_ARGB_PREMULT;

    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t c = (px >> sh) & 0xff, d = (dst >> sh) & 0xff;

        c = premult ? c + d * (255 - a) / 255 :
                      (c * a + d * (255 - a)) / 255;
        v |= MIN(c, 255) << sh;
    }
    return v;
}

/*
 * At the left and top edges of the screen the guest puts the cursor at 0
 * and writes the hidden part to CUR_HORZ_VERT_OFF. The hardware skips that
 * many pixels at the start of each line. For the hidden lines the guest
 * moves CUR_OFFSET down itself (AIX, Linux and X.org all do), so the
 * hardware only shows 64 minus that many lines.
 */
static int ati_cursor_xoff(uint32_t hv_offs)
{
    return (hv_offs >> 16) & 0x3f;
}

static int ati_cursor_lines(uint32_t hv_offs)
{
    return 64 - (hv_offs & 0x3f);
}

/* what the host cursor is made from: its image, mode, colours and offsets */
static uint32_t ati_cursor_hash(ATIVGAState *s)
{
    uint32_t img = s->regs.cur_offset & 0x07fffff0;
    uint32_t len = ati_cursor_32bpp(s) ? 64 * 64 * 4 : 64 * 16;
    uint32_t regs[] = {
        ati_cursor_mode(s), s->regs.cur_color0, s->regs.cur_color1,
        s->regs.cur_hv_offs, ati_vram_xm(s, img),
    };
    uint32_t h = qemu_cursor_shape(0, regs, sizeof(regs));

    if (img + len <= s->vga.vram_size) {
        h = qemu_cursor_shape(h, s->vga.vram_ptr + img, len);
    }
    return h;
}

/*
 * The cursor engine reads the frame buffer as the card stores it. When the
 * aperture swaps bytes, LEMU keeps the frame buffer in the guest's byte
 * order (big_endian_fb), so a read by the card swaps them back: byte n as
 * the card sees it is byte n ^ 3 here (32 bpp swap) or n ^ 1 (16 bpp swap).
 * HP-UX 11.31 sets the 32 bpp swap and writes its mono cursor through it.
 */
static unsigned ati_fb_swap_xor(ATIVGAState *s)
{
    if (!s->vga.big_endian_fb) {
        return 0;
    }
    if ((s->regs.config_cntl & APER_0_ENDIAN) == 2 ||
        (s->regs.config_cntl & APER_1_ENDIAN) == 8 ||
        s->regs.surface_cntl & (NONSURF_AP0_SWP_32BPP |
                                NONSURF_AP1_SWP_32BPP)) {
        return 3;
    }
    return 1;
}

/*
 * One line of a mono cursor plane: 64 pixels, the leftmost in the top bit.
 * The R100 and R200 keep the frame buffer in their own swap model and read
 * the mono planes as they are stored, as they did before.
 */
static uint64_t ati_cursor_bits(ATIVGAState *s, uint32_t off)
{
    unsigned x = ati_is_r100_r200(s) ? 0 : ati_fb_swap_xor(s);
    uint64_t v = 0;

    for (int i = 0; i < 8; i++) {
        v = v << 8 | s->vga.vram_ptr[(off + i) ^ x];
    }
    return v;
}

/*
 * Used by host side hardware cursor. Each new image goes into a new
 * QEMUCursor: a display (cocoa) may still draw from the old one. An
 * unchanged image is not sent again, as the guest may rewrite CUR_OFFSET
 * at every move.
 */
static void ati_cursor_define(ATIVGAState *s)
{
    uint8_t data[1024];
    uint32_t srcoff;
    int xoff = ati_cursor_xoff(s->regs.cur_hv_offs);
    int lines = ati_cursor_lines(s->regs.cur_hv_offs);
    QEMUCursor *c;

    if ((s->regs.cur_offset & BIT(31)) || s->cursor_guest_mode) {
        return; /* Do not update cursor if locked or rendered by guest */
    }
    s->cursor_hash = ati_cursor_hash(s);
    srcoff = s->regs.cur_offset & 0x07fffff0;
    if (ati_cursor_32bpp(s)) {
        bool argb = ati_cursor_argb(s);

        if (srcoff > s->vga.vram_size - 64 * 64 * 4) {
            return;
        }
        c = cursor_alloc(64, 64);
        for (int i = 0; i < 64; i++) {
            for (int j = 0; j < 64; j++) {
                uint32_t px = argb ? 0 : 0xff000000; /* clear */

                if (i < lines && j < 64 - xoff) {
                    px = ati_cursor_pixel(s, srcoff + i * 64 * 4 +
                                             (j + xoff) * 4);
                }
                if (argb) {
                    c->data[i * 64 + j] = ati_cursor_argb_pixel(s, px);
                } else if (!(px & 0xff000000)) {
                    c->data[i * 64 + j] = px | 0xff000000;
                } else if (px & 0xffffff) {
                    /* the host cursor cannot XOR, show it black */
                    c->data[i * 64 + j] = 0xff000000;
                } else {
                    c->data[i * 64 + j] = 0;
                }
            }
        }
    } else {
        if (srcoff > s->vga.vram_size - 64 * 16) {
            return;
        }
        for (int i = 0; i < 64; i++, srcoff += 16) {
            uint64_t abits = UINT64_MAX, xbits = 0; /* transparent */

            if (i < lines) {
                abits = ati_cursor_bits(s, srcoff) << xoff |
                        (BIT_ULL(xoff) - 1);
                xbits = ati_cursor_bits(s, srcoff + 8) << xoff;
            }
            stq_be_p(&data[i * 8], abits);
            stq_be_p(&data[512 + i * 8], xbits);
        }
        c = cursor_alloc(64, 64);
        cursor_set_mono(c, s->regs.cur_color1, s->regs.cur_color0,
                        &data[512], 1, &data[0]);
    }
    if (s->cursor && !memcmp(c->data, s->cursor->data, 64 * 64 * 4)) {
        cursor_unref(c);
        return;
    }
    cursor_unref(s->cursor);
    s->cursor = c;
    qemu_console_set_cursor(s->vga.con, s->cursor);
}

/* Where the cursor image is, read from the registers as the guest set them */
static bool ati_cursor_state(VGACommonState *vga, QemuCursorState *cs)
{
    ATIVGAState *s = container_of(vga, ATIVGAState, vga);
    uint32_t offs = s->regs.cur_hv_offs;
    uint32_t img = s->regs.cur_offset & 0x07fffff0;
    uint32_t len = ati_cursor_32bpp(s) ? 64 * 64 * 4 : 64 * 16;

    cs->visible = s->regs.crtc_gen_cntl & CRTC2_CUR_EN;
    /* less the part hidden beyond the left and top edges */
    cs->x = (int)(s->regs.cur_hv_pos >> 16) - ati_cursor_xoff(offs);
    cs->y = (int)(s->regs.cur_hv_pos & 0xffff) - (int)(offs & 0x3f);
    cs->w = 64;
    cs->h = 64;
    if (img + len <= s->vga.vram_size) {
        cs->shape = qemu_cursor_shape(0, s->vga.vram_ptr + img, len);
    }
    return true;
}

/* Alternatively support guest rendered hardware cursor */
static void ati_cursor_invalidate(VGACommonState *vga)
{
    ATIVGAState *s = container_of(vga, ATIVGAState, vga);
    int size = (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) ? 64 : 0;

    if (s->regs.cur_offset & BIT(31)) {
        return; /* Do not update cursor if locked */
    }
    if (s->cursor_size != size ||
        vga->hw_cursor_x != s->regs.cur_hv_pos >> 16 ||
        vga->hw_cursor_y != (s->regs.cur_hv_pos & 0xffff) ||
        s->cursor_offset != (s->regs.cur_offset & 0x07fffff0) ||
        s->cursor_hv_offs != s->regs.cur_hv_offs) {
        /* Remove old cursor then update and show new one if needed */
        vga_invalidate_scanlines(vga, vga->hw_cursor_y, vga->hw_cursor_y + 63);
        vga->hw_cursor_x = s->regs.cur_hv_pos >> 16;
        vga->hw_cursor_y = s->regs.cur_hv_pos & 0xffff;
        s->cursor_offset = s->regs.cur_offset & 0x07fffff0;
        s->cursor_hv_offs = s->regs.cur_hv_offs;
        s->cursor_size = size;
        if (size) {
            vga_invalidate_scanlines(vga,
                                     vga->hw_cursor_y, vga->hw_cursor_y + 63);
        }
    }
}

static void ati_cursor_draw_line(VGACommonState *vga, uint8_t *d, int scr_y)
{
    ATIVGAState *s = container_of(vga, ATIVGAState, vga);
    uint32_t h, srcoff, color;
    uint64_t abits, xbits, mask;
    uint32_t *dp = (uint32_t *)d;
    int xoff = ati_cursor_xoff(s->cursor_hv_offs);
    int lines = ati_cursor_lines(s->cursor_hv_offs);

    if (!(s->regs.crtc_gen_cntl & CRTC2_CUR_EN) ||
        scr_y < vga->hw_cursor_y || scr_y >= vga->hw_cursor_y + lines ||
        scr_y > s->regs.crtc_v_total_disp >> 16) {
        return;
    }
    dp = &dp[vga->hw_cursor_x];
    h = ((s->regs.crtc_h_total_disp >> 16) + 1) * 8;
    if (ati_cursor_32bpp(s)) {
        bool argb = ati_cursor_argb(s);

        srcoff = s->cursor_offset + (scr_y - vga->hw_cursor_y) * 64 * 4;
        if (srcoff > s->vga.vram_size - 64 * 4) {
            return;
        }
        srcoff += xoff * 4;
        for (int i = 0; i < 64 - xoff; i++, srcoff += 4) {
            uint32_t px = ati_cursor_pixel(s, srcoff);

            if (vga->hw_cursor_x + i >= h) {
                return; /* end of screen, don't span to next line */
            }
            if (argb) {
                dp[i] = ati_cursor_blend(s, px, dp[i]);
            } else if (px & 0xff000000) {
                dp[i] ^= px & 0xffffff;
            } else {
                dp[i] = px | 0xff000000;
            }
        }
        return;
    }
    srcoff = s->cursor_offset + (scr_y - vga->hw_cursor_y) * 16;
    if (srcoff > s->vga.vram_size - 16) {
        return;
    }
    abits = ati_cursor_bits(s, srcoff) << xoff;
    xbits = ati_cursor_bits(s, srcoff + 8) << xoff;
    mask = BIT_ULL(63);
    for (int i = 0; i < 64 - xoff; i++, mask >>= 1) {
        if (vga->hw_cursor_x + i >= h) {
            return; /* end of screen, don't span to next line */
        }
        if (abits & mask) {
            if (xbits & mask) {
                color = dp[i] ^ 0xffffffff; /* complement */
            } else {
                continue; /* transparent, no change */
            }
        } else {
            color = (xbits & mask ? s->regs.cur_color1 :
                                    s->regs.cur_color0) | 0xff000000;
        }
        dp[i] = color;
    }
}

static uint64_t ati_i2c(bitbang_i2c_interface *i2c, uint64_t data, int base)
{
    bool c = (data & BIT(base + 17) ? !!(data & BIT(base + 1)) : 1);
    bool d = (data & BIT(base + 16) ? !!(data & BIT(base)) : 1);

    bitbang_i2c_set(i2c, BITBANG_I2C_SCL, c);
    d = bitbang_i2c_set(i2c, BITBANG_I2C_SDA, d);

    data &= ~0xf00ULL;
    if (c) {
        data |= BIT(base + 9);
    }
    if (d) {
        data |= BIT(base + 8);
    }
    return data;
}

static void ati_vga_update_irq(ATIVGAState *s)
{
    pci_set_irq(&s->dev, !!(s->regs.gen_int_status & s->regs.gen_int_cntl));
}

static void ati_vga_vblank_irq(void *opaque)
{
    ATIVGAState *s = opaque;

    timer_mod(&s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
    /*
     * On the R100 and R200 the status bits are set every frame, enabled or
     * not: drivers poll them to wait for the vertical retrace (AmigaOS
     * waits for VSYNC). The others only run the timer while the VBLANK
     * interrupt is on.
     */
    if (!ati_is_r100_r200(s)) {
        s->regs.gen_int_status |= CRTC_VBLANK_INT;
        ati_vga_update_irq(s);
        return;
    }
    s->regs.gen_int_status |= CRTC_VBLANK_INT | CRTC_VSYNC_INT |
                              RADEON_CRTC2_VBLANK_STAT;
    ati_vga_update_irq(s);
    /*
     * Guests also change the cursor's image in place, without writing a
     * cursor register (busy pointers): the host's copy follows it.
     */
    if (!s->cursor_guest_mode && (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) &&
        !(s->regs.cur_offset & BIT(31)) &&
        ati_cursor_hash(s) != s->cursor_hash) {
        ati_cursor_define(s);
    }
}

static inline uint32_t ati_reg_read_offs(uint32_t reg, int offs,
                                         unsigned int size)
{
    if (offs == 0 && size == 4) {
        return reg;
    } else {
        return extract32(reg, offs * BITS_PER_BYTE, size * BITS_PER_BYTE);
    }
}

static uint64_t ati_mm_read(void *opaque, hwaddr addr, unsigned int size)
{
    ATIVGAState *s = opaque;
    uint32_t val = 0;

    switch (addr) {
    case MM_INDEX:
        val = s->regs.mm_index;
        break;
    case CLOCK_CNTL_INDEX ... CLOCK_CNTL_INDEX + 3:
        val = ati_reg_read_offs(s->regs.clock_cntl_index,
                                addr - CLOCK_CNTL_INDEX, size);
        break;
    case CLOCK_CNTL_DATA ... CLOCK_CNTL_DATA + 3:
        val = ati_reg_read_offs(s->regs.pll[s->regs.clock_cntl_index & 0x3f],
                                addr - CLOCK_CNTL_DATA, size);
        break;
    case MM_DATA ... MM_DATA + 3:
        /* indexed access to regs or memory */
        if (s->regs.mm_index & BIT(31)) {
            uint32_t idx = s->regs.mm_index & ~BIT(31);
            if (idx <= s->vga.vram_size - size) {
                val = ldn_le_p(s->vga.vram_ptr + idx, size);
            }
        } else if (s->regs.mm_index > MM_DATA + 3) {
            val = ati_mm_read(s, s->regs.mm_index + addr - MM_DATA, size);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "ati_mm_read: mm_index too small: %u\n", s->regs.mm_index);
        }
        break;
    case BIOS_0_SCRATCH ... BUS_CNTL - 1:
    {
        int i = (addr - BIOS_0_SCRATCH) / 4;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF && i > 3) {
            break;
        }
        val = ati_reg_read_offs(s->regs.bios_scratch[i],
                                addr - (BIOS_0_SCRATCH + i * 4), size);
        break;
    }
    case GEN_INT_CNTL:
        val = s->regs.gen_int_cntl;
        break;
    case GEN_INT_STATUS:
        val = s->regs.gen_int_status;
        break;
    case CRTC_GEN_CNTL ... CRTC_GEN_CNTL + 3:
        val = ati_reg_read_offs(s->regs.crtc_gen_cntl,
                                addr - CRTC_GEN_CNTL, size);
        break;
    case CRTC_EXT_CNTL ... CRTC_EXT_CNTL + 3:
        val = ati_reg_read_offs(s->regs.crtc_ext_cntl,
                                addr - CRTC_EXT_CNTL, size);
        break;
    case DAC_CNTL:
        val = s->regs.dac_cntl;
        break;
    case GPIO_VGA_DDC ... GPIO_VGA_DDC + 3:
        val = ati_reg_read_offs(s->regs.gpio_vga_ddc,
                                addr - GPIO_VGA_DDC, size);
        break;
    case GPIO_DVI_DDC ... GPIO_DVI_DDC + 3:
        val = ati_reg_read_offs(s->regs.gpio_dvi_ddc,
                                addr - GPIO_DVI_DDC, size);
        break;
    case GPIO_MONID ... GPIO_MONID + 3:
        val = ati_reg_read_offs(s->regs.gpio_monid,
                                addr - GPIO_MONID, size);
        break;
    case PALETTE_INDEX:
        /* FIXME unaligned access */
        val = vga_ioport_read(&s->vga, VGA_PEL_IR) << 16;
        val |= vga_ioport_read(&s->vga, VGA_PEL_IW) & 0xff;
        break;
    case PALETTE_DATA:
        val = vga_ioport_read(&s->vga, VGA_PEL_D);
        break;
    case PALETTE_30_DATA:
        val = s->regs.palette[vga_ioport_read(&s->vga, VGA_PEL_IR)];
        break;
    case CNFG_CNTL:
        val = s->regs.config_cntl;
        break;
    case CNFG_MEMSIZE:
        val = s->vga.vram_size;
        break;
    case SURFACE_CNTL:
        val = s->regs.surface_cntl;
        break;
    case CONFIG_APER_0_BASE:
    case CONFIG_APER_1_BASE:
        val = pci_default_read_config(&s->dev,
                                      PCI_BASE_ADDRESS_0, size) & 0xfffffff0;
        break;
    case CONFIG_APER_SIZE:
        val = memory_region_size(&s->linear_aper) / 2;
        break;
    case CONFIG_REG_1_BASE:
        val = pci_default_read_config(&s->dev,
                                      PCI_BASE_ADDRESS_2, size) & 0xfffffff0;
        break;
    case CONFIG_REG_APER_SIZE:
        val = memory_region_size(&s->mm) / 2;
        break;
    case HOST_PATH_CNTL:
        val = BIT(23); /* Radeon HDP_APER_CNTL */
        break;
    case MC_STATUS:
        val = 5;
        break;
    case MEM_SDRAM_MODE_REG:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val = BIT(28) | BIT(20);
        }
        break;
    case GPIOPAD_Y:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RADEON_ES1000) {
            /* the AIX driver allows X11 only if this pad reads high */
            val = BIT(15);
        }
        break;
    case RBBM_STATUS:
    case GUI_STAT:
        val = 64; /* free CMDFIFO entries */
        break;
    case RBBM_GUICNTL:
        val = s->regs.rbbm_guicntl;
        break;
    case CRTC_H_TOTAL_DISP:
        val = s->regs.crtc_h_total_disp;
        break;
    case CRTC_H_SYNC_STRT_WID:
        val = s->regs.crtc_h_sync_strt_wid;
        break;
    case CRTC_V_TOTAL_DISP:
        val = s->regs.crtc_v_total_disp;
        break;
    case CRTC_V_SYNC_STRT_WID:
        val = s->regs.crtc_v_sync_strt_wid;
        break;
    case CRTC_OFFSET:
        val = s->regs.crtc_offset;
        break;
    case CRTC_OFFSET_CNTL:
        val = s->regs.crtc_offset_cntl;
        break;
    case CRTC_PITCH:
        val = s->regs.crtc_pitch;
        break;
    case 0xf00 ... 0xfff:
        val = pci_default_read_config(&s->dev, addr - 0xf00, size);
        break;
    case CUR_OFFSET ... CUR_OFFSET + 3:
        val = ati_reg_read_offs(s->regs.cur_offset, addr - CUR_OFFSET, size);
        break;
    case CUR_HORZ_VERT_POSN ... CUR_HORZ_VERT_POSN + 3:
        val = ati_reg_read_offs(s->regs.cur_hv_pos,
                                addr - CUR_HORZ_VERT_POSN, size);
        if (addr + size > CUR_HORZ_VERT_POSN + 3) {
            val |= (s->regs.cur_offset & BIT(31)) >> (4 - size);
        }
        break;
    case CUR_HORZ_VERT_OFF ... CUR_HORZ_VERT_OFF + 3:
        val = ati_reg_read_offs(s->regs.cur_hv_offs,
                                addr - CUR_HORZ_VERT_OFF, size);
        if (addr + size > CUR_HORZ_VERT_OFF + 3) {
            val |= (s->regs.cur_offset & BIT(31)) >> (4 - size);
        }
        break;
    case CUR_CLR0 ... CUR_CLR0 + 3:
        val = ati_reg_read_offs(s->regs.cur_color0, addr - CUR_CLR0, size);
        break;
    case CUR_CLR1 ... CUR_CLR1 + 3:
        val = ati_reg_read_offs(s->regs.cur_color1, addr - CUR_CLR1, size);
        break;
    case DST_OFFSET:
        val = s->regs.dst_offset;
        break;
    case DST_PITCH:
        val = s->regs.dst_pitch;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val |= s->regs.dst_tile << 16;
        }
        break;
    case DST_WIDTH:
        val = s->regs.dst_width;
        break;
    case DST_HEIGHT:
        val = s->regs.dst_height;
        break;
    case SRC_X:
        val = s->regs.src_x;
        break;
    case SRC_Y:
        val = s->regs.src_y;
        break;
    case DST_X:
        val = s->regs.dst_x;
        break;
    case DST_Y:
        val = s->regs.dst_y;
        break;
    case DP_GUI_MASTER_CNTL:
        /* DP_GUI_MASTER_CNTL aliases fields from DP_MIX and DP_DATATYPE */
        val = s->regs.dp_gui_master_cntl |
              ((s->regs.dp_datatype & DP_BRUSH_DATATYPE) >> 4) |
              ((s->regs.dp_datatype & DP_DST_DATATYPE) << 8) |
              ((s->regs.dp_datatype & DP_SRC_DATATYPE) >> 4) |
              (s->regs.dp_mix & DP_ROP3) |
              ((s->regs.dp_mix & DP_SRC_SOURCE) << 16);
        break;
    case SRC_OFFSET:
        val = s->regs.src_offset;
        break;
    case SRC_PITCH:
        val = s->regs.src_pitch;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val |= s->regs.src_tile << 16;
        }
        break;
    case DP_BRUSH_BKGD_CLR:
        val = s->regs.dp_brush_bkgd_clr;
        break;
    case DP_BRUSH_FRGD_CLR:
        val = s->regs.dp_brush_frgd_clr;
        break;
    case BRUSH_DATA0 ... BRUSH_DATA63 + 3:
        val = ati_reg_read_offs(s->regs.brush_data[(addr - BRUSH_DATA0) / 4],
                                addr & 3, size);
        break;
    case DST_LINE_START:
        val = s->regs.dst_line_start;
        break;
    case DP_SRC_FRGD_CLR:
        val = s->regs.dp_src_frgd_clr;
        break;
    case DP_SRC_BKGD_CLR:
        val = s->regs.dp_src_bkgd_clr;
        break;
    case DP_CNTL:
        val = s->regs.dp_cntl;
        break;
    case DP_DATATYPE:
        val = s->regs.dp_datatype;
        break;
    case DP_MIX:
        val = s->regs.dp_mix;
        break;
    case DP_WRITE_MASK:
        val = s->regs.dp_write_mask;
        break;
    case DEFAULT_OFFSET:
        val = s->regs.default_offset;
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val >>= 10;
            val |= s->regs.default_pitch << 16;
            val |= s->regs.default_tile << 30;
        }
        break;
    case DEFAULT_PITCH:
        val = s->regs.default_pitch;
        val |= s->regs.default_tile << 16;
        break;
    case DEFAULT_SC_BOTTOM_RIGHT:
        val = s->regs.default_sc_right;
        val |= s->regs.default_sc_bottom << 16;
        break;
    case SC_TOP:
        val = s->regs.sc_top;
        break;
    case SC_LEFT:
        val = s->regs.sc_left;
        break;
    case SC_BOTTOM:
        val = s->regs.sc_bottom;
        break;
    case SC_RIGHT:
        val = s->regs.sc_right;
        break;
    case SRC_SC_BOTTOM:
        val = s->regs.src_sc_bottom;
        break;
    case SRC_SC_RIGHT:
        val = s->regs.src_sc_right;
        break;
    case SC_TOP_LEFT:
    case SC_BOTTOM_RIGHT:
    case SRC_SC_BOTTOM_RIGHT:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from write-only register 0x%x\n", (unsigned)addr);
        break;
    default:
        /*
         * On the R100 and R200, registers we do not model read back what
         * was written; on the others they read 0.
         */
        if (ati_is_r100_r200(s) && !ati_cp_mm_read(s, addr, size, &val) &&
            addr < memory_region_size(&s->mm)) {
            val = ati_reg_read_offs(s->mmregs[addr / 4], addr & 3, size);
        }
        break;
    }
    if (addr < CUR_OFFSET || addr > CUR_CLR1 || ATI_DEBUG_HW_CURSOR) {
        trace_ati_mm_read(size, addr, ati_reg_name(addr & ~3ULL), val);
    }
    return val;
}

static inline void ati_reg_write_offs(uint32_t *reg, int offs,
                                      uint64_t data, unsigned int size)
{
    if (offs == 0 && size == 4) {
        *reg = data;
    } else {
        *reg = deposit32(*reg, offs * BITS_PER_BYTE, size * BITS_PER_BYTE,
                         data);
    }
}

/*
 * With GMC_SRC/DST_PITCH_OFFSET_CNTL clear the engine uses the default pitch
 * and offset, which may also be set after DP_GUI_MASTER_CNTL (AIX does).
 */
static void ati_update_default_pitch_offset(ATIVGAState *s)
{
    if (!(s->regs.dp_gui_master_cntl & GMC_SRC_PITCH_OFFSET_CNTL)) {
        s->regs.src_offset = s->regs.default_offset;
        s->regs.src_pitch = s->regs.default_pitch;
    }
    if (!(s->regs.dp_gui_master_cntl & GMC_DST_PITCH_OFFSET_CNTL)) {
        s->regs.dst_offset = s->regs.default_offset;
        s->regs.dst_pitch = s->regs.default_pitch;
    }
}

void ati_mm_write(void *opaque, hwaddr addr, uint64_t data, unsigned int size)
{
    ATIVGAState *s = opaque;

    if (addr < CUR_OFFSET || addr > CUR_CLR1 || ATI_DEBUG_HW_CURSOR) {
        trace_ati_mm_write(size, addr, ati_reg_name(addr & ~3ULL), data);
    }
    switch (addr) {
    case MM_INDEX:
        s->regs.mm_index = data & ~3;
        break;
    case CLOCK_CNTL_INDEX ... CLOCK_CNTL_INDEX + 3:
        ati_reg_write_offs(&s->regs.clock_cntl_index,
                           addr - CLOCK_CNTL_INDEX, data, size);
        s->regs.clock_cntl_index &= 0x3bf;
        break;
    case CLOCK_CNTL_DATA ... CLOCK_CNTL_DATA + 3:
    {
        /*
         * Writes are taken without PLL_WR_EN too: the HP-UX X server
         * writes the data when the index is already right, and may not
         * have set PLL_WR_EN.
         */
        int idx = s->regs.clock_cntl_index & 0x3f;

        ati_reg_write_offs(&s->regs.pll[idx], addr - CLOCK_CNTL_DATA,
                           data, size);
        if (idx == PPLL_REF_DIV || idx == P2PLL_REF_DIV) {
            /* the PLL takes the new dividers at once */
            s->regs.pll[idx] &= ~PPLL_ATOMIC_UPDATE_R;
        }
        break;
    }
    case MM_DATA ... MM_DATA + 3:
        /* indexed access to regs or memory */
        if (s->regs.mm_index & BIT(31)) {
            uint32_t idx = s->regs.mm_index & ~BIT(31);
            if (idx <= s->vga.vram_size - size) {
                stn_le_p(s->vga.vram_ptr + idx, size, data);
            }
        } else if (s->regs.mm_index > MM_DATA + 3) {
            ati_mm_write(s, s->regs.mm_index + addr - MM_DATA, data, size);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "ati_mm_write: mm_index too small: %u\n", s->regs.mm_index);
        }
        break;
    case BIOS_0_SCRATCH ... BUS_CNTL - 1:
    {
        int i = (addr - BIOS_0_SCRATCH) / 4;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF && i > 3) {
            break;
        }
        ati_reg_write_offs(&s->regs.bios_scratch[i],
                           addr - (BIOS_0_SCRATCH + i * 4), data, size);
        break;
    }
    case GEN_INT_CNTL:
        s->regs.gen_int_cntl = data;
        if (ati_is_r100_r200(s)) {
            /* the frame timer always runs, see ati_vga_vblank_irq() */
            ati_vga_update_irq(s);
        } else if (data & CRTC_VBLANK_INT) {
            ati_vga_vblank_irq(s);
        } else {
            timer_del(&s->vblank_timer);
            ati_vga_update_irq(s);
        }
        break;
    case GEN_INT_STATUS:
        if (ati_is_r100_r200(s)) {
            bool fire = data & RADEON_SW_INT_FIRE;

            /* SW_INT_TEST is acked by writing it, as the other bits */
            data &= (0xfc080effUL | RADEON_SW_INT_TEST) & ~RADEON_SW_INT_FIRE;
            s->regs.gen_int_status &= ~data;
            if (fire) {
                /* the CP writes this to raise an interrupt (fences) */
                s->regs.gen_int_status |= RADEON_SW_INT_TEST;
            }
        } else {
            data &= (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF ?
                     0x000f040fUL : 0xfc080effUL);
            s->regs.gen_int_status &= ~data;
        }
        ati_vga_update_irq(s);
        break;
    case CRTC_GEN_CNTL ... CRTC_GEN_CNTL + 3:
    {
        uint32_t val = s->regs.crtc_gen_cntl;
        ati_reg_write_offs(&s->regs.crtc_gen_cntl,
                           addr - CRTC_GEN_CNTL, data, size);
        if ((val ^ s->regs.crtc_gen_cntl) & CRTC_CUR_MODE_MASK &&
            val & s->regs.crtc_gen_cntl & CRTC2_CUR_EN) {
            /* the cursor stays on in another mode: show it again */
            if (s->cursor_guest_mode) {
                vga_invalidate_scanlines(&s->vga, s->vga.hw_cursor_y,
                                         s->vga.hw_cursor_y + 63);
            } else {
                ati_cursor_define(s);
            }
        }
        if ((val & CRTC2_CUR_EN) != (s->regs.crtc_gen_cntl & CRTC2_CUR_EN)) {
            ati_vga_switch_mode(s);
            if (s->cursor_guest_mode) {
                s->vga.force_shadow = !!(s->regs.crtc_gen_cntl & CRTC2_CUR_EN);
            } else {
                if (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) {
                    ati_cursor_define(s);
                }
                qemu_console_set_mouse(s->vga.con, s->regs.cur_hv_pos >> 16,
                                       s->regs.cur_hv_pos & 0xffff,
                                       (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) != 0);
            }
        }
        if ((val ^ s->regs.crtc_gen_cntl) &
            (CRTC2_EXT_DISP_EN | CRTC2_EN |
             (ati_is_r100_r200(s) ? CRTC_PIX_WIDTH_MASK : 0))) {
            /* MorphOS changes the depth while the CRTC stays on */
            ati_vga_switch_mode(s);
        }
        break;
    }
    case CRTC_EXT_CNTL ... CRTC_EXT_CNTL + 3:
    {
        uint32_t val = s->regs.crtc_ext_cntl;
        ati_reg_write_offs(&s->regs.crtc_ext_cntl,
                           addr - CRTC_EXT_CNTL, data, size);
        if (s->regs.crtc_ext_cntl & CRT_CRTC_DISPLAY_DIS) {
            DPRINTF("Display disabled\n");
            s->vga.ar_index &= ~BIT(5);
        } else {
            DPRINTF("Display enabled\n");
            s->vga.ar_index |= BIT(5);
            ati_vga_switch_mode(s);
        }
        if ((val & CRT_CRTC_DISPLAY_DIS) !=
            (s->regs.crtc_ext_cntl & CRT_CRTC_DISPLAY_DIS)) {
            ati_vga_switch_mode(s);
        }
        break;
    }
    case DAC_CNTL:
        s->regs.dac_cntl = data & 0xffffe3ff;
        s->vga.dac_8bit = !!(data & DAC_8BIT_EN);
        break;
    /*
     * GPIO regs for DDC access. Because some drivers access these via
     * multiple byte writes we have to be careful when we send bits to
     * avoid spurious changes in bitbang_i2c state. Only do it when either
     * the enable bits are changed or output bits changed while enabled.
     */
    case GPIO_VGA_DDC ... GPIO_VGA_DDC + 3:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            /* FIXME: Maybe add a property to select VGA or DVI port? */
        }
        break;
    case GPIO_DVI_DDC ... GPIO_DVI_DDC + 3:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            ati_reg_write_offs(&s->regs.gpio_dvi_ddc,
                               addr - GPIO_DVI_DDC, data, size);
            if ((addr <= GPIO_DVI_DDC + 2 && addr + size > GPIO_DVI_DDC + 2) ||
                (addr == GPIO_DVI_DDC && (s->regs.gpio_dvi_ddc & 0x30000))) {
                s->regs.gpio_dvi_ddc = ati_i2c(&s->bbi2c,
                                               s->regs.gpio_dvi_ddc, 0);
            }
        }
        break;
    case GPIO_MONID ... GPIO_MONID + 3:
        /* FIXME What does Radeon have here? */
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            /* Rage128p accesses DDC via MONID(1-2) with additional mask bit */
            ati_reg_write_offs(&s->regs.gpio_monid,
                               addr - GPIO_MONID, data, size);
            if ((s->regs.gpio_monid & BIT(25)) &&
                ((addr <= GPIO_MONID + 2 && addr + size > GPIO_MONID + 2) ||
                 (addr == GPIO_MONID && (s->regs.gpio_monid & 0x60000)))) {
                s->regs.gpio_monid = ati_i2c(&s->bbi2c, s->regs.gpio_monid, 1);
            }
        }
        break;
    case PALETTE_INDEX ... PALETTE_INDEX + 3:
        if (size == 4) {
            vga_ioport_write(&s->vga, VGA_PEL_IR, (data >> 16) & 0xff);
            vga_ioport_write(&s->vga, VGA_PEL_IW, data & 0xff);
        } else {
            if (addr == PALETTE_INDEX) {
                vga_ioport_write(&s->vga, VGA_PEL_IW, data & 0xff);
            } else {
                vga_ioport_write(&s->vga, VGA_PEL_IR, data & 0xff);
            }
        }
        break;
    case PALETTE_DATA ... PALETTE_DATA + 3:
        data <<= addr - PALETTE_DATA;
        data = bswap32(data) >> 8;
        vga_ioport_write(&s->vga, VGA_PEL_D, data & 0xff);
        data >>= 8;
        vga_ioport_write(&s->vga, VGA_PEL_D, data & 0xff);
        data >>= 8;
        vga_ioport_write(&s->vga, VGA_PEL_D, data & 0xff);
        break;
    case PALETTE_30_DATA:
        s->regs.palette[vga_ioport_read(&s->vga, VGA_PEL_IW)] = data;
        vga_ioport_write(&s->vga, VGA_PEL_D, (data >> 22) & 0xff);
        vga_ioport_write(&s->vga, VGA_PEL_D, (data >> 12) & 0xff);
        vga_ioport_write(&s->vga, VGA_PEL_D, (data >> 2) & 0xff);
        break;
    case CNFG_CNTL: {
        int xm = ati_nonsurf_xm(s);

        s->regs.config_cntl = data;
        ati_nonsurf_reswap(s, xm, ati_nonsurf_xm(s));
        break;
    }
    case 0x2200 ... 0x220f: /* SE_TCL_VECTOR/SCALAR_INDX/DATA_REG */
        if (ati_is_r100_r200(s) && size == 4 && !(addr & 3)) {
            s->mmregs[addr / 4] = data;
            trace_ati_reg_store(addr, data);
            ati_tcl_write(s, addr, data);
        }
        break;
    case SURFACE_CNTL:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            int xm = ati_nonsurf_xm(s);

            trace_ati_reg_store(addr, data);
            s->regs.surface_cntl = data;
            ati_nonsurf_reswap(s, xm, ati_nonsurf_xm(s));
            if (s->mode == EXT_MODE) {
                s->vga.big_endian_fb = ati_big_endian_fb(s);
            }
        }
        break;
    case CRTC_H_TOTAL_DISP:
        if (s->regs.crtc_h_total_disp != (data & 0x07ff07ff)) {
            s->regs.crtc_h_total_disp = data & 0x07ff07ff;
            if (ati_is_r100_r200(s) && s->mode == EXT_MODE) {
                ati_vga_switch_mode(s);
            }
        }
        break;
    case CRTC_H_SYNC_STRT_WID:
        s->regs.crtc_h_sync_strt_wid = data & 0x17bf1fff;
        break;
    case CRTC_V_TOTAL_DISP:
        if (s->regs.crtc_v_total_disp != (data & 0x0fff0fff)) {
            s->regs.crtc_v_total_disp = data & 0x0fff0fff;
            if (ati_is_r100_r200(s) && s->mode == EXT_MODE) {
                ati_vga_switch_mode(s);
            }
        }
        break;
    case CRTC_V_SYNC_STRT_WID:
        s->regs.crtc_v_sync_strt_wid = data & 0x9f0fff;
        break;
    case CRTC_OFFSET:
        ati_vis_unpin(s);
        s->regs.crtc_offset = data & 0x87fffff8;
        ati_vga_set_offset(&s->vga, s->regs.crtc_offset & 0x07ffffff);
        if (ati_is_r100_r200(s) && s->mode == EXT_MODE) {
            s->vga.big_endian_fb = ati_big_endian_fb(s);
        }
        break;
    case CRTC_OFFSET_CNTL:
        s->regs.crtc_offset_cntl = data; /* FIXME */
        break;
    case CRTC_PITCH:
        data &= 0x07ff07ff;
        if (s->regs.crtc_pitch != data) {
            s->regs.crtc_pitch = data;
            ati_vga_switch_mode(s);
        }
        break;
    case 0xf00 ... 0xfff:
        /* read-only copy of PCI config space so ignore writes */
        break;
    case CUR_OFFSET ... CUR_OFFSET + 3:
    {
        uint32_t t = s->regs.cur_offset;
        bool unlock;

        ati_reg_write_offs(&t, addr - CUR_OFFSET, data, size);
        t &= 0x87fffff0;
        unlock = (s->regs.cur_offset & ~t) & BIT(31);
        if (s->regs.cur_offset != t) {
            s->regs.cur_offset = t;
            ati_cursor_define(s);
        }
        /*
         * X.org and the HP-UX 11.31 X server move the cursor with
         * CUR_HORZ_VERT_OFF and _POSN locked, then unlock it here.
         */
        if (unlock && !s->cursor_guest_mode &&
            (s->regs.crtc_gen_cntl & CRTC2_CUR_EN)) {
            qemu_console_set_mouse(s->vga.con, s->regs.cur_hv_pos >> 16,
                                   s->regs.cur_hv_pos & 0xffff, true);
        }
        break;
    }
    case CUR_HORZ_VERT_POSN ... CUR_HORZ_VERT_POSN + 3:
    {
        uint32_t t = s->regs.cur_hv_pos | (s->regs.cur_offset & BIT(31));

        ati_reg_write_offs(&t, addr - CUR_HORZ_VERT_POSN, data, size);
        s->regs.cur_hv_pos = t & 0x3fff0fff;
        if (t & BIT(31)) {
            s->regs.cur_offset |= t & BIT(31);
        } else if (s->regs.cur_offset & BIT(31)) {
            s->regs.cur_offset &= ~BIT(31);
            ati_cursor_define(s);
        }
        if (!s->cursor_guest_mode &&
            (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) && !(t & BIT(31))) {
            qemu_console_set_mouse(s->vga.con, s->regs.cur_hv_pos >> 16,
                                   s->regs.cur_hv_pos & 0xffff, true);
        }
        break;
    }
    case CUR_HORZ_VERT_OFF:
    {
        uint32_t t = s->regs.cur_hv_offs | (s->regs.cur_offset & BIT(31));

        ati_reg_write_offs(&t, addr - CUR_HORZ_VERT_OFF, data, size);
        s->regs.cur_hv_offs = t & 0x3f003f;
        if (t & BIT(31)) {
            s->regs.cur_offset |= t & BIT(31);
        } else if (s->regs.cur_offset & BIT(31)) {
            s->regs.cur_offset &= ~BIT(31);
            ati_cursor_define(s);
        }
        break;
    }
    case CUR_CLR0 ... CUR_CLR0 + 3:
    {
        uint32_t t = s->regs.cur_color0;

        ati_reg_write_offs(&t, addr - CUR_CLR0, data, size);
        t &= 0xffffff;
        if (s->regs.cur_color0 != t) {
            s->regs.cur_color0 = t;
            ati_cursor_define(s);
        }
        break;
    }
    case CUR_CLR1 ... CUR_CLR1 + 3:
        /*
         * Update cursor unconditionally here because some clients set up
         * other registers before actually writing cursor data to memory at
         * offset so we would miss cursor change unless always updating here
         */
        ati_reg_write_offs(&s->regs.cur_color1, addr - CUR_CLR1, data, size);
        s->regs.cur_color1 &= 0xffffff;
        ati_cursor_define(s);
        break;
    case DST_OFFSET:
            s->regs.dst_offset = data & 0xfffffff0;
        break;
    case DST_PITCH:
            s->regs.dst_pitch = data & 0x3fff;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.dst_tile = (data >> 16) & 1;
        }
        break;
    case DST_TILE:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.dst_tile = data & 3;
        }
        break;
    case DST_WIDTH:
        s->regs.dst_width = data & 0x3fff;
        ati_2d_blt(s);
        break;
    case DST_HEIGHT:
        s->regs.dst_height = data & 0x3fff;
        break;
    case SRC_X:
        s->regs.src_x = data & 0x3fff;
        break;
    case SRC_Y:
        s->regs.src_y = data & 0x3fff;
        break;
    case DST_X:
        s->regs.dst_x = data & 0x3fff;
        break;
    case DST_Y:
        s->regs.dst_y = data & 0x3fff;
        break;
    case SRC_PITCH_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.src_offset = (data & 0x1fffff) << 5;
            s->regs.src_pitch = (data & 0x7fe00000) >> 21;
            s->regs.src_tile = data >> 31;
        } else {
            s->regs.src_offset = (data & 0x3fffff) << 10;
            s->regs.src_pitch = (data & 0x3fc00000) >> 16;
            s->regs.src_tile = (data >> 30) & 1;
        }
        break;
    case DST_PITCH_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.dst_offset = (data & 0x1fffff) << 5;
            s->regs.dst_pitch = (data & 0x7fe00000) >> 21;
            s->regs.dst_tile = data >> 31;
        } else {
            s->regs.dst_offset = (data & 0x3fffff) << 10;
            s->regs.dst_pitch = (data & 0x3fc00000) >> 16;
            s->regs.dst_tile = data >> 30;
        }
        break;
    case SRC_Y_X:
        s->regs.src_x = data & 0x3fff;
        s->regs.src_y = (data >> 16) & 0x3fff;
        break;
    case DST_Y_X:
        s->regs.dst_x = data & 0x3fff;
        s->regs.dst_y = (data >> 16) & 0x3fff;
        break;
    case DST_HEIGHT_WIDTH:
        s->regs.dst_width = data & 0x3fff;
        s->regs.dst_height = (data >> 16) & 0x3fff;
        ati_2d_blt(s);
        break;
    case DP_GUI_MASTER_CNTL:
        s->regs.dp_gui_master_cntl = data & 0xf800000f;
        s->regs.dp_datatype = (data & 0x0f00) >> 8 | (data & 0x30f0) << 4 |
                              (data & 0x4000) << 16 |
                              (s->regs.dp_datatype & DP_HOST_BIG_ENDIAN_EN);
        s->regs.dp_mix = (data & GMC_ROP3_MASK) | (data & 0x7000000) >> 16;
        ati_update_default_pitch_offset(s);
        if (!(data & GMC_SRC_CLIPPING)) {
            s->regs.src_sc_right = s->regs.default_sc_right;
            s->regs.src_sc_bottom = s->regs.default_sc_bottom;
        }
        if (!(data & GMC_DST_CLIPPING)) {
            s->regs.sc_top = 0;
            s->regs.sc_left = 0;
            s->regs.sc_right = s->regs.default_sc_right;
            s->regs.sc_bottom = s->regs.default_sc_bottom;
        }
        break;
    case DST_WIDTH_X:
        s->regs.dst_x = data & 0x3fff;
        s->regs.dst_width = (data >> 16) & 0x3fff;
        ati_2d_blt(s);
        break;
    case SRC_X_Y:
        s->regs.src_y = data & 0x3fff;
        s->regs.src_x = (data >> 16) & 0x3fff;
        break;
    case DST_X_Y:
        s->regs.dst_y = data & 0x3fff;
        s->regs.dst_x = (data >> 16) & 0x3fff;
        break;
    case DST_WIDTH_HEIGHT:
        s->regs.dst_height = data & 0x3fff;
        s->regs.dst_width = (data >> 16) & 0x3fff;
        ati_2d_blt(s);
        break;
    case DST_HEIGHT_Y:
        s->regs.dst_y = data & 0x3fff;
        s->regs.dst_height = (data >> 16) & 0x3fff;
        break;
    case SRC_OFFSET:
            s->regs.src_offset = data & 0xfffffff0;
        break;
    case SRC_PITCH:
            s->regs.src_pitch = data & 0x3fff;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.src_tile = (data >> 16) & 1;
        }
        break;
    case DP_BRUSH_BKGD_CLR:
        s->regs.dp_brush_bkgd_clr = data;
        break;
    case DP_BRUSH_FRGD_CLR:
        s->regs.dp_brush_frgd_clr = data;
        break;
    case BRUSH_DATA0 ... BRUSH_DATA63 + 3:
        ati_reg_write_offs(&s->regs.brush_data[(addr - BRUSH_DATA0) / 4],
                           addr & 3, data, size);
        break;
    case DST_LINE_START:
        s->regs.dst_line_start = data;
        break;
    case DST_LINE_END:
        ati_2d_line(s, data);
        break;
    case DP_CNTL:
        s->regs.dp_cntl = data;
        break;
    case DP_SRC_FRGD_CLR:
        s->regs.dp_src_frgd_clr = data;
        break;
    case DP_SRC_BKGD_CLR:
        s->regs.dp_src_bkgd_clr = data;
        break;
    case DP_DATATYPE:
        s->regs.dp_datatype = data & 0xe0070f0f;
        break;
    case DP_MIX:
        s->regs.dp_mix = data & 0x00ff0700;
        break;
    case DP_WRITE_MASK:
        s->regs.dp_write_mask = data;
        break;
    case DP_CNTL_XDIR_YDIR_YMAJOR:
        s->regs.dp_cntl &= ~(DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM |
                             DST_Y_MAJOR);
        s->regs.dp_cntl |= (data & BIT(31) ? DST_X_LEFT_TO_RIGHT : 0) |
                           (data & BIT(15) ? DST_Y_TOP_TO_BOTTOM : 0) |
                           (data & BIT(2) ? DST_Y_MAJOR : 0);
        break;
    case DEFAULT_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.default_offset = data & 0xfffffff0;
        } else {
            /* Radeon has DEFAULT_PITCH_OFFSET here like DST_PITCH_OFFSET */
            s->regs.default_offset = (data & 0x3fffff) << 10;
            s->regs.default_pitch = (data & 0x3fc00000) >> 16;
            s->regs.default_tile = data >> 30;
        }
        ati_update_default_pitch_offset(s);
        break;
    case DEFAULT_PITCH:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.default_pitch = data & 0x3fff;
            s->regs.default_tile = (data >> 16) & 1;
            ati_update_default_pitch_offset(s);
        }
        break;
    case DEFAULT_SC_BOTTOM_RIGHT:
        s->regs.default_sc_right = data & 0x3fff;
        s->regs.default_sc_bottom = (data >> 16) & 0x3fff;
        break;
    case SC_TOP_LEFT:
        s->regs.sc_left = data & 0x3fff;
        s->regs.sc_top = (data >> 16) & 0x3fff;
        break;
    case SC_LEFT:
        s->regs.sc_left = data & 0x3fff;
        break;
    case SC_TOP:
        s->regs.sc_top = data & 0x3fff;
        break;
    case SC_BOTTOM_RIGHT:
        s->regs.sc_right = data & 0x3fff;
        s->regs.sc_bottom = (data >> 16) & 0x3fff;
        break;
    case SC_RIGHT:
        s->regs.sc_right = data & 0x3fff;
        break;
    case SC_BOTTOM:
        s->regs.sc_bottom = data & 0x3fff;
        break;
    case SRC_SC_BOTTOM_RIGHT:
        s->regs.src_sc_right = data & 0x3fff;
        s->regs.src_sc_bottom = (data >> 16) & 0x3fff;
        break;
    case SRC_SC_RIGHT:
        s->regs.src_sc_right = data & 0x3fff;
        break;
    case SRC_SC_BOTTOM:
        s->regs.src_sc_bottom = data & 0x3fff;
        break;
    case RBBM_GUICNTL:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.rbbm_guicntl = data & HOST_DATA_SWAP_MASK;
        }
        break;
    case HOST_DATA0:
    case HOST_DATA1:
    case HOST_DATA2:
    case HOST_DATA3:
    case HOST_DATA4:
    case HOST_DATA5:
    case HOST_DATA6:
    case HOST_DATA7:
    case HOST_DATA_LAST:
        if (!s->host_data.active) {
            break;
        }
        switch (s->regs.rbbm_guicntl & HOST_DATA_SWAP_MASK) {
        case HOST_DATA_SWAP_16BIT:
            data = ((data & 0x00ff00ff) << 8) | ((data >> 8) & 0x00ff00ff);
            break;
        case HOST_DATA_SWAP_32BIT:
            data = bswap32(data);
            break;
        case HOST_DATA_SWAP_HDW:
            data = ror32(data, 16);
            break;
        }
        s->host_data.acc[s->host_data.next++] = data;
        if (addr == HOST_DATA_LAST) {
            ati_host_data_finish(s);
            s->host_data.next = 0;
        } else if (s->host_data.next >= 4) {
            ati_host_data_flush(s);
            s->host_data.next = 0;
        }
        break;
    case SURFACE0_LOWER_BOUND ... SURFACE0_LOWER_BOUND + ATI_SURFACES * 0x10 - 1:
        if ((addr & 0xf) && ati_is_r100_r200(s)) {
            ati_surface_write(s, addr, data, size);
            break;
        }
        /* fall through */
    default:
        /* only the R100 and R200 keep what is written to other registers */
        if (ati_is_r100_r200(s) && !ati_cp_mm_write(s, addr, data, size) &&
            addr < memory_region_size(&s->mm)) {
            uint32_t alias;

            ati_reg_write_offs(&s->mmregs[addr / 4], addr & 3, data, size);
            trace_ati_reg_store(addr, s->mmregs[addr / 4]);
            alias = ati_is_r200(s) ? ati_r200_alias(addr) : 0;
            if (alias) {
                s->mmregs[alias / 4] = s->mmregs[addr / 4];
            }
        }
        break;
    }
}

/*
 * With APER_REG_ENDIAN set the register aperture swaps the bytes of each
 * 32-bit word, so a big endian host can use plain loads and stores (the
 * AIX X server does). The command queue apertures are not swapped: their
 * data goes to the CP as it is, in the little endian order of a ring
 * buffer in memory, which is how the AmigaOS 4.1 Radeon driver writes its
 * PIO packets there.
 */
static bool ati_mm_aper_swapped(ATIVGAState *s, hwaddr addr)
{
    if (!(s->regs.config_cntl & APER_REG_ENDIAN)) {
        return false;
    }
    if (ati_is_r100_r200(s) &&
        ((addr >= RADEON_CP_CSQ_APER_PRIMARY &&
          addr < RADEON_CP_CSQ_APER_PRIMARY + 0x200) ||
         (addr >= RADEON_CP_CSQ_APER_INDIRECT &&
          addr < RADEON_CP_CSQ_APER_INDIRECT + 0x100))) {
        return false;
    }
    return true;
}

static uint64_t ati_mm_aper_read(void *opaque, hwaddr addr, unsigned int size)
{
    ATIVGAState *s = opaque;

    if (!ati_mm_aper_swapped(s, addr)) {
        return ati_mm_read(s, addr, size);
    }
    switch (size) {
    case 1:
        return ati_mm_read(s, addr ^ 3, size);
    case 2:
        return bswap16(ati_mm_read(s, addr ^ 2, size));
    default:
        return bswap32(ati_mm_read(s, addr, size));
    }
}

static void ati_mm_aper_write(void *opaque, hwaddr addr,
                              uint64_t data, unsigned int size)
{
    ATIVGAState *s = opaque;

    if (!ati_mm_aper_swapped(s, addr)) {
        ati_mm_write(s, addr, data, size);
        return;
    }
    switch (size) {
    case 1:
        ati_mm_write(s, addr ^ 3, data, size);
        break;
    case 2:
        ati_mm_write(s, addr ^ 2, bswap16(data), size);
        break;
    default:
        ati_mm_write(s, addr, bswap32(data), size);
        break;
    }
}

static const MemoryRegionOps ati_mm_ops = {
    .read = ati_mm_aper_read,
    .write = ati_mm_aper_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void ati_vga_realize(PCIDevice *dev, Error **errp)
{
    ATIVGAState *s = ATI_VGA(dev);
    VGACommonState *vga = &s->vga;
    I2CBus *i2cbus;

#ifndef CONFIG_PIXMAN
    if (s->use_pixman != 0) {
        warn_report("x-pixman != 0, not effective without PIXMAN");
    }
#endif

    if (s->model) {
        int i;
        for (i = 0; i < ARRAY_SIZE(ati_model_aliases); i++) {
            if (!strcmp(s->model, ati_model_aliases[i].name)) {
                s->dev_id = ati_model_aliases[i].dev_id;
                break;
            }
        }
        if (i >= ARRAY_SIZE(ati_model_aliases)) {
            warn_report("Unknown ATI VGA model name, "
                        "using default rage128p");
        }
    }
    if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF &&
        s->dev_id != PCI_DEVICE_ID_ATI_RADEON_QY &&
        s->dev_id != PCI_DEVICE_ID_ATI_RADEON_QW &&
        s->dev_id != PCI_DEVICE_ID_ATI_RADEON_ES1000 &&
        s->dev_id != PCI_DEVICE_ID_ATI_RADEON_LF &&
        s->dev_id != PCI_DEVICE_ID_ATI_RADEON_9250) {
        error_setg(errp, "Unknown ATI VGA device id, only 0x5046, 0x5159, "
                   "0x5157, 0x515e, 0x4c66 and 0x5960 are supported");
        return;
    }
    pci_set_word(dev->config + PCI_DEVICE_ID, s->dev_id);
    if (s->dev_id == PCI_DEVICE_ID_ATI_RADEON_ES1000) {
        /* the AIX driver only accepts the subsystem ID of ATI's board */
        pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID, PCI_VENDOR_ID_ATI);
        pci_set_word(dev->config + PCI_SUBSYSTEM_ID, s->dev_id);
    }

    if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF &&
        s->vga.vram_size_mb < 16) {
        warn_report("Too small video memory for device id");
        s->vga.vram_size_mb = 16;
    }

    /* init vga bits */
    if (!vga_common_init(vga, OBJECT(s), errp)) {
        return;
    }
    vga_init(vga, OBJECT(s), pci_address_space(dev),
             pci_address_space_io(dev), true);
    vga->con = qemu_graphic_console_create(DEVICE(s), 0, s->vga.hw_ops, vga);
    vga->cursor_state = ati_cursor_state;
    if (s->cursor_guest_mode) {
        vga->cursor_invalidate = ati_cursor_invalidate;
        vga->cursor_draw_line = ati_cursor_draw_line;
    }

    /* ddc, edid */
    i2cbus = i2c_init_bus(DEVICE(s), "ati-vga.ddc");
    bitbang_i2c_init(&s->bbi2c, i2cbus);
    i2c_slave_set_address(I2C_SLAVE(&s->i2cddc), 0x50);
    qdev_realize(DEVICE(&s->i2cddc), BUS(i2cbus), &error_abort);

    /* mmio register space */
    memory_region_init_io(&s->mm, OBJECT(s), &ati_mm_ops, s, "ati.mmregs",
                          s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF ?
                          ATI_RAGE128_MMIO_SIZE : ATI_R100_MMIO_SIZE);
    s->mmregs = g_new0(uint32_t, memory_region_size(&s->mm) / 4);
    ati_cp_init(s);
    /* io space is alias to beginning of mmregs */
    memory_region_init_alias(&s->io, OBJECT(s), "ati.io", &s->mm, 0, 0x100);

    /*
     * The framebuffer is at the beginning of the linear aperture. For
     * Rage128 the upper half of the aperture is reserved for an AGP
     * window (which we do not emulate.)
     */
    if (!s->linear_aper_sz) {
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->linear_aper_sz = ATI_RAGE128_LINEAR_APER_SIZE;
        } else {
            s->linear_aper_sz = ATI_R100_LINEAR_APER_SIZE;
        }
    }
    if (s->linear_aper_sz > 256 * MiB) {
        error_setg(errp, "x-linear-aper-size is too large (maximum 256 MiB)");
        return;
    }
    if (s->linear_aper_sz < 16 * MiB) {
        error_setg(errp, "x-linear-aper-size is too small (minimum 16 MiB)");
        return;
    }
    memory_region_init(&s->linear_aper, OBJECT(dev), "ati-linear-aperture0",
                       s->linear_aper_sz);
    memory_region_add_subregion(&s->linear_aper, 0, &vga->vram);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->linear_aper);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mm);

    /*
     * The AGP capability of an AGP card: version 2.0, 1x, 2x and 4x, side
     * band addressing, a request queue of 48. There is no AGP transfer
     * mode behind it: the card still does PCI cycles, as it does in a slot
     * where the host has no GART.
     */
    if (s->agp) {
        int cap = pci_add_capability(dev, PCI_CAP_ID_AGP, 0x58,
                                     PCI_AGP_SIZEOF, errp);
        if (cap < 0) {
            return;
        }
        pci_set_byte(dev->config + cap + PCI_AGP_VERSION, 0x20);
        pci_set_long(dev->config + cap + PCI_AGP_STATUS,
                     (47u << 24) | PCI_AGP_STATUS_SBA | PCI_AGP_STATUS_RATE4 |
                     PCI_AGP_STATUS_RATE2 | PCI_AGP_STATUS_RATE1);
        pci_set_long(dev->wmask + cap + PCI_AGP_COMMAND,
                     PCI_AGP_COMMAND_RQ_MASK | PCI_AGP_COMMAND_SBA |
                     PCI_AGP_COMMAND_AGP | PCI_AGP_COMMAND_64BIT |
                     PCI_AGP_COMMAND_FW | PCI_AGP_COMMAND_RATE4 |
                     PCI_AGP_COMMAND_RATE2 | PCI_AGP_COMMAND_RATE1);
    }

    /* most interrupts are not yet emulated but MacOS needs at least VBlank */
    dev->config[PCI_INTERRUPT_PIN] = 1;
    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL, ati_vga_vblank_irq, s);
}

static void ati_vga_reset(DeviceState *dev)
{
    ATIVGAState *s = ATI_VGA(dev);

    if (ati_is_r100_r200(s)) {
        /* the frame timer runs all the time, see ati_vga_vblank_irq() */
        timer_mod(&s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND / 60);
        s->regs.gen_int_cntl = 0;
        s->regs.gen_int_status = 0;
    } else {
        timer_del(&s->vblank_timer);
    }
    ati_vga_update_irq(s);

    /* reset vga */
    vga_common_reset(&s->vga);
    s->mode = VGA_MODE;

    s->host_data.active = false;
    s->host_data.next = 0;
    s->host_data.row = 0;
    s->host_data.col = 0;
    s->regs.rbbm_guicntl = 0;
    s->regs.config_cntl = 0;
    s->regs.surface_cntl = 0;
    ati_cp_reset(s);
    memset(&s->tcl, 0, sizeof(s->tcl));
    s->vis_pin = false;
    /*
     * PLL registers (CLOCK_CNTL_INDEX/DATA): only stored. The dividers are
     * a 27 MHz reference / 12, as on many Radeons, so drivers that work
     * out the clocks from them don't divide by zero.
     */
    s->regs.clock_cntl_index = 0;
    memset(s->regs.pll, 0, sizeof(s->regs.pll));
    s->regs.pll[PPLL_REF_DIV] = 12;
    s->regs.pll[PPLL_DIV_3] = 0x00030060;
    s->regs.pll[P2PLL_REF_DIV] = 12;
    s->regs.pll[X_MPLL_REF_FB_DIV] = 0x0058580c;
}

static void ati_vga_exit(PCIDevice *dev)
{
    ATIVGAState *s = ATI_VGA(dev);

    timer_del(&s->vblank_timer);
    qemu_graphic_console_close(s->vga.con);
    ati_cp_exit(s);
    g_free(s->mmregs);
}

static const Property ati_vga_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", ATIVGAState, vga.vram_size_mb, 16),
    DEFINE_PROP_STRING("model", ATIVGAState, model),
    DEFINE_PROP_UINT16("x-device-id", ATIVGAState, dev_id,
                       PCI_DEVICE_ID_ATI_RAGE128_PF),
    DEFINE_PROP_BOOL("guest_hwcursor", ATIVGAState, cursor_guest_mode, false),
    DEFINE_PROP_BOOL("agp", ATIVGAState, agp, false),
    /* this is a debug option, prefer PROP_UINT over PROP_BIT for simplicity */
    DEFINE_PROP_UINT8("x-pixman", ATIVGAState, use_pixman, DEFAULT_X_PIXMAN),
    DEFINE_PROP_UINT64("x-linear-aper-size", ATIVGAState, linear_aper_sz, 0),
    DEFINE_EDID_PROPERTIES(ATIVGAState, i2cddc.edid_info),
};

static void ati_vga_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ati_vga_reset);
    device_class_set_props(dc, ati_vga_properties);
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->vendor_id = PCI_VENDOR_ID_ATI;
    k->device_id = PCI_DEVICE_ID_ATI_RAGE128_PF;
    k->romfile = "vgabios-ati.bin";
    k->realize = ati_vga_realize;
    k->exit = ati_vga_exit;
}

static void ati_vga_init(Object *o)
{
    ATIVGAState *s = ATI_VGA(o);

    object_initialize_child(o, "edid", &s->i2cddc, TYPE_I2CDDC);
    object_property_set_description(o, "x-pixman", "Use pixman for: "
                                    "1: fill, 2: blit");
}

static const TypeInfo ati_vga_info = {
    .name = TYPE_ATI_VGA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ATIVGAState),
    .class_init = ati_vga_class_init,
    .instance_init = ati_vga_init,
    .interfaces = (const InterfaceInfo[]) {
          { INTERFACE_CONVENTIONAL_PCI_DEVICE },
          { },
    },
};

static void ati_vga_register_types(void)
{
    type_register_static(&ati_vga_info);
}

type_init(ati_vga_register_types)
