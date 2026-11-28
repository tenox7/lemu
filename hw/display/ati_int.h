/*
 * QEMU ATI SVGA emulation
 *
 * Copyright (c) 2019 BALATON Zoltan
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef ATI_INT_H
#define ATI_INT_H

#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/i2c-ddc.h"
#include "vga_int.h"
#include "qom/object.h"

/*#define DEBUG_ATI*/

#ifdef DEBUG_ATI
#define DPRINTF(fmt, ...) printf("%s: " fmt, __func__, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

#define PCI_VENDOR_ID_ATI 0x1002
/* Rage128 Pro GL */
#define PCI_DEVICE_ID_ATI_RAGE128_PF 0x5046
/* Radeon RV100 (VE) */
#define PCI_DEVICE_ID_ATI_RADEON_QY 0x5159
/* Radeon 7500 (RV200), emulated as the RV100 */
#define PCI_DEVICE_ID_ATI_RADEON_QW 0x5157
/* Radeon ES1000 (RN50), a server RV100 with one CRTC */
#define PCI_DEVICE_ID_ATI_RADEON_ES1000 0x515e
/* Mobility Radeon 9000 (M9), an RV250, on the Sam440ep */
#define PCI_DEVICE_ID_ATI_RADEON_LF 0x4c66
/* Radeon 9250, an RV280, the AGP card of the Pegasos II */
#define PCI_DEVICE_ID_ATI_RADEON_9250 0x5960

#define ATI_RAGE128_LINEAR_APER_SIZE (64 * MiB)
#define ATI_R100_LINEAR_APER_SIZE (128 * MiB)
#define ATI_RAGE128_MMIO_SIZE (16 * KiB)
#define ATI_R100_MMIO_SIZE (64 * KiB)
#define ATI_HOST_DATA_ACC_BITS 128

#define TYPE_ATI_VGA "ati-vga"
OBJECT_DECLARE_SIMPLE_TYPE(ATIVGAState, ATI_VGA)

typedef struct ATIVGARegs {
    uint32_t mm_index;
    uint32_t clock_cntl_index;
    uint32_t pll[64];
    uint32_t bios_scratch[8];
    uint32_t gen_int_cntl;
    uint32_t gen_int_status;
    uint32_t crtc_gen_cntl;
    uint32_t crtc_ext_cntl;
    uint32_t dac_cntl;
    uint32_t gpio_vga_ddc;
    uint32_t gpio_dvi_ddc;
    uint32_t gpio_monid;
    uint32_t config_cntl;
    uint32_t surface_cntl;
    uint32_t palette[256];
    uint32_t crtc_h_total_disp;
    uint32_t crtc_h_sync_strt_wid;
    uint32_t crtc_v_total_disp;
    uint32_t crtc_v_sync_strt_wid;
    uint32_t crtc_offset;
    uint32_t crtc_offset_cntl;
    uint32_t crtc_pitch;
    uint32_t cur_offset;
    uint32_t cur_hv_pos;
    uint32_t cur_hv_offs;
    uint32_t cur_color0;
    uint32_t cur_color1;
    uint32_t dst_offset;
    uint32_t dst_pitch;
    uint32_t dst_tile;
    uint32_t dst_width;
    uint32_t dst_height;
    uint32_t src_offset;
    uint32_t src_pitch;
    uint32_t src_tile;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t dst_line_start;
    uint32_t dp_gui_master_cntl;
    uint32_t dp_brush_bkgd_clr;
    uint32_t dp_brush_frgd_clr;
    uint32_t brush_data[64];
    uint32_t dp_src_frgd_clr;
    uint32_t dp_src_bkgd_clr;
    uint16_t sc_top;
    uint16_t sc_left;
    uint16_t sc_bottom;
    uint16_t sc_right;
    uint16_t src_sc_bottom;
    uint16_t src_sc_right;
    uint32_t dp_cntl;
    uint32_t dp_datatype;
    uint32_t dp_mix;
    uint32_t dp_write_mask;
    uint32_t default_offset;
    uint32_t default_pitch;
    uint16_t default_sc_bottom;
    uint16_t default_sc_right;
    uint32_t default_tile;
    uint32_t rbbm_guicntl;
} ATIVGARegs;

typedef struct ATIHostDataState {
    bool active;
    uint32_t src_source;
    uint32_t row;
    uint32_t col;
    uint32_t next;
    uint32_t acc[4];
} ATIHostDataState;

typedef struct ATICPParser {
    uint32_t hdr;
    uint32_t left;      /* dwords of the current packet still to come */
    uint32_t pos;
    uint32_t reg;
    uint32_t *buf;      /* body of a type 3 packet */
} ATICPParser;

