/*
 * QEMU ATI SVGA emulation
 * Radeon command processor (CP/CCE), PCI GART and GPU address space
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

/*
 * The CP reads command packets from a ring buffer, from indirect buffers or
 * from writes to the CSQ aperture (PIO mode) and executes them. Packets of
 * type 0 and 1 write registers, type 2 is a filler and type 3 carries an
 * opcode. The 2D type 3 packets are turned into the register writes the
 * MMIO path already knows, so they share the 2D engine. 3D packets are
 * skipped: there is no 3D engine yet.
 *
 * The CP sees the GPU address space: the frame buffer at MC_FB_LOCATION,
 * the AGP aperture at MC_AGP_LOCATION (bus address AGP_BASE) and the PCI
 * GART between AIC_LO_ADDR and AIC_HI_ADDR, whose page table is in system
 * memory at AIC_PT_BASE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "trace.h"

#define CP_PKT_TYPE(h)          (((h) >> 30) & 3)
#define CP_PKT_COUNT(h)         (((h) >> 16) & 0x3fff)
#define CP_PKT0_REG(h)          (((h) & 0x1fff) << 2)
#define CP_PKT0_ONE_REG_WR      BIT(15)
#define CP_PKT1_REG0(h)         (((h) & 0x7ff) << 2)
#define CP_PKT1_REG1(h)         ((((h) >> 11) & 0x7ff) << 2)
#define CP_PKT3_OPCODE(h)       (((h) >> 8) & 0xff)

/* type 3 opcodes */
#define CP3_NOP                 0x10
#define CP3_NEXT_CHAR           0x19
#define CP3_PLY_NEXTSCAN        0x1d
#define CP3_SET_SCISSORS        0x1e
#define CP3_LOAD_MICROCODE      0x24
#define CP3_WAIT_FOR_IDLE       0x26
#define CP3_LOAD_PALETTE        0x2c
#define CP3_CNTL_PAINT          0x91
#define CP3_CNTL_BITBLT         0x92
#define CP3_CNTL_SMALLTEXT      0x93
#define CP3_CNTL_HOSTDATA_BLT   0x94
#define CP3_CNTL_POLYLINE       0x95
#define CP3_CNTL_POLYSCANLINES  0x98
#define CP3_CNTL_PAINT_MULTI    0x9a
#define CP3_CNTL_BITBLT_MULTI   0x9b
#define CP3_CNTL_TRANS_BITBLT   0x9c

/* DP_GUI_MASTER_CNTL fields that select optional packet data */
#define GMC_SRC_PITCH_OFFSET    BIT(0)
#define GMC_DST_PITCH_OFFSET    BIT(1)
#define GMC_SRC_CLIP            BIT(2)
#define GMC_DST_CLIP            BIT(3)
#define GMC_BRUSH(g)            (((g) >> 4) & 0xf)
#define GMC_SRC_DT(g)           (((g) >> 12) & 3)

#define CSQ_MODE(c)             (((c) >> 28) & 0xf)
#define RB_BUFSZ(c)             ((c) & 0x3f)
#define RB_SWAP(c)              (((c) >> 16) & 3)
#define RB_NO_UPDATE            BIT(27)
#define RB_RPTR_WR_ENA          BIT(31)

#define AIC_GART_EN             BIT(0)
#define ATI_GPU_PAGE            4096

static bool ati_cp_is_radeon(ATIVGAState *s)
{
    return ati_is_r100_r200(s);
}

/* GPU address space */

static bool ati_range(uint32_t loc, uint32_t addr, uint32_t *start)
{
    uint32_t lo = (loc & 0xffff) << 16;
    uint32_t hi = (loc & 0xffff0000) | 0xffff;

    if (lo > hi || addr < lo || addr > hi) {
        return false;
    }
    *start = lo;
    return true;
}