typedef struct ATICPState {
    uint32_t mc_fb_location;
    bool mc_fb_location_set;
    uint32_t mc_agp_location;
    uint32_t agp_base;
    uint32_t aic_cntl;
    uint32_t aic_pt_base;
    uint32_t aic_lo_addr;
    uint32_t aic_hi_addr;
    uint32_t rb_base;
    uint32_t rb_cntl;
    uint32_t rb_rptr_addr;
    uint32_t rb_rptr;
    uint32_t rb_wptr;
    uint32_t ib_base;
    uint32_t ib_bufsz;
    uint32_t csq_cntl;
    uint32_t csq_mode;
    uint32_t scratch[8];
    uint32_t scratch_umsk;
    uint32_t scratch_addr;
    uint32_t me_cntl;
    uint32_t me_ram_addr;
    uint32_t me_ram_raddr;
    uint32_t me_ram[512];
    uint32_t soft_reset;
    int depth;
    bool in_reg;        /* a packet is writing a register */
    bool running;
    ATICPParser ring;
    ATICPParser ib;
} ATICPState;

/* the vertex arrays of 3D_LOAD_VBPNTR */
typedef struct ATIAos {
    uint32_t addr;
    uint8_t size;       /* dwords of a vertex */
    uint8_t stride;     /* dwords from a vertex to the next */
} ATIAos;

#define ATI_TCL_VECTORS 0x200
#define ATI_TCL_SCALARS 0x200
#define ATI_AOS_MAX 16

/* the memory of the transform and lighting engine, and its write ports */
typedef struct ATITCLState {
    float vec[ATI_TCL_VECTORS][4];
    float scl[ATI_TCL_SCALARS];
    uint32_t vec_idx;
    uint32_t vec_stride;
    uint32_t vec_dw;
    uint32_t vec_dwords;
    uint32_t scl_idx;
    uint32_t scl_stride;
    ATIAos aos[ATI_AOS_MAX];
    int naos;
} ATITCLState;

struct ATIVGAState {
    PCIDevice dev;
    VGACommonState vga;
    char *model;
    uint16_t dev_id;
    uint8_t mode;
    uint8_t use_pixman;
    bool cursor_guest_mode;
    bool agp;
    uint16_t cursor_size;
    uint32_t cursor_offset;
    uint32_t cursor_hv_offs;
    uint32_t cursor_hash;       /* of what the host cursor was made from */
    QEMUCursor *cursor;
    QEMUTimer vblank_timer;
    bitbang_i2c_interface bbi2c;
    I2CDDCState i2cddc;
    uint64_t linear_aper_sz;
    MemoryRegion linear_aper;
    MemoryRegion io;
    MemoryRegion mm;
    ATIVGARegs regs;
    ATIHostDataState host_data;
    ATICPState cp;
    ATITCLState tcl;
    /*
     * The visible buffer kept in another order than the memory around it,
     * as the display cannot show the swap that the CPU now uses there
     */
    bool vis_pin;
    uint32_t vis_lo, vis_hi;
    int vis_xm;
    uint32_t *mmregs;   /* registers that are only stored */
};

/* the R200 family: the Mobility M9 and the Radeon 9250 */
static inline bool ati_is_r200(ATIVGAState *s)
{
    return s->dev_id == PCI_DEVICE_ID_ATI_RADEON_LF ||
           s->dev_id == PCI_DEVICE_ID_ATI_RADEON_9250;
}

/*
 * The Radeon 7000 (R100 family) and the R200 family. Only they have the
 * command processor, the 3D engine, the surfaces and byte swap model, the
 * memory controller's address map, the frame timer that always runs and
 * the registers that read back what was written: the AmigaOS and MorphOS
 * drivers they were made for need them. The Rage 128, the ES1000 and the
 * Radeon 7500 (rv200) keep the behaviour they had before, as we do not know
 * that their guests (AIX on the ES1000, HP-UX 11.31 on the Radeon 7500)
 * would work with it.
 */
static inline bool ati_is_r100_r200(ATIVGAState *s)
{
    return s->dev_id == PCI_DEVICE_ID_ATI_RADEON_QY || ati_is_r200(s);
}

const char *ati_reg_name(int num);
void ati_tcl_write(ATIVGAState *s, uint32_t addr, uint32_t val);
void ati_mm_write(void *opaque, hwaddr addr, uint64_t data, unsigned int size);

#define ATI_SURFACES 8

uint32_t ati_fb_offset(ATIVGAState *s, uint32_t gpu_addr);
int ati_vram_xm(ATIVGAState *s, uint32_t off);
bool ati_gpu_vram_offset(ATIVGAState *s, uint32_t gpu_addr, uint32_t *off);
void ati_3d_packet(ATIVGAState *s, uint8_t op, const uint32_t *d, uint32_t n);
bool ati_gpu_rw(ATIVGAState *s, uint32_t addr, void *buf, uint32_t len,
                bool write);
bool ati_cp_mm_read(ATIVGAState *s, hwaddr addr, unsigned size,
                    uint32_t *val);
bool ati_cp_mm_write(ATIVGAState *s, hwaddr addr, uint32_t data,
                     unsigned size);
void ati_cp_reset(ATIVGAState *s);
void ati_cp_init(ATIVGAState *s);
void ati_cp_exit(ATIVGAState *s);

void ati_2d_blt(ATIVGAState *s);
void ati_2d_line(ATIVGAState *s, uint32_t end);
bool ati_host_data_flush(ATIVGAState *s);
void ati_host_data_finish(ATIVGAState *s);

#endif /* ATI_INT_H */