/*
 * The video BIOS puts the frame buffer where BAR 0 is in the GPU address
 * space. Without a BIOS, or until a driver sets it, we do the same.
 */
static uint32_t ati_mc_fb_location(ATIVGAState *s)
{
    pcibus_t bar;

    if (s->cp.mc_fb_location_set) {
        return s->cp.mc_fb_location;
    }
    bar = pci_get_bar_addr(&s->dev, 0);
    if (bar == PCI_BAR_UNMAPPED) {
        bar = 0;
    }
    return (bar >> 16) | ((bar + s->vga.vram_size - 1) & 0xffff0000);
}

/*
 * The memory controller ignores the address bits above the size of the
 * video memory, so a frame buffer range larger than the memory wraps around
 * (MorphOS sets the range from 0 and uses the BAR address as base).
 */
static uint32_t ati_vram_wrap(ATIVGAState *s, uint32_t offset)
{
    return offset & (pow2ceil(s->vga.vram_size) - 1);
}

uint32_t ati_fb_offset(ATIVGAState *s, uint32_t gpu_addr)
{
    uint32_t start;

    if (ati_cp_is_radeon(s) &&
        ati_range(ati_mc_fb_location(s), gpu_addr, &start)) {
        return ati_vram_wrap(s, gpu_addr - start);
    }
    return gpu_addr;
}

bool ati_gpu_vram_offset(ATIVGAState *s, uint32_t gpu_addr, uint32_t *off)
{
    uint32_t start;

    if (!ati_range(ati_mc_fb_location(s), gpu_addr, &start)) {
        return false;
    }
    *off = ati_vram_wrap(s, gpu_addr - start);
    return true;
}

/*
 * Translate one GPU address to either a VRAM offset or a bus address.
 * Returns the number of bytes up to the end of the page, 0 if unmapped.
 */
static uint32_t ati_gpu_translate(ATIVGAState *s, uint32_t addr,
                                  bool *is_vram, dma_addr_t *out)
{
    uint32_t start;
    uint32_t left = ATI_GPU_PAGE - (addr & (ATI_GPU_PAGE - 1));

    if (ati_range(ati_mc_fb_location(s), addr, &start)) {
        *is_vram = true;
        *out = ati_vram_wrap(s, addr - start);
        if (*out >= s->vga.vram_size) {
            return 0;
        }
        return left;
    }
    if ((s->cp.aic_cntl & AIC_GART_EN) &&
        addr >= s->cp.aic_lo_addr && addr <= s->cp.aic_hi_addr) {
        uint32_t idx = (addr - s->cp.aic_lo_addr) / ATI_GPU_PAGE;
        uint32_t ent = 0;

        pci_dma_read(&s->dev, s->cp.aic_pt_base + idx * 4, &ent, 4);
        ent = le32_to_cpu(ent);
        *is_vram = false;
        *out = (ent & ~(ATI_GPU_PAGE - 1)) | (addr & (ATI_GPU_PAGE - 1));
        return left;
    }
    if (ati_range(s->cp.mc_agp_location, addr, &start)) {
        *is_vram = false;
        *out = s->cp.agp_base + (addr - start);
        return left;
    }
    return 0;
}

bool ati_gpu_rw(ATIVGAState *s, uint32_t addr, void *buf, uint32_t len,
                bool write)
{
    uint8_t *p = buf;

    while (len) {
        bool vram;
        dma_addr_t a;
        uint32_t n = ati_gpu_translate(s, addr, &vram, &a);

        if (!n) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ati_cp: GPU address 0x%08x is not mapped\n", addr);
            if (!write) {
                memset(p, 0, len);
            }
            return false;
        }
        n = MIN(n, len);
        if (vram) {
            n = MIN(n, s->vga.vram_size - a);
            if (write) {
                memcpy(s->vga.vram_ptr + a, p, n);
                memory_region_set_dirty(&s->vga.vram, a, n);
            } else {
                memcpy(p, s->vga.vram_ptr + a, n);
            }
        } else {
            pci_dma_rw(&s->dev, a, p, n,
                       write ? DMA_DIRECTION_FROM_DEVICE :
                               DMA_DIRECTION_TO_DEVICE,
                       MEMTXATTRS_UNSPECIFIED);
        }
        p += n;
        addr += n;
        len -= n;
    }
    return true;
}

static void ati_gpu_write32(ATIVGAState *s, uint32_t addr, uint32_t val)
{
    uint32_t le = cpu_to_le32(val);

    ati_gpu_rw(s, addr, &le, 4, true);
}

static uint32_t ati_cp_swap(uint32_t v, int swap)
{
    switch (swap) {
    case 1:
        return ((v & 0x00ff00ff) << 8) | ((v >> 8) & 0x00ff00ff);
    case 2:
        return bswap32(v);
    case 3:
        return ror32(v, 16);
    default:
        return v;
    }
}

/* Packet execution */

static void ati_cp_reg(ATIVGAState *s, uint32_t reg, uint32_t val)
{
    bool in_reg = s->cp.in_reg;

    /* saved and restored, so that a reset of the CP here cannot upset it */
    s->cp.in_reg = true;
    ati_mm_write(s, reg, val, 4);
    s->cp.in_reg = in_reg;
}

/* Skips the brush data of a 2D packet; returns false if out of data */
static bool ati_cp_brush(ATIVGAState *s, uint32_t gmc, const uint32_t *d,
                         uint32_t n, uint32_t *i)
{
    uint32_t k;

    switch (GMC_BRUSH(gmc)) {
    case 0: /* 8x8 mono, with and without background */
    case 1:
        if (*i + 4 > n) {
            return false;
        }
        ati_cp_reg(s, DP_BRUSH_BKGD_CLR, d[(*i)++]);
        ati_cp_reg(s, DP_BRUSH_FRGD_CLR, d[(*i)++]);
        ati_cp_reg(s, BRUSH_DATA0, d[(*i)++]);
        ati_cp_reg(s, BRUSH_DATA0 + 4, d[(*i)++]);
        break;
    case 2: /* 8x1 and 32x1 mono */
    case 3:
    case 6:
    case 7:
        if (*i + 3 > n) {
            return false;
        }
        ati_cp_reg(s, DP_BRUSH_BKGD_CLR, d[(*i)++]);
        ati_cp_reg(s, DP_BRUSH_FRGD_CLR, d[(*i)++]);
        ati_cp_reg(s, BRUSH_DATA0, d[(*i)++]);
        break;
    case 8: /* 32x32 mono */
    case 9:
        if (*i + 34 > n) {
            return false;
        }
        ati_cp_reg(s, DP_BRUSH_BKGD_CLR, d[(*i)++]);
        ati_cp_reg(s, DP_BRUSH_FRGD_CLR, d[(*i)++]);
        for (k = 0; k < 32; k++) {
            ati_cp_reg(s, BRUSH_DATA0 + k * 4, d[(*i)++]);
        }
        break;
    case 13: /* solid colour */
    case 14:
        if (*i + 1 > n) {
            return false;
        }
        ati_cp_reg(s, DP_BRUSH_FRGD_CLR, d[(*i)++]);
        break;
    case 15: /* none */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ati_cp: brush type %d in packet\n",
                      GMC_BRUSH(gmc));
        return false;
    }
    return true;
}

static void ati_cp_2d(ATIVGAState *s, uint8_t op, const uint32_t *d,
                      uint32_t n)
{
    uint32_t gmc, i = 0;

    if (op & 0x80) {
        if (n < 1) {
            return;
        }
        gmc = d[i++];
        ati_cp_reg(s, DP_GUI_MASTER_CNTL, gmc);
        if (gmc & GMC_SRC_PITCH_OFFSET && i < n) {
            ati_cp_reg(s, SRC_PITCH_OFFSET, d[i++]);
        }
        if (gmc & GMC_DST_PITCH_OFFSET && i < n) {
            ati_cp_reg(s, DST_PITCH_OFFSET, d[i++]);
        }
        if (gmc & GMC_SRC_CLIP && i < n) {
            ati_cp_reg(s, SRC_SC_BOTTOM_RIGHT, d[i++]);
        }
        if (gmc & GMC_DST_CLIP && i + 1 < n) {
            ati_cp_reg(s, SC_TOP_LEFT, d[i++]);
            ati_cp_reg(s, SC_BOTTOM_RIGHT, d[i++]);
        }
        if (!ati_cp_brush(s, gmc, d, n, &i)) {
            return;
        }
    } else {
        /*
         * The short forms (0x11 to 0x1c) leave out DP_GUI_MASTER_CNTL and
         * what it chooses: they draw with the current one. AmigaOS 4.1
         * draws its checkmarks with HOSTDATA_BLT (0x14).
         */
        gmc = (s->regs.dp_datatype & DP_SRC_DATATYPE) >> 4;
        op |= 0x80;
    }
    /* mono sources take their colours from the packet */
    if (op == CP3_CNTL_HOSTDATA_BLT || GMC_SRC_DT(gmc) < 2) {
        if (i + 2 > n) {
            return;
        }
        ati_cp_reg(s, DP_SRC_FRGD_CLR, d[i++]);
        ati_cp_reg(s, DP_SRC_BKGD_CLR, d[i++]);
    }

    switch (op) {
    case CP3_CNTL_PAINT:
    case CP3_CNTL_PAINT_MULTI:
        while (i + 2 <= n) {
            ati_cp_reg(s, DST_X_Y, d[i++]);
            ati_cp_reg(s, DST_WIDTH_HEIGHT, d[i++]);
        }
        break;
    case CP3_CNTL_BITBLT:
    case CP3_CNTL_BITBLT_MULTI:
        while (i + 3 <= n) {
            ati_cp_reg(s, SRC_X_Y, d[i++]);
            ati_cp_reg(s, DST_X_Y, d[i++]);
            ati_cp_reg(s, DST_WIDTH_HEIGHT, d[i++]);
        }
        break;
    case CP3_CNTL_HOSTDATA_BLT:
    {
        uint32_t count;

        if (i + 3 > n) {
            return;
        }
        ati_cp_reg(s, DST_Y_X, d[i++]);
        ati_cp_reg(s, DST_HEIGHT_WIDTH, d[i++]);
        count = MIN(d[i++], n - i);
        while (count--) {
            ati_cp_reg(s, count ? HOST_DATA0 : HOST_DATA_LAST, d[i++]);
        }
        break;
    }
    case CP3_CNTL_POLYLINE:
        if (i + 1 > n) {
            return;
        }
        ati_cp_reg(s, DST_LINE_START, d[i++]);
        while (i < n) {
            ati_cp_reg(s, DST_LINE_END, d[i]);
            ati_cp_reg(s, DST_LINE_START, d[i++]);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ati_cp: 2D packet 0x%02x\n", op);
        break;
    }
}

static void ati_cp_type3(ATIVGAState *s, uint32_t hdr, const uint32_t *d,
                         uint32_t n)
{
    uint8_t op = CP_PKT3_OPCODE(hdr);

    trace_ati_cp_packet3(op, n);
    switch (op) {
    case CP3_NOP:
    case CP3_WAIT_FOR_IDLE:
    case CP3_LOAD_MICROCODE:
    case CP3_SET_SCISSORS:
    case CP3_NEXT_CHAR:
    case CP3_PLY_NEXTSCAN:
        break;
    case CP3_CNTL_PAINT:
    case CP3_CNTL_PAINT_MULTI:
    case CP3_CNTL_BITBLT:
    case CP3_CNTL_BITBLT_MULTI:
    case CP3_CNTL_HOSTDATA_BLT:
    case CP3_CNTL_POLYLINE:
    case CP3_CNTL_SMALLTEXT:
    case CP3_CNTL_POLYSCANLINES:
    case CP3_CNTL_TRANS_BITBLT:
    /* and their short forms, without the DP_GUI_MASTER_CNTL dword */
    case CP3_CNTL_PAINT & 0x7f:
    case CP3_CNTL_BITBLT & 0x7f:
    case CP3_CNTL_SMALLTEXT & 0x7f:
    case CP3_CNTL_HOSTDATA_BLT & 0x7f:
    case CP3_CNTL_POLYLINE & 0x7f:
    case CP3_CNTL_POLYSCANLINES & 0x7f:
    case CP3_CNTL_PAINT_MULTI & 0x7f:
    case CP3_CNTL_BITBLT_MULTI & 0x7f:
    case CP3_CNTL_TRANS_BITBLT & 0x7f:
        ati_cp_2d(s, op, d, n);
        break;
    case 0x23:
    case 0x25:
    case 0x27 ... 0x2b:
    case 0x2d ... 0x3f:
        ati_3d_packet(s, op, d, n);
        break;
    case CP3_LOAD_PALETTE:
    default:
        qemu_log_mask(LOG_UNIMP, "ati_cp: packet 0x%02x:", op);
        for (uint32_t j = 0; j < n; j++) {
            qemu_log_mask(LOG_UNIMP, " %08x", d[j]);
        }
        qemu_log_mask(LOG_UNIMP, "\n");
        break;
    }
}

static void ati_cp_parser_reset(ATICPParser *p)
{
    p->left = 0;
    p->pos = 0;
}

/* Feeds one dword to a packet parser */
static void ati_cp_feed(ATIVGAState *s, ATICPParser *p, uint32_t dw)
{
    if (!p->left) {
        p->hdr = dw;
        p->pos = 0;
        switch (CP_PKT_TYPE(dw)) {
        case 0:
            p->reg = CP_PKT0_REG(dw);
            p->left = CP_PKT_COUNT(dw) + 1;
            break;
        case 1:
            p->left = 2;
            break;
        case 2:
            break;
        case 3:
            p->left = CP_PKT_COUNT(dw) + 1;
            break;
        }
        return;
    }
    p->left--;
    switch (CP_PKT_TYPE(p->hdr)) {
    case 0:
        ati_cp_reg(s, p->reg, dw);
        if (!(p->hdr & CP_PKT0_ONE_REG_WR)) {
            p->reg += 4;
        }
        break;
    case 1:
        ati_cp_reg(s, p->pos++ ? CP_PKT1_REG1(p->hdr) :
                   CP_PKT1_REG0(p->hdr), dw);
        break;
    case 3:
        p->buf[p->pos++] = dw;
        if (!p->left) {
            ati_cp_type3(s, p->hdr, p->buf, p->pos);
        }
        break;
    }
}

static void ati_cp_run_ib(ATIVGAState *s)
{
    uint32_t i, dw;
    uint32_t base = s->cp.ib_base & ~3;
    uint32_t size = s->cp.ib_bufsz & 0x7fffff;

    if (s->cp.depth) {
        qemu_log_mask(LOG_GUEST_ERROR, "ati_cp: nested indirect buffer\n");
        return;
    }
    trace_ati_cp_ib(base, size);
    s->cp.depth++;
    ati_cp_parser_reset(&s->cp.ib);
    for (i = 0; i < size; i++) {
        if (!ati_gpu_rw(s, base + i * 4, &dw, 4, false)) {
            break;
        }
        ati_cp_feed(s, &s->cp.ib, ati_cp_swap(le32_to_cpu(dw),
                                               RB_SWAP(s->cp.rb_cntl)));
    }
    s->cp.depth--;
}

static uint32_t ati_cp_ring_dwords(ATIVGAState *s)
{
    return 2u << RB_BUFSZ(s->cp.rb_cntl);
}

static void ati_cp_writeback_rptr(ATIVGAState *s)
{
    if (!(s->cp.rb_cntl & RB_NO_UPDATE) && s->cp.rb_rptr_addr & ~3) {
        ati_gpu_write32(s, s->cp.rb_rptr_addr & ~3, s->cp.rb_rptr);
    }
}

static void ati_cp_run_ring(ATIVGAState *s)
{
    uint32_t mask = ati_cp_ring_dwords(s) - 1;
    uint32_t dw, n = 0;

    if (s->cp.running) {
        return;
    }
    s->cp.running = true;
    s->cp.rb_wptr &= mask;
    trace_ati_cp_ring(s->cp.rb_rptr, s->cp.rb_wptr);
    while (s->cp.rb_rptr != s->cp.rb_wptr && n++ <= mask) {
        if (!ati_gpu_rw(s, s->cp.rb_base + s->cp.rb_rptr * 4, &dw, 4, false)) {
            s->cp.rb_rptr = s->cp.rb_wptr;
            break;
        }
        s->cp.rb_rptr = (s->cp.rb_rptr + 1) & mask;
        ati_cp_feed(s, &s->cp.ring, ati_cp_swap(le32_to_cpu(dw),
                                                 RB_SWAP(s->cp.rb_cntl)));
    }
    ati_cp_writeback_rptr(s);
    s->cp.running = false;
}

static void ati_cp_scratch(ATIVGAState *s, int i, uint32_t val)
{
    s->cp.scratch[i] = val;
    if (s->cp.scratch_umsk & BIT(i)) {
        ati_gpu_write32(s, (s->cp.scratch_addr & ~0x1f) + i * 4, val);
    }
}

/* Register interface, returns true if the register was handled here */

bool ati_cp_mm_read(ATIVGAState *s, hwaddr addr, unsigned size,
                    uint32_t *val)
{
    if (!ati_cp_is_radeon(s)) {
        return false;
    }
    switch (addr) {
    case RADEON_MC_FB_LOCATION:
        *val = ati_mc_fb_location(s);
        break;
    case RADEON_MC_AGP_LOCATION:
        *val = s->cp.mc_agp_location;
        break;
    case RADEON_AGP_BASE:
        *val = s->cp.agp_base;
        break;
    case RADEON_AIC_CNTL:
        *val = s->cp.aic_cntl;
        break;
    case RADEON_AIC_STAT:
        *val = 0;
        break;
    case RADEON_AIC_PT_BASE:
        *val = s->cp.aic_pt_base;
        break;
    case RADEON_AIC_LO_ADDR:
        *val = s->cp.aic_lo_addr;
        break;
    case RADEON_AIC_HI_ADDR:
        *val = s->cp.aic_hi_addr;
        break;
    case RADEON_CP_RB_BASE:
        *val = s->cp.rb_base;
        break;
    case RADEON_CP_RB_CNTL:
        *val = s->cp.rb_cntl;
        break;
    case RADEON_CP_RB_RPTR_ADDR:
        *val = s->cp.rb_rptr_addr;
        break;
    case RADEON_CP_RB_RPTR:
        *val = s->cp.rb_rptr;
        break;
    case RADEON_CP_RB_WPTR:
        *val = s->cp.rb_wptr;
        break;
    case RADEON_CP_IB_BASE:
        *val = s->cp.ib_base;
        break;
    case RADEON_CP_IB_BUFSZ:
        *val = s->cp.ib_bufsz;
        break;
    case RADEON_CP_CSQ_CNTL:
        /*
         * The low byte counts the free entries of the primary queue. We run
         * every packet as it comes, so the queue (as big as the 128 dword
         * PIO aperture) is always empty.
         */
        *val = (s->cp.csq_cntl & ~0xff) | 0x80;
        break;
    case RADEON_CP_CSQ_MODE:
        *val = s->cp.csq_mode;
        break;
    case RADEON_SCRATCH_UMSK:
        *val = s->cp.scratch_umsk;
        break;
    case RADEON_SCRATCH_ADDR:
        *val = s->cp.scratch_addr;
        break;
    case RADEON_SCRATCH_REG0 ... RADEON_SCRATCH_REG7:
        *val = s->cp.scratch[(addr - RADEON_SCRATCH_REG0) / 4];
        break;
    case RADEON_CP_STAT:
    case RADEON_CP_CSQ_STAT:
    case RADEON_CP_CSQ2_STAT:
        *val = 0; /* everything is done as soon as it is written */
        break;
    case RADEON_CP_ME_CNTL:
        *val = s->cp.me_cntl;
        break;
    case RADEON_CP_ME_RAM_ADDR:
        *val = s->cp.me_ram_addr;
        break;
    case RADEON_CP_ME_RAM_DATAH:
        *val = s->cp.me_ram[(s->cp.me_ram_raddr & 0xff) * 2];
        break;
    case RADEON_CP_ME_RAM_DATAL:
        *val = s->cp.me_ram[(s->cp.me_ram_raddr & 0xff) * 2 + 1];
        s->cp.me_ram_raddr++;
        break;
    case RADEON_RB2D_DSTCACHE_CTLSTAT:
    case RADEON_RB3D_DSTCACHE_CTLSTAT:
        *val = 0; /* never busy */
        break;
    default:
        return false;
    }
    if (size < 4) {
        *val = extract32(*val, (addr & 3) * 8, size * 8);
    }
    return true;
}

bool ati_cp_mm_write(ATIVGAState *s, hwaddr addr, uint32_t data,
                     unsigned size)
{
    if (!ati_cp_is_radeon(s)) {
        return false;
    }
    if ((addr >= RADEON_CP_CSQ_APER_PRIMARY &&
         addr < RADEON_CP_CSQ_APER_PRIMARY + 0x200) ||
        (addr >= RADEON_CP_CSQ_APER_INDIRECT &&
         addr < RADEON_CP_CSQ_APER_INDIRECT + 0x100)) {
        /*
         * A packet that writes the CSQ apertures would feed the CP from
         * inside the CP, as deep as the guest likes: drop it.
         */
        if (s->cp.in_reg) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ati_cp: packet writes the CSQ aperture 0x%x\n",
                          (unsigned)addr);
            return true;
        }
        /* PIO mode: the packets are written straight to the CSQ */
        ati_cp_feed(s, addr < RADEON_CP_CSQ_APER_INDIRECT ?
                    &s->cp.ring : &s->cp.ib, data);
        return true;
    }
    switch (addr) {
    case RADEON_MC_FB_LOCATION:
        s->cp.mc_fb_location = data;
        s->cp.mc_fb_location_set = true;
        break;
    case RADEON_MC_AGP_LOCATION:
        s->cp.mc_agp_location = data;
        break;
    case RADEON_AGP_BASE:
        s->cp.agp_base = data;
        break;
    case RADEON_AIC_CNTL:
        s->cp.aic_cntl = data;
        break;
    case RADEON_AIC_STAT:
    case RADEON_AIC_TLB_ADDR:
    case RADEON_AIC_TLB_DATA:
        break;
    case RADEON_AIC_PT_BASE:
        s->cp.aic_pt_base = data & ~(ATI_GPU_PAGE - 1);
        break;
    case RADEON_AIC_LO_ADDR:
        s->cp.aic_lo_addr = data & ~(ATI_GPU_PAGE - 1);
        break;
    case RADEON_AIC_HI_ADDR:
        s->cp.aic_hi_addr = data;
        break;
    case RADEON_CP_RB_BASE:
        s->cp.rb_base = data & ~3;
        break;
    case RADEON_CP_RB_CNTL:
        s->cp.rb_cntl = data;
        break;
    case RADEON_CP_RB_RPTR_ADDR:
        s->cp.rb_rptr_addr = data;
        break;
    case RADEON_CP_RB_RPTR:
        if (s->cp.rb_cntl & RB_RPTR_WR_ENA) {
            s->cp.rb_rptr = data & (ati_cp_ring_dwords(s) - 1);
        }
        break;
    case RADEON_CP_RB_WPTR:
        s->cp.rb_wptr = data;
        if (CSQ_MODE(s->cp.csq_cntl) == 2 || CSQ_MODE(s->cp.csq_cntl) == 4) {
            ati_cp_run_ring(s);
        }
        break;
    case RADEON_CP_IB_BASE:
        s->cp.ib_base = data;
        break;
    case RADEON_CP_IB_BUFSZ:
        s->cp.ib_bufsz = data;
        ati_cp_run_ib(s);
        break;
    case RADEON_CP_CSQ_CNTL:
        s->cp.csq_cntl = data;
        if (CSQ_MODE(data) == 0) {
            ati_cp_parser_reset(&s->cp.ring);
        }
        break;
    case RADEON_CP_CSQ_MODE:
        s->cp.csq_mode = data;
        break;
    case RADEON_SCRATCH_UMSK:
        s->cp.scratch_umsk = data & 0xff;
        break;
    case RADEON_SCRATCH_ADDR:
        s->cp.scratch_addr = data;
        break;
    case RADEON_SCRATCH_REG0 ... RADEON_SCRATCH_REG7:
        ati_cp_scratch(s, (addr - RADEON_SCRATCH_REG0) / 4, data);
        break;
    case RADEON_CP_ME_CNTL:
        s->cp.me_cntl = data;
        break;
    case RADEON_CP_ME_RAM_ADDR:
        s->cp.me_ram_addr = data;
        break;
    case RADEON_CP_ME_RAM_RADDR:
        s->cp.me_ram_raddr = data;
        break;
    case RADEON_CP_ME_RAM_DATAH:
        s->cp.me_ram[(s->cp.me_ram_addr & 0xff) * 2] = data;
        break;
    case RADEON_CP_ME_RAM_DATAL:
        s->cp.me_ram[(s->cp.me_ram_addr & 0xff) * 2 + 1] = data;
        s->cp.me_ram_addr++;
        break;
    case RADEON_RBBM_SOFT_RESET:
        if (data & RADEON_SOFT_RESET_CP) {
            s->cp.rb_rptr = 0;
            s->cp.rb_wptr = 0;
            ati_cp_parser_reset(&s->cp.ring);
            ati_cp_parser_reset(&s->cp.ib);
        }
        s->cp.soft_reset = data;
        break;
    case WAIT_UNTIL:
    case RADEON_ISYNC_CNTL:
    case RADEON_RB2D_DSTCACHE_CTLSTAT:
    case RADEON_RB3D_DSTCACHE_CTLSTAT:
        break; /* the engine is always idle and coherent */
    default:
        return false;
    }
    return true;
}

void ati_cp_reset(ATIVGAState *s)
{
    uint32_t *ring_buf = s->cp.ring.buf;
    uint32_t *ib_buf = s->cp.ib.buf;

    memset(&s->cp, 0, sizeof(s->cp));
    s->cp.ring.buf = ring_buf;
    s->cp.ib.buf = ib_buf;
    /* no AGP aperture (start above end) */
    s->cp.mc_agp_location = 0x0000ffff;
}

void ati_cp_init(ATIVGAState *s)
{
    s->cp.ring.buf = g_new0(uint32_t, 0x4000);
    s->cp.ib.buf = g_new0(uint32_t, 0x4000);
}

void ati_cp_exit(ATIVGAState *s)
{
    g_free(s->cp.ring.buf);
    g_free(s->cp.ib.buf);
}
