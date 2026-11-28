/*
 * QEMU ATI SVGA emulation
 * Radeon R100 and R200 3D engine
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

/*
 * This is the rasterizer and pixel pipeline of the 3D engine. It takes
 * vertices in window coordinates (no hardware transform and lighting) and
 * draws points, lines, triangles, strips, fans, rectangles and quads with:
 *  - perspective correct colours and texture coordinates, with the 1/w and
 *    pre-multiplied forms of SE_COORD_FMT, and the viewport transform
 *  - flat, gouraud and solid shading, front and back face culling
 *  - up to two textures on the R100 (six on the R200), with mipmaps, nearest,
 *    linear and trilinear filtering and all the wrap and clamp modes
 *  - the R100 texture combiners and the R200 register combiners
 *  - specular colour and fog
 *  - alpha test, a depth buffer (16 bit and 24 bit with 8 bit stencil),
 *    stencil test, depth bias, alpha blending, logic operations and the
 *    colour plane mask
 * GLQuake draws its world with it through Warp3D on AmigaOS 4, and MorphOS
 * draws its anti-aliased text and translucent parts. The R200 (the Mobility
 * M9 of the Sam440ep) has the same rasterizer, texture formats and blending,
 * but its own vertex format registers, texture unit registers and register
 * combiners; AmigaOS 4.1 composites its windows and the AmiDock with them.
 *
 * The CPU sees the video memory through apertures that can swap the bytes
 * of each 16 or 32 bit word (SURFACE_CNTL), while the engine sees it
 * little endian. QEMU stores the video memory as the CPU wrote it, so the
 * engine reads and writes it through the swap of the aperture.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "trace.h"

/* registers of the 3D engine, stored in the register file */
#define R3D_PP_MISC             0x1c14
#define R3D_PP_FOG_COLOR        0x1c18
#define R3D_RE_SOLID_COLOR      0x1c1c
#define R3D_BLENDCNTL           0x1c20
#define R3D_DEPTHOFFSET         0x1c24
#define R3D_DEPTHPITCH          0x1c28
#define R3D_ZSTENCILCNTL        0x1c2c
#define R3D_PP_CNTL             0x1c38
#define R3D_CNTL                0x1c3c
#define R3D_COLOROFFSET         0x1c40
#define R3D_RE_WIDTH_HEIGHT     0x1c44
#define R3D_COLORPITCH          0x1c48
#define R3D_SE_CNTL             0x1c4c
#define R3D_SE_COORD_FMT        0x1c50
#define R3D_PP_TXFILTER(n)      (0x1c54 + (n) * 0x18)
#define R3D_PP_TXFORMAT(n)      (0x1c58 + (n) * 0x18)
#define R3D_PP_TXOFFSET(n)      (0x1c5c + (n) * 0x18)
#define R3D_PP_TXCBLEND(n)      (0x1c60 + (n) * 0x18)
#define R3D_PP_TXABLEND(n)      (0x1c64 + (n) * 0x18)
#define R3D_PP_TFACTOR(n)       (0x1c68 + (n) * 0x18)
#define R3D_PP_TEX_SIZE(n)      (0x1d04 + (n) * 8)
#define R3D_PP_TEX_PITCH(n)     (0x1d08 + (n) * 8)
#define R3D_PP_BORDER_COLOR(n)  (0x1d40 + (n) * 4)
#define R3D_STENCILREFMASK      0x1d7c
#define R3D_ROPCNTL             0x1d80
#define R3D_PLANEMASK           0x1d84
#define R3D_VPORT_XSCALE        0x1d98
#define R3D_VPORT_XOFFSET       0x1d9c
#define R3D_VPORT_YSCALE        0x1da0
#define R3D_VPORT_YOFFSET       0x1da4
#define R3D_VPORT_ZSCALE        0x1da8
#define R3D_VPORT_ZOFFSET       0x1dac
#define R3D_SE_ZBIAS_FACTOR     0x1db0
#define R3D_SE_ZBIAS_CONSTANT   0x1db4
#define R3D_RE_TOP_LEFT         0x26c0
#define R3D_SE_VTX_FMT          0x2080

/* R200 */
#define R200_SE_VTE_CNTL        0x20b0
#define R200_VTX_ST_DENORM      BIT(12)
#define R200_SE_VAP_CNTL        0x2080
#define R200_VAP_TCL_ENABLE     BIT(0)
#define R200_VAP_DX_CLIP_SPACE  BIT(22)
#define R200_SE_VAP_CNTL_STATUS 0x2140
#define R200_SE_VTX_FMT_0       0x2088
#define R200_SE_VTX_FMT_1       0x208c
#define R200_PP_TXFILTER(n)     (0x2c00 + (n) * 0x20)
#define R200_PP_TXFORMAT(n)     (0x2c04 + (n) * 0x20)
#define R200_PP_TXSIZE(n)       (0x2c0c + (n) * 0x20)
#define R200_PP_TXPITCH(n)      (0x2c10 + (n) * 0x20)
#define R200_PP_TXOFFSET(n)     (0x2d00 + (n) * 0x18)
#define R200_PP_TFACTOR(n)      (0x2ee0 + (n) * 4)
#define R200_PP_TXCBLEND(n)     (0x2f00 + (n) * 0x10)
#define R200_PP_TXCBLEND2(n)    (0x2f04 + (n) * 0x10)
#define R200_PP_TXABLEND(n)     (0x2f08 + (n) * 0x10)
#define R200_PP_TXABLEND2(n)    (0x2f0c + (n) * 0x10)

/* R200 SE_VTX_FMT_0 */
#define R200_VTX_Z0             BIT(0)
#define R200_VTX_W0             BIT(1)
#define R200_VTX_WEIGHTS(f)     (((f) >> 2) & 7)
#define R200_VTX_N0             BIT(6)
#define R200_VTX_POINT_SIZE     BIT(7)
#define R200_VTX_DISCRETE_FOG   BIT(8)
#define R200_VTX_SHININESS_0    BIT(9)
#define R200_VTX_SHININESS_1    BIT(10)
#define R200_VTX_COLOR(f, n)    (((f) >> (11 + (n) * 2)) & 3)

/* R200 combiner arguments and controls */
#define R200_TX_COMP(i)         BIT(16 + (i) * 4)
#define R200_TX_BIAS(i)         BIT(17 + (i) * 4)
#define R200_TX_SCALE(i)        BIT(18 + (i) * 4)
#define R200_TX_NEG(i)          BIT(19 + (i) * 4)
#define R200_TX_OP(v)           (((v) >> 28) & 7)
#define R200_TX2_SCALE(v)       (((v) >> 8) & 7)
#define R200_TX2_CLAMP(v)       (((v) >> 12) & 3)
#define R200_TX2_OUTPUT_REG(v)  (((v) >> 16) & 7)
#define R200_TX2_REPL(v, i)     (((v) >> (26 + (i) * 2)) & 3)

/* PP_CNTL */
#define PP_TEX_ENABLE(n)        BIT(4 + (n))
#define PP_TEX_BLEND_ENABLE(n)  BIT(12 + (n))
#define PP_SPECULAR_ENABLE      BIT(21)
#define PP_FOG_ENABLE           BIT(22)
#define PP_ALPHA_TEST_ENABLE    BIT(23)

/* PP_FOG_COLOR */
#define FOG_TABLE               BIT(24)
#define FOG_SOURCE(v)           (((v) >> 25) & 3)

/* RB3D_CNTL */
#define RB_ALPHA_BLEND_ENABLE   BIT(0)
#define RB_PLANE_MASK_ENABLE    BIT(1)
#define RB_ROP_ENABLE           BIT(6)
#define RB_STENCIL_ENABLE       BIT(7)
#define RB_Z_ENABLE             BIT(8)
#define RB_COLOR_FORMAT(c)      (((c) >> 10) & 0xf)

/* RB3D_ZSTENCILCNTL */
#define ZS_DEPTH_FORMAT(v)      ((v) & 0xf)
#define ZS_Z_TEST(v)            (((v) >> 4) & 7)
#define ZS_STENCIL_TEST(v)      (((v) >> 12) & 7)
#define ZS_STENCIL_FAIL(v)      (((v) >> 16) & 7)
#define ZS_STENCIL_ZPASS(v)     (((v) >> 20) & 7)
#define ZS_STENCIL_ZFAIL(v)     (((v) >> 24) & 7)
#define ZS_Z_WRITE_ENABLE       BIT(30)

/* SE_CNTL */
#define SE_FFACE_CCW            BIT(0)
#define SE_BFACE_MODE(v)        (((v) >> 1) & 3)
#define SE_FFACE_MODE(v)        (((v) >> 3) & 3)
#define SE_FLAT_SHADE_VTX(v)    (((v) >> 6) & 3)
#define SE_DIFFUSE_SHADE(v)     (((v) >> 8) & 3)
#define SE_ALPHA_SHADE(v)       (((v) >> 10) & 3)
#define SE_SPECULAR_SHADE(v)    (((v) >> 12) & 3)
#define SE_FOG_SHADE(v)         (((v) >> 14) & 3)
#define SE_ZBIAS_POINT          BIT(16)
#define SE_ZBIAS_LINE           BIT(17)
#define SE_ZBIAS_TRI            BIT(18)
#define SE_VPORT_XY_XFORM       BIT(24)
#define SE_VPORT_Z_XFORM        BIT(25)
#define SE_PIX_CENTER_OGL       BIT(27)

#define SHADE_SOLID             0
#define SHADE_FLAT              1

/* SE_COORD_FMT */
#define CF_XY_PRE_MULT          BIT(0)
#define CF_Z_PRE_MULT           BIT(1)
#define CF_W0_IS_NOT_1_OVER_W0  BIT(16)
#define CF_ST_PRE_MULT(n)       BIT(17 + (n) * 2)

/* SE_VTX_FMT */
#define VTX_W0                  BIT(0)
#define VTX_FPCOLOR             BIT(1)
#define VTX_FPALPHA             BIT(2)
#define VTX_PKCOLOR             BIT(3)
#define VTX_FPSPEC              BIT(4)
#define VTX_FPFOG               BIT(5)
#define VTX_PKSPEC              BIT(6)
#define VTX_ST0                 BIT(7)
#define VTX_ST1                 BIT(8)
#define VTX_Q1                  BIT(9)
#define VTX_ST2                 BIT(10)
#define VTX_Q2                  BIT(11)
#define VTX_ST3                 BIT(12)
#define VTX_Q3                  BIT(13)
#define VTX_Q0                  BIT(14)
#define VTX_WEIGHTS(f)          (((f) >> 15) & 7)
#define VTX_N0                  BIT(18)
#define VTX_Z                   BIT(31)

/* SE_VF_CNTL */
#define VF_PRIM(v)              ((v) & 0xf)
#define VF_WALK(v)              (((v) >> 4) & 3)
#define VF_WALK_IND             1
#define VF_WALK_LIST            2
#define VF_COLOR_ORDER_RGBA     BIT(6)
#define VF_TCL_ENABLE           BIT(9)          /* R100 */
#define VF_NUM_VERTICES(v)      ((v) >> 16)

/* PP_TXFORMAT */
#define TXF_FORMAT(f)           ((f) & 0x1f)
#define TXF_ALPHA_IN_MAP        BIT(6)
#define TXF_NON_POWER2          BIT(7)
#define TXF_WIDTH_LOG2(f)       (((f) >> 8) & 0xf)
#define TXF_HEIGHT_LOG2(f)      (((f) >> 12) & 0xf)

/* PP_TXFILTER */
#define TXFL_MAG_LINEAR         BIT(0)
#define TXFL_MIN(f)             (((f) >> 1) & 0xf)
#define TXFL_MAX_MIP(f)         (((f) >> 16) & 0xf)
#define TXFL_CLAMP_S(f)         (((f) >> 23) & 7)
#define TXFL_CLAMP_T(f)         (((f) >> 27) & 7)

#define ATI_3D_MAX_VERTS        0x4000
#define ATI_3D_UNITS            6       /* R200; the R100 uses two */
#define R100_UNITS              2
#define ATI_TEX_LEVELS          12

/*
 * A vertex in window coordinates. The colours and texture coordinates are
 * the plain values; rhw is 1/w, by which the rasterizer interpolates them.
 */
typedef struct ATIVertex {
    float x, y, z, rhw;
    float c[4];                 /* diffuse r, g, b, a */
    float spec[4];              /* specular r, g, b and fog */
    float tc[ATI_3D_UNITS][3];  /* s, t, q */
} ATIVertex;

/* the attributes interpolated in the vertex, after x, y, z and rhw */
#define ATI_ATTRS               (4 + 4 + ATI_3D_UNITS * 3)

typedef struct ATITexLevel {
    uint32_t off;               /* from the start of level 0 */
    int w, h, pitch;
} ATITexLevel;

typedef struct ATITexture {
    bool enabled;
    const uint8_t *data;
    uint8_t *copy;              /* data fetched from system memory */
    int xm;                     /* byte address swap of the aperture */
    int fmt;
    bool alpha_in_map;
    int bypp;
    int w, h;
    int levels;
    ATITexLevel lvl[ATI_TEX_LEVELS];
    int swap;
    bool mag_linear;
    int min;                    /* TXFL_MIN */
    int clamp_s, clamp_t;
    float border[4];
    bool nonparam;
    int route;                  /* the texture coordinates it uses */
} ATITexture;

typedef struct ATI3DCtx {
    ATIVGAState *s;
    uint32_t cb_offset;
    int cb_pitch;               /* in pixels */
    int cb_fmt;
    int cb_bypp;
    int xm;                     /* byte address swap of the frame buffer */
    int clip_x0, clip_y0, clip_x1, clip_y1;
    uint32_t rb3d_cntl;
    uint32_t blendcntl;
    uint32_t planemask;
    uint32_t rop;
    uint32_t pp_cntl;
    uint32_t pp_misc;
    uint32_t se_cntl;
    uint32_t coord_fmt;
    float fog[4];
    uint32_t fog_cntl;
    float solid[4];
    /* depth and stencil */
    bool z_test, z_write, stencil;
    uint32_t zs_cntl;
    uint32_t zb_offset;
    int zb_pitch;               /* in pixels */
    int zb_bypp;
    uint32_t zmask;
    uint32_t st_ref, st_mask, st_wmask;
    float zbias_factor, zbias_const;
    bool ogl_center;
    bool r200;
    int units;
    ATITexture tex[ATI_3D_UNITS];
    uint32_t dirty_lo, dirty_hi;
    /*
     * With the R200's transform and lighting on, the vertices are in clip
     * space (rhw holds w): they are clipped and projected to the window
     * just before they are drawn.
     */
    bool tcl;
    uint32_t vte_cntl;
    float vport[6];             /* x, y and z scale and offset */
} ATI3DCtx;

static inline uint32_t r3d(ATIVGAState *s, uint32_t reg)
{
    return s->mmregs[reg / 4];
}

static inline float u2f(uint32_t v)
{
    union {
        uint32_t u;
        float f;
    } x = { .u = v };
    return x.f;
}

static inline uint32_t f2u(float f)
{
    uint32_t v;

    memcpy(&v, &f, sizeof(v));
    return v;
}

static inline float clampf(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static void argb_to_float(uint32_t v, float out[4])
{
    out[0] = ((v >> 16) & 0xff) / 255.0f;
    out[1] = ((v >> 8) & 0xff) / 255.0f;
    out[2] = (v & 0xff) / 255.0f;
    out[3] = (v >> 24) / 255.0f;
}

/* Video memory */

/* loads bypp bytes little endian at base + off, as the engine sees them */
static uint32_t ati_ld(const uint8_t *base, uint32_t off, int bypp, int xm)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < bypp; i++) {
        v |= (uint32_t)base[(off + i) ^ xm] << (i * 8);
    }
    return v;
}

static void ati_st(uint8_t *base, uint32_t off, int bypp, int xm, uint32_t v)
{
    int i;

    for (i = 0; i < bypp; i++) {
        base[(off + i) ^ xm] = v >> (i * 8);
    }
}

/* Pixels and texels */

static uint32_t ati_swap(uint32_t v, int bypp, int swap)
{
    switch (swap) {
    case 1:
        return bypp == 4 ? ((v & 0x00ff00ff) << 8) | ((v >> 8) & 0x00ff00ff) :
               bypp == 2 ? bswap16(v) : v;
    case 2:
        return bypp == 4 ? bswap32(v) : bypp == 2 ? bswap16(v) : v;
    case 3:
        return bypp == 4 ? ror32(v, 16) : v;
    default:
        return v;
    }
}

static void ati_texel_decode(const ATITexture *t, uint32_t v, float out[4])
{
    int a = 255, r, g, b;

    switch (t->fmt) {
    case 0: /* I8 */
    case 8: /* Y8 */
        r = g = b = v & 0xff;
        if (t->alpha_in_map) {
            a = r;
        }
        break;
    case 1: /* AI88 */
        r = g = b = v & 0xff;
        if (t->alpha_in_map) {
            a = (v >> 8) & 0xff;
        }
        break;
    case 2: /* RGB332 */
        r = ((v >> 5) & 7) * 255 / 7;
        g = ((v >> 2) & 7) * 255 / 7;
        b = (v & 3) * 255 / 3;
        break;
    case 3: /* ARGB1555 */
        r = ((v >> 10) & 0x1f) * 255 / 31;
        g = ((v >> 5) & 0x1f) * 255 / 31;
        b = (v & 0x1f) * 255 / 31;
        if (t->alpha_in_map) {
            a = v & 0x8000 ? 255 : 0;
        }
        break;
    case 4: /* RGB565 */
        r = ((v >> 11) & 0x1f) * 255 / 31;
        g = ((v >> 5) & 0x3f) * 255 / 63;
        b = (v & 0x1f) * 255 / 31;
        break;
    case 5: /* ARGB4444 */
        r = ((v >> 8) & 0xf) * 17;
        g = ((v >> 4) & 0xf) * 17;
        b = (v & 0xf) * 17;
        if (t->alpha_in_map) {
            a = ((v >> 12) & 0xf) * 17;
        }
        break;
    case 7: /* RGBA8888 */
        r = (v >> 24) & 0xff;
        g = (v >> 16) & 0xff;
        b = (v >> 8) & 0xff;
        if (t->alpha_in_map) {
            a = v & 0xff;
        }
        break;
    case 6: /* ARGB8888 */
    default:
        r = (v >> 16) & 0xff;
        g = (v >> 8) & 0xff;
        b = v & 0xff;
        if (t->alpha_in_map) {
            a = (v >> 24) & 0xff;
        }
        break;
    }
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

/*
 * The texel index for a wrap or clamp mode, or -1 for the border colour:
 * 0 wrap, 1 mirror, 2 clamp to the last texel, 3 mirror once, 4 clamp to
 * the border, 5 mirror once to the border, 6 and 7 as 2 and 3 for GL.
 */
static int ati_tex_wrap(int i, int size, int mode)
{
    switch (mode) {
    case 0:
        i %= size;
        return i < 0 ? i + size : i;
    case 1: {
        int p = size * 2;

        i %= p;
        i = i < 0 ? i + p : i;
        return i >= size ? p - 1 - i : i;
    }
    case 3:
    case 5:
    case 7:
        if (i < 0) {
            i = -1 - i;
        }
        if (mode == 5) {
            return i >= size ? -1 : i;
        }
        return i >= size ? size - 1 : i;
    case 4:
        return i < 0 || i >= size ? -1 : i;
    default:
        return i < 0 ? 0 : i >= size ? size - 1 : i;
    }
}

static void ati_tex_fetch(const ATITexture *t, int level, int x, int y,
                          float out[4])
{
    const ATITexLevel *l = &t->lvl[level];
    uint32_t v;

    x = ati_tex_wrap(x, l->w, t->clamp_s);
    y = ati_tex_wrap(y, l->h, t->clamp_t);
    if (x < 0 || y < 0) {
        memcpy(out, t->border, sizeof(t->border));
        return;
    }
    if (t->fmt == 10 || t->fmt == 11) {
        /* packed YUV 4:2:2, two pixels in a dword */
        uint32_t pair = ati_ld(t->data, l->off + y * l->pitch + (x & ~1) * 2,
                               4, t->xm);
        int yy, u, vv;
        float yf;

        pair = ati_swap(pair, 4, t->swap);
        if (t->fmt == 10) { /* VYUY */
            yy = (x & 1) ? (pair >> 24) & 0xff : (pair >> 8) & 0xff;
            u = pair & 0xff;
            vv = (pair >> 16) & 0xff;
        } else { /* YVYU */
            yy = (x & 1) ? (pair >> 16) & 0xff : pair & 0xff;
            vv = (pair >> 8) & 0xff;
            u = (pair >> 24) & 0xff;
        }
        yf = 1.164f * (yy - 16);
        out[0] = clampf((yf + 1.596f * (vv - 128)) / 255.0f);
        out[1] = clampf((yf - 0.813f * (vv - 128) - 0.391f * (u - 128)) /
                        255.0f);
        out[2] = clampf((yf + 2.018f * (u - 128)) / 255.0f);
        out[3] = 1.0f;
        return;
    }
    v = ati_ld(t->data, l->off + y * l->pitch + x * t->bypp, t->bypp, t->xm);
    v = ati_swap(v, t->bypp, t->swap);
    ati_texel_decode(t, v, out);
}

/* samples one mipmap level at s, t (0 to 1, or texels if nonparam) */
static void ati_tex_sample_level(const ATITexture *t, int level, bool linear,
                                 float s, float tt, float out[4])
{
    const ATITexLevel *l = &t->lvl[level];
    float u = t->nonparam ? s / (1 << level) : s * l->w;
    float v = t->nonparam ? tt / (1 << level) : tt * l->h;

    if (linear) {
        float t00[4], t10[4], t01[4], t11[4];
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
        float ax = fu - x0, ay = fv - y0;
        int i;

        ati_tex_fetch(t, level, x0, y0, t00);
        ati_tex_fetch(t, level, x0 + 1, y0, t10);
        ati_tex_fetch(t, level, x0, y0 + 1, t01);
        ati_tex_fetch(t, level, x0 + 1, y0 + 1, t11);
        for (i = 0; i < 4; i++) {
            out[i] = (t00[i] * (1 - ax) + t10[i] * ax) * (1 - ay) +
                     (t01[i] * (1 - ax) + t11[i] * ax) * ay;
        }
    } else {
        ati_tex_fetch(t, level, (int)floorf(u), (int)floorf(v), out);
    }
}

/* the min filters that blend four texels */
static bool ati_min_linear(int min)
{
    return min == 1 || min == 6 || min == 7 || min == 9;
}

/*
 * Samples a texture with the level of detail lod (log2 of the texels per
 * pixel): magnified with the mag filter, minified with the min filter,
 * which may pick one mipmap level or blend two.
 */
static void ati_tex_sample(const ATITexture *t, float s, float tt, float lod,
                           float out[4])
{
    bool linear;
    int mip;                    /* 0 none, 1 nearest level, 2 two levels */
    int l0, l1, i;
    float a, o1[4];

    if (lod <= 0.0f || t->levels <= 1) {
        linear = lod <= 0.0f ? t->mag_linear : ati_min_linear(t->min);
        ati_tex_sample_level(t, 0, linear, s, tt, out);
        return;
    }
    switch (t->min) {
    case 0: case 8:
        linear = false; mip = 0;
        break;
    case 1: case 9:
        linear = true; mip = 0;
        break;
    case 2: case 10:
        linear = false; mip = 1;
        break;
    case 3: case 11:
        linear = false; mip = 2;
        break;
    case 6:
        linear = true; mip = 1;
        break;
    default: /* 7: trilinear */
        linear = true; mip = 2;
        break;
    }
    if (!mip) {
        ati_tex_sample_level(t, 0, linear, s, tt, out);
        return;
    }
    if (mip == 1) {
        l0 = MIN((int)floorf(lod + 0.5f), t->levels - 1);
        ati_tex_sample_level(t, l0, linear, s, tt, out);
        return;
    }
    l0 = MIN((int)floorf(lod), t->levels - 1);
    l1 = MIN(l0 + 1, t->levels - 1);
    a = lod - floorf(lod);
    ati_tex_sample_level(t, l0, linear, s, tt, out);
    if (l1 != l0) {
        ati_tex_sample_level(t, l1, linear, s, tt, o1);
        for (i = 0; i < 4; i++) {
            out[i] = out[i] * (1 - a) + o1[i] * a;
        }
    }
}

static int ati_tex_bypp(int fmt)
{
    switch (fmt) {
    case 0:
    case 2:
    case 8:
        return 1;
    case 6:
    case 7:
        return 4;
    default:
        return 2;
    }
}

/* the texture registers have the same layout on R100 and R200 */
static bool ati_tex_setup(ATI3DCtx *c, int n)
{
    ATIVGAState *s = c->s;
    ATITexture *t = &c->tex[n];
    uint32_t fmt = r3d(s, c->r200 ? R200_PP_TXFORMAT(n) : R3D_PP_TXFORMAT(n));
    uint32_t filter = r3d(s, c->r200 ? R200_PP_TXFILTER(n) :
                                       R3D_PP_TXFILTER(n));
    uint32_t offset = r3d(s, c->r200 ? R200_PP_TXOFFSET(n) :
                                       R3D_PP_TXOFFSET(n));
    uint32_t addr = offset & ~0x1f;
    uint32_t size = 0;
    uint32_t start;
    int i, max;

    t->fmt = TXF_FORMAT(fmt);
    t->alpha_in_map = fmt & TXF_ALPHA_IN_MAP;
    t->bypp = ati_tex_bypp(t->fmt);
    t->route = c->r200 ? MIN((fmt >> 24) & 7, ATI_3D_UNITS - 1) : n;
    if (fmt & TXF_NON_POWER2) {
        uint32_t sz = r3d(s, c->r200 ? R200_PP_TXSIZE(n) : R3D_PP_TEX_SIZE(n));
        uint32_t pitch = r3d(s, c->r200 ? R200_PP_TXPITCH(n) :
                                          R3D_PP_TEX_PITCH(n));

        t->w = (sz & 0x7ff) + 1;
        t->h = ((sz >> 16) & 0x7ff) + 1;
        t->levels = 1;
        t->lvl[0].off = 0;
        t->lvl[0].w = t->w;
        t->lvl[0].h = t->h;
        t->lvl[0].pitch = (pitch & 0x3fe0) + 32;
        size = t->lvl[0].pitch * t->h;
    } else {
        int lw = TXF_WIDTH_LOG2(fmt), lh = TXF_HEIGHT_LOG2(fmt);

        t->w = 1 << lw;
        t->h = 1 << lh;
        /*
         * The levels follow each other, each one's rows 32 byte aligned;
         * the filter says how many there are.
         */
        max = MIN(TXFL_MAX_MIP(filter), MAX(lw, lh));
        max = MIN(max, ATI_TEX_LEVELS - 1);
        t->levels = max + 1;
        for (i = 0; i <= max; i++) {
            ATITexLevel *l = &t->lvl[i];

            l->w = MAX(t->w >> i, 1);
            l->h = MAX(t->h >> i, 1);
            l->pitch = ROUND_UP(l->w * t->bypp, 32);
            l->off = size;
            size += l->pitch * l->h;
        }
    }
    t->swap = offset & 3;
    t->mag_linear = filter & TXFL_MAG_LINEAR;
    t->min = TXFL_MIN(filter);
    t->clamp_s = TXFL_CLAMP_S(filter);
    t->clamp_t = TXFL_CLAMP_T(filter);
    argb_to_float(c->r200 ? 0 : r3d(s, R3D_PP_BORDER_COLOR(n)), t->border);
    /* R200: SE_VTE_CNTL says texture coordinates are in texels */
    t->nonparam = c->r200 ? !!(r3d(s, R200_SE_VTE_CNTL) & R200_VTX_ST_DENORM) :
                            !!(c->coord_fmt & BIT(8 + n));

    /* textures in video memory are read in place, others are copied */
    if (ati_gpu_vram_offset(s, addr, &start) &&
        start + size <= s->vga.vram_size) {
        t->data = s->vga.vram_ptr + start;
        t->xm = ati_vram_xm(s, start);
        t->copy = NULL;
        t->enabled = true;
        trace_ati_3d_texture(n, offset, fmt, filter, t->xm);
        return true;
    }
    t->copy = g_try_malloc(size);
    if (!t->copy) {
        return false;
    }
    ati_gpu_rw(s, addr, t->copy, size, false);
    t->data = t->copy;
    t->xm = 0;
    t->enabled = true;
    trace_ati_3d_texture(n, offset, fmt, filter, -1);
    return true;
}

/* Texture combiners */

static void ati_comb_arg_color(int arg, const float cur[4], const float dif[4],
                               const float spec[4], const float tf[4],
                               float tx[][4], float out[3])
{
    const float *src;
    bool alpha = arg & 1;

    switch (arg) {
    case 2:
    case 3:
        src = cur;
        break;
    case 4:
    case 5:
        src = dif;
        break;
    case 6:
    case 7:
        src = spec;
        break;
    case 8:
    case 9:
        src = tf;
        break;
    case 10 ... 13:
        src = tx[(arg - 10) / 2];
        break;
    default:
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    if (alpha) {
        out[0] = out[1] = out[2] = src[3];
    } else {
        out[0] = src[0];
        out[1] = src[1];
        out[2] = src[2];
    }
}

static float ati_comb_arg_alpha(int arg, const float cur[4], const float dif[4],
                                const float spec[4], const float tf[4],
                                float tx[][4])
{
    switch (arg) {
    case 1:
        return cur[3];
    case 2:
        return dif[3];
    case 3:
        return spec[3];
    case 4:
        return tf[3];
    case 5:
    case 6:
        return tx[arg - 5][3];
    default:
        return 0.0f;
    }
}

static float ati_comb_op(uint32_t ctl, float a, float b, float cc)
{
    float r;

    switch ((ctl >> 18) & 7) {
    case 1: /* subtract */
        r = a * b - cc;
        break;
    case 2: /* add signed */
        r = a * b + cc - 0.5f;
        break;
    case 3: /* blend */
        r = a * (1.0f - cc) + b * cc;
        break;
    default: /* add */
        r = a * b + cc;
        break;
    }
    r *= 1 << ((ctl >> 21) & 3);
    return clampf(r);
}

static void ati_combine(ATI3DCtx *c, int n, float cur[4], const float dif[4],
                        const float spec[4], float tx[][4])
{
    ATIVGAState *s = c->s;
    uint32_t cb = r3d(s, R3D_PP_TXCBLEND(n));
    uint32_t ab = r3d(s, R3D_PP_TXABLEND(n));
    float tf[4], a[3], b[3], cc[3], res[4];
    float aa, ba, ca;
    int i;

    argb_to_float(r3d(s, R3D_PP_TFACTOR(n)), tf);
    ati_comb_arg_color(cb & 0x1f, cur, dif, spec, tf, tx, a);
    ati_comb_arg_color((cb >> 5) & 0x1f, cur, dif, spec, tf, tx, b);
    ati_comb_arg_color((cb >> 10) & 0x1f, cur, dif, spec, tf, tx, cc);
    for (i = 0; i < 3; i++) {
        if (cb & BIT(15)) {
            a[i] = 1.0f - a[i];
        }
        if (cb & BIT(16)) {
            b[i] = 1.0f - b[i];
        }
        if (cb & BIT(17)) {
            cc[i] = 1.0f - cc[i];
        }
        res[i] = ati_comb_op(cb, a[i], b[i], cc[i]);
    }
    if (((cb >> 18) & 7) == 4) {
        /* dot3: the dot product of A and B as signed vectors */
        float d = 0.0f;

        for (i = 0; i < 3; i++) {
            d += (a[i] * 2.0f - 1.0f) * (b[i] * 2.0f - 1.0f);
        }
        res[0] = res[1] = res[2] = clampf(d * (1 << ((cb >> 21) & 3)));
    }
    aa = ati_comb_arg_alpha(ab & 0xf, cur, dif, spec, tf, tx);
    ba = ati_comb_arg_alpha((ab >> 4) & 0xf, cur, dif, spec, tf, tx);
    ca = ati_comb_arg_alpha((ab >> 8) & 0xf, cur, dif, spec, tf, tx);
    if (ab & BIT(15)) {
        aa = 1.0f - aa;
    }
    if (ab & BIT(16)) {
        ba = 1.0f - ba;
    }
    if (ab & BIT(17)) {
        ca = 1.0f - ca;
    }
    res[3] = ati_comb_op(ab, aa, ba, ca);
    memcpy(cur, res, sizeof(res));
}

/* R200 register combiners: the textures are in R0-R5, R0 is the result */

static void r200_comb_arg(int arg, int repl, bool alpha_stage, float r[][4],
                          const float dif[4], const float tf[4],
                          const float tf1[4], float out[4])
{
    const float *src;

    switch (arg) {
    case 2:
    case 3:
        src = r[0];
        break;
    case 4:
    case 5:
        src = dif;
        break;
    case 8:
    case 9:
        src = tf;
        break;
    case 10 ... 21:
        src = r[(arg - 10) / 2];
        break;
    case 26:
    case 27:
        src = tf1;
        break;
    default:
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    if (alpha_stage) {
        /* even arguments are alpha, odd ones blue */
        out[3] = src[arg & 1 ? 2 : 3];
        return;
    }
    if (arg & 1) {
        out[0] = out[1] = out[2] = src[3];
    } else if (repl) {
        out[0] = out[1] = out[2] = src[repl - 1];
    } else {
        memcpy(out, src, 3 * sizeof(float));
    }
}

static float r200_comb_mod(uint32_t ctl, int i, float v)
{
    if (ctl & R200_TX_COMP(i)) {
        v = 1.0f - v;
    }
    if (ctl & R200_TX_BIAS(i)) {
        v -= 0.5f;
    }
    if (ctl & R200_TX_SCALE(i)) {
        v *= 2.0f;
    }
    if (ctl & R200_TX_NEG(i)) {
        v = -v;
    }
    return v;
}

static float r200_comb_out(uint32_t ctl, uint32_t ctl2, const float *a,
                           const float *b, const float *cc)
{
    static const float scale[8] = {
        1.0f, 2.0f, 4.0f, 8.0f, 1.0f, 0.5f, 0.25f, 0.125f
    };
    float va = r200_comb_mod(ctl, 0, *a);
    float vb = r200_comb_mod(ctl, 1, *b);
    float vc = r200_comb_mod(ctl, 2, *cc);
    float r;

    switch (R200_TX_OP(ctl)) {
    case 3: /* lerp from A to B by C, as Mesa's r200 maps GL_INTERPOLATE */
        r = va * (1.0f - vc) + vb * vc;
        break;
    default: /* multiply and add */
        r = va * vb + vc;
        break;
    }
    r *= scale[R200_TX2_SCALE(ctl2)];
    return R200_TX2_CLAMP(ctl2) == 1 ? clampf(r) : r;
}

static void r200_combine(ATI3DCtx *c, int n, float r[][4], const float dif[4])
{
    ATIVGAState *s = c->s;
    uint32_t cb = r3d(s, R200_PP_TXCBLEND(n));
    uint32_t cb2 = r3d(s, R200_PP_TXCBLEND2(n));
    uint32_t ab = r3d(s, R200_PP_TXABLEND(n));
    uint32_t ab2 = r3d(s, R200_PP_TXABLEND2(n));
    float tf[4], tf1[4], arg[3][4], res[4];
    int i, k, reg;

    argb_to_float(r3d(s, R200_PP_TFACTOR(cb2 & 7)), tf);
    argb_to_float(r3d(s, R200_PP_TFACTOR((cb2 >> 4) & 7)), tf1);

    for (k = 0; k < 3; k++) {
        r200_comb_arg((cb >> (k * 5)) & 0x1f, R200_TX2_REPL(cb2, k), false,
                      r, dif, tf, tf1, arg[k]);
    }
    for (i = 0; i < 3; i++) {
        res[i] = r200_comb_out(cb, cb2, &arg[0][i], &arg[1][i], &arg[2][i]);
    }
    for (k = 0; k < 3; k++) {
        r200_comb_arg((ab >> (k * 5)) & 0x1f, 0, true, r, dif, tf, tf1,
                      arg[k]);
    }
    res[3] = r200_comb_out(ab, ab2, &arg[0][3], &arg[1][3], &arg[2][3]);

    /* the colour and the alpha each go to the register they name */
    reg = R200_TX2_OUTPUT_REG(cb2);
    if (reg && reg <= ATI_3D_UNITS && ((cb2 >> 20) & 7) != 7) {
        memcpy(r[reg - 1], res, 3 * sizeof(float));
    }
    reg = R200_TX2_OUTPUT_REG(ab2);
    if (reg && reg <= ATI_3D_UNITS) {
        r[reg - 1][3] = res[3];
    }
}

/* Frame buffer blending and output */

static float ati_blend_factor(int f, const float src[4], const float dst[4],
                              int ch)
{
    switch (f) {
    case 32:
        return 0.0f;
    case 33:
        return 1.0f;
    case 34:
        return src[ch];
    case 35:
        return 1.0f - src[ch];
    case 36:
        return dst[ch];
    case 37:
        return 1.0f - dst[ch];
    case 38:
        return src[3];
    case 39:
        return 1.0f - src[3];
    case 40:
        return dst[3];
    case 41:
        return 1.0f - dst[3];
    case 42:
        return ch == 3 ? 1.0f : MIN(src[3], 1.0f - dst[3]);
    default:
        return 1.0f;
    }
}

static void ati_cb_unpack(int fmt, uint32_t v, float out[4])
{
    switch (fmt) {
    case 3: /* ARGB1555 */
        out[0] = ((v >> 10) & 0x1f) / 31.0f;
        out[1] = ((v >> 5) & 0x1f) / 31.0f;
        out[2] = (v & 0x1f) / 31.0f;
        out[3] = v & 0x8000 ? 1.0f : 0.0f;
        break;
    case 4: /* RGB565 */
        out[0] = ((v >> 11) & 0x1f) / 31.0f;
        out[1] = ((v >> 5) & 0x3f) / 63.0f;
        out[2] = (v & 0x1f) / 31.0f;
        out[3] = 1.0f;
        break;
    case 15: /* ARGB4444 */
        out[0] = ((v >> 8) & 0xf) / 15.0f;
        out[1] = ((v >> 4) & 0xf) / 15.0f;
        out[2] = (v & 0xf) / 15.0f;
        out[3] = ((v >> 12) & 0xf) / 15.0f;
        break;
    default: /* ARGB8888 */
        argb_to_float(v, out);
        break;
    }
}

static uint32_t ati_cb_pack(int fmt, const float c[4])
{
    int r = c[0] * 255.0f + 0.5f, g = c[1] * 255.0f + 0.5f;
    int b = c[2] * 255.0f + 0.5f, a = c[3] * 255.0f + 0.5f;

    switch (fmt) {
    case 3:
        return (a >= 128 ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 |
               b >> 3;
    case 4:
        return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    case 15:
        return (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4;
    default:
        return (uint32_t)a << 24 | r << 16 | g << 8 | b;
    }
}

/* the eight compare functions of the alpha, depth and stencil tests */
static bool ati_compare(int fn, uint32_t a, uint32_t b)
{
    switch (fn) {
    case 0:
        return false;
    case 1:
        return a < b;
    case 2:
        return a <= b;
    case 3:
        return a == b;
    case 4:
        return a >= b;
    case 5:
        return a > b;
    case 6:
        return a != b;
    default:
        return true;
    }
}

static uint32_t ati_rop(int rop, uint32_t s, uint32_t d)
{
    switch (rop) {
    case 0:
        return 0;
    case 1:
        return ~(s | d);
    case 2:
        return ~s & d;
    case 3:
        return ~s;
    case 4:
        return s & ~d;
    case 5:
        return ~d;
    case 6:
        return s ^ d;
    case 7:
        return ~(s & d);
    case 8:
        return s & d;
    case 9:
        return ~(s ^ d);
    case 10:
        return d;
    case 11:
        return ~s | d;
    case 12:
        return s;
    case 13:
        return s | ~d;
    case 14:
        return s | d;
    default:
        return ~0u;
    }
}

static uint8_t ati_stencil_op(int op, uint8_t v, uint8_t ref)
{
    switch (op) {
    case 1:
        return 0;
    case 2:
        return ref;
    case 3:
        return v == 0xff ? v : v + 1;
    case 4:
        return v ? v - 1 : 0;
    case 5:
        return ~v;
    case 6:
        return v + 1;
    case 7:
        return v - 1;
    default:
        return v;
    }
}

/*
 * The stencil and depth tests. Returns whether the fragment goes on to the
 * colour buffer, after updating the depth and stencil buffer.
 */
static bool ati_depth_stencil(ATI3DCtx *c, int x, int y, float z)
{
    ATIVGAState *s = c->s;
    uint32_t off = c->zb_offset + (y * c->zb_pitch + x) * c->zb_bypp;
    uint32_t d, nd, zold, znew;
    uint8_t st, ref = c->st_ref;
    bool pass = true;

    if (off + c->zb_bypp > s->vga.vram_size) {
        return true;
    }
    d = ati_ld(s->vga.vram_ptr, off, c->zb_bypp, c->xm);
    zold = d & c->zmask;
    znew = (uint32_t)((double)clampf(z) * c->zmask + 0.5);
    st = d >> 24;
    nd = d;
    if (c->stencil &&
        !ati_compare(ZS_STENCIL_TEST(c->zs_cntl), ref & c->st_mask,
                     st & c->st_mask)) {
        st = ati_stencil_op(ZS_STENCIL_FAIL(c->zs_cntl), st, ref);
        pass = false;
    } else {
        if (c->z_test) {
            pass = ati_compare(ZS_Z_TEST(c->zs_cntl), znew, zold);
        }
        if (c->stencil) {
            st = ati_stencil_op(pass ? ZS_STENCIL_ZPASS(c->zs_cntl) :
                                       ZS_STENCIL_ZFAIL(c->zs_cntl), st, ref);
        }
        if (pass && c->z_write) {
            nd = (nd & ~c->zmask) | znew;
        }
    }
    if (c->stencil) {
        nd = (nd & ~(c->st_wmask << 24)) | ((st & c->st_wmask) << 24);
    }
    if (nd != d) {
        ati_st(s->vga.vram_ptr, off, c->zb_bypp, c->xm, nd);
    }
    return pass;
}

static void ati_put_fragment(ATI3DCtx *c, int x, int y, float z, float col[4])
{
    ATIVGAState *s = c->s;
    uint32_t off = c->cb_offset + (y * c->cb_pitch + x) * c->cb_bypp;
    uint32_t v, old = 0;
    int i;

    if (off + c->cb_bypp > s->vga.vram_size) {
        return;
    }
    if ((c->pp_cntl & PP_ALPHA_TEST_ENABLE) &&
        !ati_compare((c->pp_misc >> 8) & 7, (int)(col[3] * 255.0f + 0.5f),
                     c->pp_misc & 0xff)) {
        return;
    }
    if ((c->z_test || c->stencil) && !ati_depth_stencil(c, x, y, z)) {
        return;
    }
    if (c->rb3d_cntl & (RB_ALPHA_BLEND_ENABLE | RB_PLANE_MASK_ENABLE |
                        RB_ROP_ENABLE)) {
        old = ati_ld(s->vga.vram_ptr, off, c->cb_bypp, c->xm);
    }
    if (c->rb3d_cntl & RB_ALPHA_BLEND_ENABLE) {
        float dst[4], out[4];
        int sf = (c->blendcntl >> 16) & 0x3f;
        int df = (c->blendcntl >> 24) & 0x3f;
        int fcn = (c->blendcntl >> 12) & 3;

        ati_cb_unpack(c->cb_fmt, old, dst);
        for (i = 0; i < 4; i++) {
            float a = col[i] * ati_blend_factor(sf, col, dst, i);
            float b = dst[i] * ati_blend_factor(df, col, dst, i);

            out[i] = fcn >= 2 ? a - b : a + b;
            out[i] = clampf(out[i]);
        }
        memcpy(col, out, sizeof(out));
    }
    v = ati_cb_pack(c->cb_fmt, col);
    if (c->rb3d_cntl & RB_ROP_ENABLE) {
        v = ati_rop((c->rop >> 8) & 0xf, v, old);
    }
    if (c->rb3d_cntl & RB_PLANE_MASK_ENABLE) {
        v = (v & c->planemask) | (old & ~c->planemask);
    }
    ati_st(s->vga.vram_ptr, off, c->cb_bypp, c->xm, v);
    c->dirty_lo = MIN(c->dirty_lo, off);
    c->dirty_hi = MAX(c->dirty_hi, off + c->cb_bypp);
}

/* Fragments */

/* the values of a fragment, and how its texture coordinates change */
typedef struct ATIFrag {
    float z;
    float c[4];
    float spec[4];
    float tc[ATI_3D_UNITS][2];
    float lod[ATI_3D_UNITS];
} ATIFrag;

static void ati_shade(ATI3DCtx *c, int x, int y, ATIFrag *f)
{
    float tx[ATI_3D_UNITS][4];
    float cur[4];
    int n;

    memcpy(cur, f->c, sizeof(cur));
    for (n = 0; n < c->units; n++) {
        if (c->tex[n].enabled) {
            int r = c->tex[n].route;

            ati_tex_sample(&c->tex[n], f->tc[r][0], f->tc[r][1], f->lod[r],
                           tx[n]);
        } else {
            tx[n][0] = tx[n][1] = tx[n][2] = tx[n][3] = 0.0f;
        }
    }
    if (c->r200) {
        /*
         * The stages work on the registers; R0 starts as texture 0. With
         * no stage, the diffuse colour goes through.
         */
        bool staged = false;

        for (n = 0; n < c->units; n++) {
            if (c->pp_cntl & PP_TEX_BLEND_ENABLE(n)) {
                r200_combine(c, n, tx, f->c);
                staged = true;
            }
        }
        if (staged) {
            for (n = 0; n < 4; n++) {
                cur[n] = clampf(tx[0][n]);
            }
        }
    } else {
        for (n = 0; n < c->units; n++) {
            if (c->pp_cntl & PP_TEX_BLEND_ENABLE(n)) {
                ati_combine(c, n, cur, f->c, f->spec, tx);
            }
        }
    }
    if (c->pp_cntl & PP_SPECULAR_ENABLE) {
        for (n = 0; n < 3; n++) {
            cur[n] = clampf(cur[n] + f->spec[n]);
        }
    }
    if (c->pp_cntl & PP_FOG_ENABLE) {
        float k;

        /* 1 is no fog */
        switch (FOG_SOURCE(c->fog_cntl)) {
        case 0:
            k = 1.0f - f->z;
            break;
        case 2:
            k = f->c[3];
            break;
        default:
            k = f->spec[3];
            break;
        }
        k = clampf(k);
        for (n = 0; n < 3; n++) {
            cur[n] = cur[n] * k + c->fog[n] * (1.0f - k);
        }
    }
    ati_put_fragment(c, x, y, f->z, cur);
}

/* Rasterization */

/*
 * An attribute over the screen: v(x, y) = a + dx * x + dy * y. The colours
 * and texture coordinates are interpolated multiplied by 1/w (rhw), which
 * goes over the screen in a straight line; z goes in a straight line as it
 * is.
 */
typedef struct ATIPlane {
    float a, dx, dy;
} ATIPlane;

typedef struct ATISetup {
    ATIPlane z, rhw;
    ATIPlane attr[ATI_ATTRS];
    float zbias;
} ATISetup;

static float ati_attr(const ATIVertex *v, int i)
{
    return i < 4 ? v->c[i] : i < 8 ? v->spec[i - 4] :
           v->tc[(i - 8) / 3][(i - 8) % 3];
}

static void ati_plane(ATIPlane *p, const ATIVertex *v0, const ATIVertex *v1,
                      const ATIVertex *v2, float l0, float l1, float l2,
                      float idet)
{
    float x1 = v1->x - v0->x, y1 = v1->y - v0->y;
    float x2 = v2->x - v0->x, y2 = v2->y - v0->y;

    p->dx = ((l1 - l0) * y2 - (l2 - l0) * y1) * idet;
    p->dy = ((l2 - l0) * x1 - (l1 - l0) * x2) * idet;
    p->a = l0 - p->dx * v0->x - p->dy * v0->y;
}

static inline float ati_peval(const ATIPlane *p, float x, float y)
{
    return p->a + p->dx * x + p->dy * y;
}

/* the colour each vertex shows with SE_CNTL's shade modes */
static void ati_shade_vertices(ATI3DCtx *c, ATIVertex *o, const ATIVertex **v,
                               int n, const ATIVertex *prov)
{
    int modes[4] = {
        SE_DIFFUSE_SHADE(c->se_cntl), SE_ALPHA_SHADE(c->se_cntl),
        SE_SPECULAR_SHADE(c->se_cntl), SE_FOG_SHADE(c->se_cntl)
    };
    int i, k;

    for (i = 0; i < n; i++) {
        o[i] = *v[i];
        for (k = 0; k < 4; k++) {
            float *dst = k == 3 ? &o[i].spec[3] : k == 2 ? o[i].spec :
                         k == 1 ? &o[i].c[3] : o[i].c;
            const float *src = k == 3 ? &prov->spec[3] : k == 2 ? prov->spec :
                               k == 1 ? &prov->c[3] : prov->c;
            int cnt = k & 1 ? 1 : 3;

            if (modes[k] == SHADE_FLAT) {
                memcpy(dst, src, cnt * sizeof(float));
            } else if (modes[k] == SHADE_SOLID && k < 2) {
                memcpy(dst, k ? &c->solid[3] : c->solid, cnt * sizeof(float));
            }
        }
    }
}

static void ati_frag_eval(ATI3DCtx *c, const ATISetup *st, float px, float py,
                          ATIFrag *f)
{
    float rhw = ati_peval(&st->rhw, px, py);
    float iw = rhw ? 1.0f / rhw : 0.0f;
    int i, n;

    f->z = ati_peval(&st->z, px, py) + st->zbias;
    for (i = 0; i < 4; i++) {
        f->c[i] = clampf(ati_peval(&st->attr[i], px, py) * iw);
        f->spec[i] = clampf(ati_peval(&st->attr[4 + i], px, py) * iw);
    }
    for (n = 0; n < c->units; n++) {
        const ATITexture *t = &c->tex[n];
        const ATIPlane *ps, *pt, *pq;
        float ls, lt, lq, iq, sx, sy, tx_, ty, su, sv;
        int r;

        if (!t->enabled) {
            continue;
        }
        r = t->route;
        ps = &st->attr[8 + r * 3];
        pt = ps + 1;
        pq = ps + 2;
        ls = ati_peval(ps, px, py);
        lt = ati_peval(pt, px, py);
        lq = ati_peval(pq, px, py);
        iq = lq ? 1.0f / lq : 0.0f;
        f->tc[r][0] = ls * iq;
        f->tc[r][1] = lt * iq;
        if (t->levels <= 1 && t->mag_linear == ati_min_linear(t->min)) {
            /* one level and one filter: no level of detail needed */
            f->lod[r] = 0.0f;
            continue;
        }
        /* d(s/q)/dx = (ds/dx * q - s * dq/dx) / q^2, in texels */
        su = t->nonparam ? 1.0f : t->w;
        sv = t->nonparam ? 1.0f : t->h;
        sx = (ps->dx * lq - ls * pq->dx) * iq * iq * su;
        tx_ = (pt->dx * lq - lt * pq->dx) * iq * iq * sv;
        sy = (ps->dy * lq - ls * pq->dy) * iq * iq * su;
        ty = (pt->dy * lq - lt * pq->dy) * iq * iq * sv;
        f->lod[r] = 0.5f * log2f(MAX(MAX(sx * sx + tx_ * tx_,
                                         sy * sy + ty * ty), 1e-12f));
    }
}

static float ati_edge(const ATIVertex *a, const ATIVertex *b, float x, float y)
{
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

/*
 * Pixels on an edge belong to the triangle if it is a top or a left edge.
 * The triangles go clockwise on the screen (y goes down), so a top edge
 * goes to the right and a left edge goes up.
 */
static bool ati_top_left(const ATIVertex *a, const ATIVertex *b)
{
    return (a->y == b->y && b->x > a->x) || b->y < a->y;
}

static void ati_triangle(ATI3DCtx *c, const ATIVertex *v0,
                         const ATIVertex *v1, const ATIVertex *v2)
{
    const ATIVertex *in[3] = { v0, v1, v2 };
    static const int flat_vtx[4] = { 0, 1, 2, 2 };
    ATIVertex sv[3];
    const ATIVertex *a, *b, *cc;
    float area = ati_edge(v0, v1, v2->x, v2->y);
    float off = c->ogl_center ? 0.5f : 0.0f;
    ATISetup st;
    float idet;
    int x0, y0, x1, y1, x, y, i;
    bool tl0, tl1, tl2, cw, front;

    if (area == 0.0f || !isfinite(area)) {
        return;
    }
    /* y goes down, so a positive area is clockwise on the screen */
    cw = area > 0;
    front = (c->se_cntl & SE_FFACE_CCW) ? !cw : cw;
    if (!(front ? SE_FFACE_MODE(c->se_cntl) : SE_BFACE_MODE(c->se_cntl))) {
        return;
    }
    ati_shade_vertices(c, sv, in, 3, in[flat_vtx[SE_FLAT_SHADE_VTX(c->se_cntl)]]);
    a = &sv[0];
    b = &sv[1];
    cc = &sv[2];
    if (area < 0) {
        const ATIVertex *t = b;

        b = cc;
        cc = t;
        area = -area;
    }

    idet = 1.0f / ati_edge(a, b, cc->x, cc->y);
    ati_plane(&st.z, a, b, cc, a->z, b->z, cc->z, idet);
    ati_plane(&st.rhw, a, b, cc, a->rhw, b->rhw, cc->rhw, idet);
    for (i = 0; i < ATI_ATTRS; i++) {
        ati_plane(&st.attr[i], a, b, cc, ati_attr(a, i) * a->rhw,
                  ati_attr(b, i) * b->rhw, ati_attr(cc, i) * cc->rhw, idet);
    }
    st.zbias = 0.0f;
    if (c->se_cntl & SE_ZBIAS_TRI) {
        st.zbias = c->zbias_const +
                   c->zbias_factor * MAX(fabsf(st.z.dx), fabsf(st.z.dy));
    }

    x0 = MAX((int)floorf(MIN(MIN(a->x, b->x), cc->x) - off), c->clip_x0);
    y0 = MAX((int)floorf(MIN(MIN(a->y, b->y), cc->y) - off), c->clip_y0);
    x1 = MIN((int)ceilf(MAX(MAX(a->x, b->x), cc->x) - off), c->clip_x1);
    y1 = MIN((int)ceilf(MAX(MAX(a->y, b->y), cc->y) - off), c->clip_y1);
    tl0 = ati_top_left(b, cc);
    tl1 = ati_top_left(cc, a);
    tl2 = ati_top_left(a, b);

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            float px = x + off, py = y + off;
            float w0 = ati_edge(b, cc, px, py);
            float w1 = ati_edge(cc, a, px, py);
            float w2 = ati_edge(a, b, px, py);
            ATIFrag f;

            if (w0 < 0 || w1 < 0 || w2 < 0 ||
                (w0 == 0 && !tl0) || (w1 == 0 && !tl1) ||
                (w2 == 0 && !tl2)) {
                continue;
            }
            ati_frag_eval(c, &st, px, py, &f);
            ati_shade(c, x, y, &f);
        }
    }
}

/* the fragment of a vertex, for points and lines */
static void ati_vertex_frag(ATI3DCtx *c, const ATIVertex *v, ATIFrag *f)
{
    int n;

    memset(f, 0, sizeof(*f));
    f->z = v->z;
    for (n = 0; n < 4; n++) {
        f->c[n] = clampf(v->c[n]);
        f->spec[n] = clampf(v->spec[n]);
    }
    for (n = 0; n < ATI_3D_UNITS; n++) {
        float q = v->tc[n][2] ? v->tc[n][2] : 1.0f;

        f->tc[n][0] = v->tc[n][0] / q;
        f->tc[n][1] = v->tc[n][1] / q;
    }
}

static bool ati_inside(ATI3DCtx *c, int x, int y)
{
    return x >= c->clip_x0 && x <= c->clip_x1 &&
           y >= c->clip_y0 && y <= c->clip_y1;
}

static void ati_point(ATI3DCtx *c, const ATIVertex *v)
{
    int x = (int)floorf(v->x), y = (int)floorf(v->y);
    ATIFrag f;

    if (ati_inside(c, x, y)) {
        ati_vertex_frag(c, v, &f);
        if (c->se_cntl & SE_ZBIAS_POINT) {
            f.z += c->zbias_const;
        }
        ati_shade(c, x, y, &f);
    }
}

/* a one pixel wide line, with its attributes in a straight line */
static void ati_line(ATI3DCtx *c, const ATIVertex *v0, const ATIVertex *v1)
{
    const ATIVertex *in[2] = { v0, v1 };
    ATIVertex sv[2];
    ATIFrag f0, f1, f;
    float dx = v1->x - v0->x, dy = v1->y - v0->y;
    int steps = (int)ceilf(MAX(fabsf(dx), fabsf(dy)));
    int i, k, n;

    ati_shade_vertices(c, sv, in, 2, v1);
    ati_vertex_frag(c, &sv[0], &f0);
    ati_vertex_frag(c, &sv[1], &f1);
    /* the last pixel is left out, as in GL */
    for (i = 0; i < MAX(steps, 1); i++) {
        float t = steps ? (float)i / steps : 0.0f;
        int x = (int)floorf(v0->x + dx * t), y = (int)floorf(v0->y + dy * t);

        if (!ati_inside(c, x, y)) {
            continue;
        }
        f.z = f0.z + (f1.z - f0.z) * t;
        if (c->se_cntl & SE_ZBIAS_LINE) {
            f.z += c->zbias_const;
        }
        for (k = 0; k < 4; k++) {
            f.c[k] = f0.c[k] + (f1.c[k] - f0.c[k]) * t;
            f.spec[k] = f0.spec[k] + (f1.spec[k] - f0.spec[k]) * t;
        }
        for (n = 0; n < ATI_3D_UNITS; n++) {
            f.tc[n][0] = f0.tc[n][0] + (f1.tc[n][0] - f0.tc[n][0]) * t;
            f.tc[n][1] = f0.tc[n][1] + (f1.tc[n][1] - f0.tc[n][1]) * t;
            f.lod[n] = 0.0f;
        }
        ati_shade(c, x, y, &f);
    }
}

static void ati_vertex_sub_add(ATIVertex *o, const ATIVertex *a,
                               const ATIVertex *b, const ATIVertex *cc)
{
    int i, n;

    /* o = a - b + c, the fourth corner of a rectangle */
    o->x = a->x - b->x + cc->x;
    o->y = a->y - b->y + cc->y;
    o->z = a->z - b->z + cc->z;
    o->rhw = a->rhw;
    for (i = 0; i < 4; i++) {
        o->c[i] = a->c[i] - b->c[i] + cc->c[i];
        o->spec[i] = a->spec[i] - b->spec[i] + cc->spec[i];
    }
    for (n = 0; n < ATI_3D_UNITS; n++) {
        for (i = 0; i < 3; i++) {
            o->tc[n][i] = a->tc[n][i] - b->tc[n][i] + cc->tc[n][i];
        }
    }
}

/*
 * Transform and lighting output: from clip space to the window. The
 * perspective divide is left out for x and y, or for z, when SE_VTE_CNTL
 * says they come already divided, and the viewport is applied where it
 * enables it.
 */
#define R200_VTE_XY_FMT         BIT(8)
#define R200_VTE_Z_FMT          BIT(9)

static bool ati_tcl_project(ATI3DCtx *c, const ATIVertex *in, ATIVertex *out)
{
    float w = in->rhw;
    float iw;
    int i;

    if (!(w > 1e-20f) || !isfinite(w)) {
        return false;
    }
    iw = 1.0f / w;
    *out = *in;
    out->rhw = iw;
    if (!(c->vte_cntl & R200_VTE_XY_FMT)) {
        out->x *= iw;
        out->y *= iw;
    }
    if (!(c->vte_cntl & R200_VTE_Z_FMT)) {
        out->z *= iw;
    }
    for (i = 0; i < 3; i++) {
        float *p = i == 0 ? &out->x : i == 1 ? &out->y : &out->z;

        if (c->vte_cntl & BIT(i * 2)) {
            *p *= c->vport[i * 2];
        }
        if (c->vte_cntl & BIT(i * 2 + 1)) {
            *p += c->vport[i * 2 + 1];
        }
    }
    return isfinite(out->x) && isfinite(out->y) && isfinite(out->z);
}

/* the distance of a clip space vertex inside the near plane */
static float ati_tcl_near(const ATI3DCtx *c, const ATIVertex *v)
{
    /* OpenGL keeps -w <= z, Direct3D 0 <= z; the R100 only does OpenGL */
    return c->r200 && (r3d(c->s, R200_SE_VAP_CNTL) & R200_VAP_DX_CLIP_SPACE) ?
           v->z : v->z + v->rhw;
}

static void ati_vertex_lerp(ATIVertex *out, const ATIVertex *a,
                            const ATIVertex *b, float t)
{
    const float *pa = &a->x, *pb = &b->x;
    float *po = &out->x;
    int i;

    for (i = 0; i < 4 + ATI_ATTRS; i++) {
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
    }
}

static void ati_prim_tri(ATI3DCtx *c, const ATIVertex *v0,
                         const ATIVertex *v1, const ATIVertex *v2)
{
    const ATIVertex *in[3] = { v0, v1, v2 };
    ATIVertex poly[4], win[4];
    int i, n = 0;

    if (!c->tcl) {
        ati_triangle(c, v0, v1, v2);
        return;
    }
    /* clip against the near plane: a triangle becomes up to a quad */
    for (i = 0; i < 3; i++) {
        const ATIVertex *a = in[i], *b = in[(i + 1) % 3];
        float da = ati_tcl_near(c, a), db = ati_tcl_near(c, b);

        if (da >= 0) {
            poly[n++] = *a;
        }
        if ((da >= 0) != (db >= 0)) {
            ati_vertex_lerp(&poly[n++], a, b, da / (da - db));
        }
    }
    for (i = 0; i < n; i++) {
        if (!ati_tcl_project(c, &poly[i], &win[i])) {
            return;
        }
    }
    for (i = 1; i + 1 < n; i++) {
        ati_triangle(c, &win[0], &win[i], &win[i + 1]);
    }
}

static void ati_prim_line(ATI3DCtx *c, const ATIVertex *v0,
                          const ATIVertex *v1)
{
    ATIVertex a, b;

    if (!c->tcl) {
        ati_line(c, v0, v1);
    } else if (ati_tcl_project(c, v0, &a) && ati_tcl_project(c, v1, &b)) {
        ati_line(c, &a, &b);
    }
}

static void ati_prim_point(ATI3DCtx *c, const ATIVertex *v)
{
    ATIVertex a;

    if (!c->tcl) {
        ati_point(c, v);
    } else if (ati_tcl_project(c, v, &a)) {
        ati_point(c, &a);
    }
}

static void ati_prims(ATI3DCtx *c, int prim, ATIVertex *v, int n)
{
    int i;

    switch (prim) {
    case 0: /* none */
        break;
    case 1: /* point list */
        for (i = 0; i < n; i++) {
            ati_prim_point(c, &v[i]);
        }
        break;
    case 2: /* line list */
        for (i = 0; i + 1 < n; i += 2) {
            ati_prim_line(c, &v[i], &v[i + 1]);
        }
        break;
    case 3: /* line strip */
        for (i = 0; i + 1 < n; i++) {
            ati_prim_line(c, &v[i], &v[i + 1]);
        }
        break;
    case 4: /* triangle list */
        for (i = 0; i + 2 < n; i += 3) {
            ati_prim_tri(c, &v[i], &v[i + 1], &v[i + 2]);
        }
        break;
    case 5: /* triangle fan */
    case 15: /* polygon */
        for (i = 1; i + 1 < n; i++) {
            ati_prim_tri(c, &v[0], &v[i], &v[i + 1]);
        }
        break;
    case 6: /* triangle strip: every other one turns the other way */
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) {
                ati_prim_tri(c, &v[i + 1], &v[i], &v[i + 2]);
            } else {
                ati_prim_tri(c, &v[i], &v[i + 1], &v[i + 2]);
            }
        }
        break;
    case 8: /* rectangle list: three corners each */
        for (i = 0; i + 2 < n; i += 3) {
            ATIVertex d;

            ati_vertex_sub_add(&d, &v[i], &v[i + 1], &v[i + 2]);
            ati_prim_tri(c, &v[i], &v[i + 1], &v[i + 2]);
            ati_prim_tri(c, &v[i], &v[i + 2], &d);
        }
        break;
    case 13: /* quad list */
        for (i = 0; i + 3 < n; i += 4) {
            ati_prim_tri(c, &v[i], &v[i + 1], &v[i + 2]);
            ati_prim_tri(c, &v[i], &v[i + 2], &v[i + 3]);
        }
        break;
    case 14: /* quad strip */
        for (i = 0; i + 3 < n; i += 2) {
            ati_prim_tri(c, &v[i], &v[i + 1], &v[i + 3]);
            ati_prim_tri(c, &v[i], &v[i + 3], &v[i + 2]);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ati_3d: primitive type %d\n", prim);
        break;
    }
}

/* Vertices */

static int ati_vtx_dwords(uint32_t fmt)
{
    int n = 2;

    n += !!(fmt & VTX_Z) + !!(fmt & VTX_W0);
    n += (fmt & VTX_FPCOLOR ? 3 : 0) + !!(fmt & VTX_FPALPHA) +
         !!(fmt & VTX_PKCOLOR);
    n += (fmt & VTX_FPSPEC ? 3 : 0) + !!(fmt & VTX_FPFOG) +
         !!(fmt & VTX_PKSPEC);
    n += (fmt & VTX_ST0 ? 2 : 0) + !!(fmt & VTX_Q0);
    n += (fmt & VTX_ST1 ? 2 : 0) + !!(fmt & VTX_Q1);
    n += (fmt & VTX_ST2 ? 2 : 0) + !!(fmt & VTX_Q2);
    n += (fmt & VTX_ST3 ? 2 : 0) + !!(fmt & VTX_Q3);
    n += fmt & VTX_N0 ? 3 : 0;
    n += VTX_WEIGHTS(fmt);
    return n;
}

/*
 * A packed colour is four bytes in memory order: B, G, R, A by default
 * (the dword is ARGB), or R, G, B, A with VF_COLOR_ORDER_RGBA (the dword
 * is ABGR), as Mesa's r100 and r200 drivers send it.
 */
static void ati_unpack_color(uint32_t p, bool rgba, float out[4])
{
    if (rgba) {
        out[0] = (p & 0xff) / 255.0f;
        out[1] = ((p >> 8) & 0xff) / 255.0f;
        out[2] = ((p >> 16) & 0xff) / 255.0f;
        out[3] = (p >> 24) / 255.0f;
    } else {
        argb_to_float(p, out);
    }
}

static void ati_vtx_parse(ATI3DCtx *c, uint32_t fmt, uint32_t vf,
                          const uint32_t *d, ATIVertex *v)
{
    ATIVGAState *s = c->s;
    uint32_t cf = c->coord_fmt;
    int i = 0, n;

    memset(v, 0, sizeof(*v));
    v->x = u2f(d[i++]);
    v->y = u2f(d[i++]);
    if (fmt & VTX_Z) {
        v->z = u2f(d[i++]);
    }
    /* W0 is 1/w unless SE_COORD_FMT says it is w */
    v->rhw = 1.0f;
    if (fmt & VTX_W0) {
        float w = u2f(d[i++]);

        v->rhw = cf & CF_W0_IS_NOT_1_OVER_W0 ? (w ? 1.0f / w : 1.0f) : w;
        if (!v->rhw || !isfinite(v->rhw)) {
            v->rhw = 1.0f;
        }
    }
    v->c[0] = v->c[1] = v->c[2] = v->c[3] = 1.0f;
    if (fmt & VTX_FPCOLOR) {
        v->c[0] = u2f(d[i++]);
        v->c[1] = u2f(d[i++]);
        v->c[2] = u2f(d[i++]);
    }
    if (fmt & VTX_FPALPHA) {
        v->c[3] = u2f(d[i++]);
    }
    if (fmt & VTX_PKCOLOR) {
        ati_unpack_color(d[i++], vf & VF_COLOR_ORDER_RGBA, v->c);
    }
    trace_ati_3d_vertex(v->x, v->y, fmt, vf, v->c[0] * 255, v->c[1] * 255,
                        v->c[2] * 255, v->c[3] * 255);
    /* the specular alpha is the fog factor; with no fog there is none */
    v->spec[3] = 1.0f;
    if (fmt & VTX_FPSPEC) {
        v->spec[0] = u2f(d[i++]);
        v->spec[1] = u2f(d[i++]);
        v->spec[2] = u2f(d[i++]);
    }
    if (fmt & VTX_FPFOG) {
        v->spec[3] = u2f(d[i++]);
    }
    if (fmt & VTX_PKSPEC) {
        ati_unpack_color(d[i++], vf & VF_COLOR_ORDER_RGBA, v->spec);
    }
    for (n = 0; n < 4; n++) {
        static const uint32_t st[4] = { VTX_ST0, VTX_ST1, VTX_ST2, VTX_ST3 };
        static const uint32_t q[4] = { VTX_Q0, VTX_Q1, VTX_Q2, VTX_Q3 };
        float ss = 0, tt = 0, qq = 1.0f;

        if (fmt & st[n]) {
            ss = u2f(d[i++]);
            tt = u2f(d[i++]);
        }
        if (fmt & q[n]) {
            qq = u2f(d[i++]);
        }
        /* pre-multiplied by 1/w: the plain values are these times w */
        if (cf & CF_ST_PRE_MULT(n)) {
            ss /= v->rhw;
            tt /= v->rhw;
            qq /= v->rhw;
        }
        if (n < ATI_3D_UNITS) {
            v->tc[n][0] = ss;
            v->tc[n][1] = tt;
            v->tc[n][2] = qq;
        }
    }
    if (!(cf & CF_XY_PRE_MULT)) {
        v->x *= v->rhw;
        v->y *= v->rhw;
    }
    if (!(cf & CF_Z_PRE_MULT)) {
        v->z *= v->rhw;
    }
    if (c->se_cntl & SE_VPORT_XY_XFORM && r3d(s, R3D_VPORT_XSCALE)) {
        v->x = v->x * u2f(r3d(s, R3D_VPORT_XSCALE)) +
               u2f(r3d(s, R3D_VPORT_XOFFSET));
        v->y = v->y * u2f(r3d(s, R3D_VPORT_YSCALE)) +
               u2f(r3d(s, R3D_VPORT_YOFFSET));
    }
    if (c->se_cntl & SE_VPORT_Z_XFORM && r3d(s, R3D_VPORT_ZSCALE)) {
        v->z = v->z * u2f(r3d(s, R3D_VPORT_ZSCALE)) +
               u2f(r3d(s, R3D_VPORT_ZOFFSET));
    }
}

/* R200 vertices: SE_VTX_FMT_0 and SE_VTX_FMT_1 */
static int r200_vtx_dwords(uint32_t fmt0, uint32_t fmt1)
{
    static const int color[4] = { 0, 1, 3, 4 };
    int n = 2, i;

    n += !!(fmt0 & R200_VTX_Z0) + !!(fmt0 & R200_VTX_W0);
    n += R200_VTX_WEIGHTS(fmt0);
    n += fmt0 & R200_VTX_N0 ? 3 : 0;
    n += !!(fmt0 & R200_VTX_POINT_SIZE) + !!(fmt0 & R200_VTX_DISCRETE_FOG);
    n += !!(fmt0 & R200_VTX_SHININESS_0) + !!(fmt0 & R200_VTX_SHININESS_1);
    for (i = 0; i < 8; i++) {
        n += color[R200_VTX_COLOR(fmt0, i)];
    }
    for (i = 0; i < 6; i++) {
        n += (fmt1 >> (i * 3)) & 7;
    }
    return n;
}

static void r200_vtx_parse(uint32_t fmt0, uint32_t fmt1, uint32_t vf,
                           const uint32_t *d, ATIVertex *v)
{
    int i = 0, n, k;

    memset(v, 0, sizeof(*v));
    v->x = u2f(d[i++]);
    v->y = u2f(d[i++]);
    if (fmt0 & R200_VTX_Z0) {
        v->z = u2f(d[i++]);
    }
    v->rhw = 1.0f;
    if (fmt0 & R200_VTX_W0) {
        float w = u2f(d[i++]);

        v->rhw = w ? 1.0f / w : 1.0f;
    }
    i += R200_VTX_WEIGHTS(fmt0);
    i += fmt0 & R200_VTX_N0 ? 3 : 0;
    i += !!(fmt0 & R200_VTX_POINT_SIZE);
    v->spec[3] = 1.0f;
    if (fmt0 & R200_VTX_DISCRETE_FOG) {
        v->spec[3] = u2f(d[i++]);
    }
    i += !!(fmt0 & R200_VTX_SHININESS_0) + !!(fmt0 & R200_VTX_SHININESS_1);
    v->c[0] = v->c[1] = v->c[2] = v->c[3] = 1.0f;
    for (n = 0; n < 8; n++) {
        float *dst = n == 0 ? v->c : n == 1 ? v->spec : NULL;

        switch (R200_VTX_COLOR(fmt0, n)) {
        case 1: { /* packed */
            uint32_t p = d[i++];
            float tmp[4];

            if (dst) {
                ati_unpack_color(p, vf & VF_COLOR_ORDER_RGBA, tmp);
                memcpy(dst, tmp, (n ? 3 : 4) * sizeof(float));
            }
            break;
        }
        case 2: /* float RGB */
        case 3: /* float RGBA */
            for (k = 0; k < (R200_VTX_COLOR(fmt0, n) == 3 ? 4 : 3); k++) {
                float f = u2f(d[i++]);
                /* without VF_COLOR_ORDER_RGBA blue comes first */
                int ch = k < 3 && !(vf & VF_COLOR_ORDER_RGBA) ? 2 - k : k;

                if (dst && (n == 0 || ch < 3)) {
                    dst[ch] = f;
                }
            }
            break;
        }
    }
    for (n = 0; n < 6; n++) {
        int cnt = (fmt1 >> (n * 3)) & 7;

        v->tc[n][2] = 1.0f;
        for (k = 0; k < cnt; k++) {
            float f = u2f(d[i++]);

            if (k < 3) {
                v->tc[n][k] = f;
            }
        }
    }
}

static bool ati_3d_setup(ATIVGAState *s, ATI3DCtx *c)
{
    uint32_t top_left = r3d(s, R3D_RE_TOP_LEFT);
    uint32_t wh = r3d(s, R3D_RE_WIDTH_HEIGHT);
    uint32_t pitch = r3d(s, R3D_COLORPITCH);
    uint32_t refmask;
    int n;

    memset(c, 0, sizeof(*c));
    c->s = s;
    c->r200 = ati_is_r200(s);
    c->units = c->r200 ? ATI_3D_UNITS : R100_UNITS;
    c->rb3d_cntl = r3d(s, R3D_CNTL);
    c->cb_fmt = RB_COLOR_FORMAT(c->rb3d_cntl);
    switch (c->cb_fmt) {
    case 3:
    case 4:
    case 15:
        c->cb_bypp = 2;
        break;
    case 6:
        c->cb_bypp = 4;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ati_3d: colour format %d\n", c->cb_fmt);
        return false;
    }
    c->cb_offset = ati_fb_offset(s, r3d(s, R3D_COLOROFFSET) & ~0xf);
    c->xm = ati_vram_xm(s, c->cb_offset);
    c->cb_pitch = pitch & 0x1ff8;
    if (!c->cb_pitch) {
        return false;
    }
    c->clip_x0 = top_left & 0x7ff;
    c->clip_y0 = (top_left >> 16) & 0x7ff;
    c->clip_x1 = wh & 0x7ff;
    c->clip_y1 = (wh >> 16) & 0x7ff;
    if (!wh) {
        c->clip_x1 = c->clip_y1 = 0x7ff;
    }
    c->blendcntl = r3d(s, R3D_BLENDCNTL);
    c->planemask = r3d(s, R3D_PLANEMASK);
    c->rop = r3d(s, R3D_ROPCNTL);
    /* on the R200, 0x2cc0 is PP_CNTL too (see ati_r200_alias) */
    c->pp_cntl = r3d(s, R3D_PP_CNTL);
    c->pp_misc = r3d(s, R3D_PP_MISC);
    c->se_cntl = r3d(s, R3D_SE_CNTL);
    c->coord_fmt = r3d(s, R3D_SE_COORD_FMT);
    c->fog_cntl = r3d(s, R3D_PP_FOG_COLOR);
    argb_to_float(c->fog_cntl, c->fog);
    argb_to_float(r3d(s, R3D_RE_SOLID_COLOR), c->solid);
    c->ogl_center = c->se_cntl & SE_PIX_CENTER_OGL;

    /* the depth and stencil buffer */
    c->zs_cntl = r3d(s, R3D_ZSTENCILCNTL);
    if (c->rb3d_cntl & (RB_Z_ENABLE | RB_STENCIL_ENABLE)) {
        int zf = ZS_DEPTH_FORMAT(c->zs_cntl);

        c->zb_offset = ati_fb_offset(s, r3d(s, R3D_DEPTHOFFSET) & ~0xf);
        c->zb_pitch = r3d(s, R3D_DEPTHPITCH) & 0x1ff8;
        c->zb_bypp = zf == 0 || zf == 7 ? 2 : 4;
        c->zmask = zf == 0 || zf == 7 ? 0xffff :
                   zf == 2 || zf == 3 || zf == 9 ? 0xffffff : 0xffffffff;
        if (c->zb_pitch) {
            c->z_test = c->rb3d_cntl & RB_Z_ENABLE;
            c->z_write = c->z_test && (c->zs_cntl & ZS_Z_WRITE_ENABLE);
            /* only the 24 bit formats keep a stencil in the top byte */
            c->stencil = (c->rb3d_cntl & RB_STENCIL_ENABLE) &&
                         c->zmask == 0xffffff;
        }
        refmask = r3d(s, R3D_STENCILREFMASK);
        c->st_ref = refmask & 0xff;
        c->st_mask = (refmask >> 16) & 0xff;
        c->st_wmask = (refmask >> 24) & 0xff;
    }
    c->zbias_factor = u2f(r3d(s, R3D_SE_ZBIAS_FACTOR));
    c->zbias_const = u2f(r3d(s, R3D_SE_ZBIAS_CONSTANT));
    if (!isfinite(c->zbias_factor)) {
        c->zbias_factor = 0.0f;
    }
    if (!isfinite(c->zbias_const)) {
        c->zbias_const = 0.0f;
    }

    c->dirty_lo = UINT32_MAX;
    for (n = 0; n < c->units; n++) {
        if (c->pp_cntl & PP_TEX_ENABLE(n)) {
            ati_tex_setup(c, n);
        }
    }
    return true;
}

static void ati_3d_finish(ATI3DCtx *c)
{
    int n;

    for (n = 0; n < ATI_3D_UNITS; n++) {
        g_free(c->tex[n].copy);
    }
    if (c->dirty_lo < c->dirty_hi) {
        memory_region_set_dirty(&c->s->vga.vram, c->dirty_lo,
                                c->dirty_hi - c->dirty_lo);
    }
}

/*
 * Draws vertices; fmt and vf as SE_VTX_FMT (SE_VTX_FMT_0 on R200) and
 * SE_VF_CNTL. The n dwords at d hold the vertices one after another, or,
 * with idx, the vertices that the 16 bit indices at idx pick.
 */
static bool r100_tcl_on(ATIVGAState *s, uint32_t vf);
static void r100_tcl_draw(ATIVGAState *s, uint32_t fmt, uint32_t vf,
                          const uint32_t *d, uint32_t n, const uint32_t *idx,
                          uint32_t nidx);

static void ati_3d_draw(ATIVGAState *s, uint32_t fmt, uint32_t vf,
                        const uint32_t *d, uint32_t n, const uint32_t *idx,
                        uint32_t nidx)
{
    ATI3DCtx c;
    ATIVertex *v;
    bool r200 = ati_is_r200(s);
    uint32_t fmt1 = r3d(s, R200_SE_VTX_FMT_1);
    int size = r200 ? r200_vtx_dwords(fmt, fmt1) : ati_vtx_dwords(fmt);
    int count = idx ? MIN(VF_NUM_VERTICES(vf), nidx * 2) :
                      MIN(VF_NUM_VERTICES(vf), n / size);
    int i, k = 0;

    if (!r200 && r100_tcl_on(s, vf)) {
        r100_tcl_draw(s, fmt, vf, d, n, idx, nidx);
        return;
    }
    if (!count || !ati_3d_setup(s, &c)) {
        return;
    }
    count = MIN(count, ATI_3D_MAX_VERTS);
    v = g_new(ATIVertex, count);
    for (i = 0; i < count; i++) {
        uint32_t j = i;

        if (idx) {
            /* two indices in a dword, the first in the low half */
            j = (idx[i / 2] >> ((i & 1) * 16)) & 0xffff;
            if ((j + 1) * size > n) {
                continue;
            }
        }
        if (r200) {
            r200_vtx_parse(fmt, fmt1, vf, d + j * size, &v[k++]);
        } else {
            ati_vtx_parse(&c, fmt, vf, d + j * size, &v[k++]);
        }
    }
    count = k;
    trace_ati_3d_draw(VF_PRIM(vf), count, fmt, c.tex[0].enabled);
    ati_prims(&c, VF_PRIM(vf), v, count);
    g_free(v);
    ati_3d_finish(&c);
}

static void ati_3d_draw_vertices(ATIVGAState *s, uint32_t fmt, uint32_t vf,
                                 const uint32_t *d, uint32_t n)
{
    if (VF_WALK(vf) == VF_WALK_IND) {
        qemu_log_mask(LOG_GUEST_ERROR, "ati_3d: indices without a buffer\n");
        return;
    }
    ati_3d_draw(s, fmt, vf, d, n, NULL, 0);
}

/*
 * 3D_RNDR_GEN_INDX_PRIM: the address of a vertex buffer, its number of
 * vertices, the vertex format and VF_CNTL, then the indices if VF_CNTL
 * walks them. Without indices the vertices are drawn in order.
 */
static void ati_3d_draw_buffer(ATIVGAState *s, const uint32_t *d, uint32_t n)
{
    uint32_t fmt = d[2], vf = d[3];
    bool r200 = ati_is_r200(s);
    int size = r200 ? r200_vtx_dwords(fmt, r3d(s, R200_SE_VTX_FMT_1)) :
                      ati_vtx_dwords(fmt);
    uint32_t nvtx = MIN(d[1], ATI_3D_MAX_VERTS);
    uint32_t *buf, start;
    int i, xm = 0;

    if (!nvtx) {
        return;
    }
    buf = g_try_new(uint32_t, nvtx * size);
    if (!buf) {
        return;
    }
    ati_gpu_rw(s, d[0], buf, nvtx * size * 4, false);
    /* a buffer in video memory is stored as the CPU sees it */
    if (ati_gpu_vram_offset(s, d[0], &start)) {
        xm = ati_vram_xm(s, start);
    }
    for (i = 0; i < nvtx * size; i++) {
        buf[i] = ati_ld((const uint8_t *)buf, i * 4, 4, xm);
    }
    if (VF_WALK(vf) == VF_WALK_IND) {
        ati_3d_draw(s, fmt, vf, buf, nvtx * size, d + 4, n - 4);
    } else {
        ati_3d_draw(s, fmt, vf, buf, nvtx * size, NULL, 0);
    }
    g_free(buf);
}

/*
 * R200 transform and lighting (TCL), the fixed function of OpenGL 1.x:
 * TinyGL on MorphOS draws through it. The driver loads matrices, lights
 * and materials into the vector and scalar memories through
 * SE_TCL_VECTOR/SCALAR_INDX_REG and _DATA_REG, points 3D_LOAD_VBPNTR at
 * its vertex arrays and draws them with 3D_DRAW_VBUF_2 or 3D_DRAW_INDX_2.
 */

#define R200_SE_TCL_MATRIX_SEL(n)       (0x2230 + (n) * 4)
#define R200_SE_TCL_OUTPUT_VTX_FMT_1    0x2094
#define R200_SE_TCL_LIGHT_MODEL_CTL_0   0x2268
#define R200_SE_TCL_LIGHT_MODEL_CTL_1   0x226c
#define R200_SE_TCL_PER_LIGHT_CTL(n)    (0x2270 + ((n) / 2) * 4)
#define R200_SE_TCL_TEX_PROC_CTL_0      0x22b0
#define R200_SE_TCL_UCP_VERT_BLEND_CTL  0x22c0
#define R200_SE_VPORT_XSCALE            0x1d98

/* LIGHT_MODEL_CTL_0 */
#define R200_LIGHTING_ENABLE            BIT(0)
#define R200_LOCAL_VIEWER               BIT(2)
#define R200_NORMALIZE_NORMALS          BIT(3)
#define R200_SPECULAR_LIGHTS            BIT(5)
#define R200_DIFFUSE_SPECULAR_COMBINE   BIT(6)

/* PER_LIGHT_CTL, for each light */
#define R200_LIGHT_ENABLE               BIT(0)
#define R200_LIGHT_ENABLE_AMBIENT       BIT(1)
#define R200_LIGHT_ENABLE_SPECULAR      BIT(2)
#define R200_LIGHT_IS_LOCAL             BIT(3)
#define R200_LIGHT_IS_SPOT              BIT(4)
#define R200_LIGHT_RANGE_ATTEN          BIT(6)
#define R200_LIGHT_CONSTANT_RANGE_ATTEN BIT(7)

/* the vector memory */
#define R200_VS_LIGHT_AMBIENT           0x28
#define R200_VS_LIGHT_DIFFUSE           0x30
#define R200_VS_LIGHT_SPECULAR          0x38
#define R200_VS_LIGHT_DIRPOS            0x40
#define R200_VS_LIGHT_HWVSPOT           0x48
#define R200_VS_LIGHT_ATTENUATION       0x50
#define R200_VS_GLOBAL_AMBIENT          0x5c
#define R200_VS_MATRIX(n)               (0x80 + (n) * 4)
#define R200_VS_MAT_EMISS(m)            (0xb0 + (m) * 4)
/* the scalar memory */
#define R200_SS_LIGHT_SPOT_EXPONENT     0x10
#define R200_SS_LIGHT_SPOT_CUTOFF       0x18
#define R200_SS_LIGHT_ATT_CONST_INV     0x30
#define R200_SS_MAT_SHININESS(m)        (0x100 + (m))

void ati_tcl_write(ATIVGAState *s, uint32_t addr, uint32_t val)
{
    ATITCLState *t = &s->tcl;

    switch (addr) {
    case 0x2200: /* SE_TCL_VECTOR_INDX_REG */
        t->vec_idx = val & 0xffff;
        t->vec_stride = (val >> 16) & 0xff;
        t->vec_dwords = (val >> 28) ? (val >> 28) : 4;
        t->vec_dw = 0;
        break;
    case 0x2204: /* SE_TCL_VECTOR_DATA_REG */
        if (t->vec_idx < ATI_TCL_VECTORS && t->vec_dw < 4) {
            t->vec[t->vec_idx][t->vec_dw] = u2f(val);
        }
        if (++t->vec_dw >= t->vec_dwords) {
            t->vec_dw = 0;
            t->vec_idx += t->vec_stride;
        }
        break;
    case 0x2208: /* SE_TCL_SCALAR_INDX_REG */
        t->scl_idx = val & 0xffff;
        t->scl_stride = (val >> 16) & 0xff;
        break;
    case 0x220c: /* SE_TCL_SCALAR_DATA_REG */
        if (t->scl_idx < ATI_TCL_SCALARS) {
            t->scl[t->scl_idx] = u2f(val);
        }
        t->scl_idx += t->scl_stride;
        break;
    }
}

/* a vertex as it goes into the TCL */
typedef struct R200TclIn {
    float pos[4];
    float nrm[3];
    float col[2][4];
    float tex[6][4];
} R200TclIn;

static void r200_tcl_in(uint32_t fmt0, uint32_t fmt1, uint32_t vf,
                        const uint32_t *d, R200TclIn *in)
{
    int i = 0, n, k;

    in->pos[0] = u2f(d[i++]);
    in->pos[1] = u2f(d[i++]);
    in->pos[2] = fmt0 & R200_VTX_Z0 ? u2f(d[i++]) : 0.0f;
    in->pos[3] = fmt0 & R200_VTX_W0 ? u2f(d[i++]) : 1.0f;
    i += R200_VTX_WEIGHTS(fmt0);
    in->nrm[0] = 0.0f;
    in->nrm[1] = 0.0f;
    in->nrm[2] = 1.0f;
    if (fmt0 & R200_VTX_N0) {
        for (k = 0; k < 3; k++) {
            in->nrm[k] = u2f(d[i++]);
        }
    }
    i += !!(fmt0 & R200_VTX_POINT_SIZE) + !!(fmt0 & R200_VTX_DISCRETE_FOG);
    i += !!(fmt0 & R200_VTX_SHININESS_0) + !!(fmt0 & R200_VTX_SHININESS_1);
    for (n = 0; n < 2; n++) {
        for (k = 0; k < 4; k++) {
            in->col[n][k] = n ? 0.0f : 1.0f;
        }
    }
    for (n = 0; n < 8; n++) {
        float tmp[4] = { 0, 0, 0, 1 };

        switch (R200_VTX_COLOR(fmt0, n)) {
        case 1:
            ati_unpack_color(d[i++], vf & VF_COLOR_ORDER_RGBA, tmp);
            break;
        case 2:
        case 3:
            for (k = 0; k < (R200_VTX_COLOR(fmt0, n) == 3 ? 4 : 3); k++) {
                /* without VF_COLOR_ORDER_RGBA blue comes first */
                tmp[k < 3 && !(vf & VF_COLOR_ORDER_RGBA) ? 2 - k : k] =
                    u2f(d[i++]);
            }
            break;
        default:
            continue;
        }
        if (n < 2) {
            memcpy(in->col[n], tmp, sizeof(tmp));
        }
    }
    for (n = 0; n < 6; n++) {
        int cnt = (fmt1 >> (n * 3)) & 7;

        in->tex[n][0] = in->tex[n][1] = in->tex[n][2] = 0.0f;
        in->tex[n][3] = 1.0f;
        for (k = 0; k < cnt; k++) {
            float f = u2f(d[i++]);

            if (k < 4) {
                in->tex[n][k] = f;
            }
        }
    }
}

/* one of the matrices: each vector of it is a row */
static void r200_tcl_mul(ATIVGAState *s, int mtx, const float in[4],
                         float out[4])
{
    const float (*m)[4] = &s->tcl.vec[R200_VS_MATRIX(mtx & 0xf)];
    int i;

    for (i = 0; i < 4; i++) {
        out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2] +
                 m[i][3] * in[3];
    }
}

static float dot3(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void normalize3(float *v)
{
    float l = sqrtf(dot3(v, v));

    if (l > 1e-20f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

/*
 * The source of a material colour (LIGHT_MODEL_CTL_1): the light colours
 * premultiplied, material 0 or 1, or a vertex colour.
 */
static const float *r200_tcl_src(ATIVGAState *s, int src, int which,
                                 const R200TclIn *in)
{
    static const float one[4] = { 1, 1, 1, 1 };

    switch (src) {
    case 0:
        return one;
    case 1:
        return s->tcl.vec[R200_VS_MAT_EMISS(0) + which];
    case 2 ... 3:
        return in->col[src - 2];
    case 0xf:
        return s->tcl.vec[R200_VS_MAT_EMISS(1) + which];
    default:
        return one;
    }
}

/*
 * Where a chip keeps its lights: the first vector or scalar of each array
 * (light n is at that plus n), and the light model.
 */
typedef struct ATITclLights {
    uint32_t lm;                /* LIGHT_MODEL_CTL(_0): the bits both share */
    const float *emis, *amb, *dif, *spc;    /* the material colours */
    const float *gamb;
    float shin;
    int vs_amb, vs_dif, vs_spc, vs_pos, vs_dir, vs_att;
    int ss_spot_exp, ss_spot_cut, ss_const_inv;
    int ss_range;               /* the range cutoff, or -1 */
} ATITclLights;

/*
 * Lighting of the front face: OpenGL's colour sum. SE_TCL_PER_LIGHT_CTL_0
 * to _3 are at the same addresses and have the same bits on both chips.
 */
static void ati_tcl_light(ATIVGAState *s, const ATITclLights *L,
                          const float eye[4], const float nrm[3],
                          float col[4], float spec[4])
{
    float v[3] = { 0, 0, 1 };
    int n, k;

    if (L->lm & R200_LOCAL_VIEWER) {
        v[0] = -eye[0];
        v[1] = -eye[1];
        v[2] = -eye[2];
        normalize3(v);
    }
    for (k = 0; k < 3; k++) {
        col[k] = L->emis[k] + L->amb[k] * L->gamb[k];
        spec[k] = 0.0f;
    }
    col[3] = L->dif[3];
    for (n = 0; n < 8; n++) {
        uint32_t pl = r3d(s, R200_SE_TCL_PER_LIGHT_CTL(n)) >> ((n & 1) * 16);
        const float *lamb = s->tcl.vec[L->vs_amb + n];
        const float *ldif = s->tcl.vec[L->vs_dif + n];
        const float *lspc = s->tcl.vec[L->vs_spc + n];
        const float *pos = s->tcl.vec[L->vs_pos + n];
        const float *dir = s->tcl.vec[L->vs_dir + n];
        const float *att = s->tcl.vec[L->vs_att + n];
        float l[3], h[3], f = 1.0f, ndl, ndh;

        if (!(pl & R200_LIGHT_ENABLE)) {
            continue;
        }
        if (pl & R200_LIGHT_IS_LOCAL) {
            float d2, d;

            for (k = 0; k < 3; k++) {
                l[k] = pos[k] - eye[k];
            }
            d2 = dot3(l, l);
            d = sqrtf(d2);
            normalize3(l);
            if (L->ss_range >= 0 && d > s->tcl.scl[L->ss_range + n]) {
                continue;
            }
            if (pl & R200_LIGHT_RANGE_ATTEN) {
                if (pl & R200_LIGHT_CONSTANT_RANGE_ATTEN) {
                    f = s->tcl.scl[L->ss_const_inv + n];
                } else {
                    float q = att[2] + att[1] * d + att[0] * d2;

                    f = q > 1e-20f ? 1.0f / q : 1.0f;
                }
            }
            if (pl & R200_LIGHT_IS_SPOT) {
                /* the direction holds minus the spot direction */
                float cs = dot3(l, dir);

                f *= cs >= s->tcl.scl[L->ss_spot_cut + n] ?
                     powf(MAX(cs, 0.0f), s->tcl.scl[L->ss_spot_exp + n]) :
                     0.0f;
            }
            for (k = 0; k < 3; k++) {
                h[k] = l[k] + v[k];
            }
            normalize3(h);
        } else {
            /* the direction to the light and the half vector, normalized */
            memcpy(l, pos, sizeof(l));
            if (L->lm & R200_LOCAL_VIEWER) {
                for (k = 0; k < 3; k++) {
                    h[k] = l[k] + v[k];
                }
                normalize3(h);
            } else {
                memcpy(h, dir, sizeof(h));
            }
        }
        ndl = dot3(nrm, l);
        for (k = 0; k < 3; k++) {
            float c = 0.0f;

            if (pl & R200_LIGHT_ENABLE_AMBIENT) {
                c += L->amb[k] * lamb[k];
            }
            if (ndl > 0) {
                c += ndl * L->dif[k] * ldif[k];
            }
            col[k] += f * c;
        }
        ndh = dot3(nrm, h);
        if ((L->lm & R200_SPECULAR_LIGHTS) &&
            (pl & R200_LIGHT_ENABLE_SPECULAR) && ndl > 0 && ndh > 0) {
            float p = L->shin > 0 ? powf(ndh, L->shin) : 1.0f;

            for (k = 0; k < 3; k++) {
                spec[k] += f * p * L->spc[k] * lspc[k];
            }
        }
    }
    if (L->lm & R200_DIFFUSE_SPECULAR_COMBINE) {
        for (k = 0; k < 3; k++) {
            col[k] += spec[k];
            spec[k] = 0.0f;
        }
    }
    for (k = 0; k < 4; k++) {
        col[k] = clampf(col[k]);
        spec[k] = clampf(spec[k]);
    }
}

static void r200_tcl_light(ATIVGAState *s, const R200TclIn *in,
                           const float eye[4], const float nrm[3],
                           float col[4], float spec[4])
{
    uint32_t lm1 = r3d(s, R200_SE_TCL_LIGHT_MODEL_CTL_1);
    ATITclLights L = {
        .lm = r3d(s, R200_SE_TCL_LIGHT_MODEL_CTL_0),
        .emis = r200_tcl_src(s, lm1 & 0xf, 0, in),
        .amb = r200_tcl_src(s, (lm1 >> 4) & 0xf, 1, in),
        .dif = r200_tcl_src(s, (lm1 >> 8) & 0xf, 2, in),
        .spc = r200_tcl_src(s, (lm1 >> 12) & 0xf, 3, in),
        .gamb = s->tcl.vec[R200_VS_GLOBAL_AMBIENT],
        .shin = s->tcl.scl[R200_SS_MAT_SHININESS(0)],
        .vs_amb = R200_VS_LIGHT_AMBIENT,
        .vs_dif = R200_VS_LIGHT_DIFFUSE,
        .vs_spc = R200_VS_LIGHT_SPECULAR,
        .vs_pos = R200_VS_LIGHT_DIRPOS,
        .vs_dir = R200_VS_LIGHT_HWVSPOT,
        .vs_att = R200_VS_LIGHT_ATTENUATION,
        .ss_spot_exp = R200_SS_LIGHT_SPOT_EXPONENT,
        .ss_spot_cut = R200_SS_LIGHT_SPOT_CUTOFF,
        .ss_const_inv = R200_SS_LIGHT_ATT_CONST_INV,
        .ss_range = -1,
    };

    ati_tcl_light(s, &L, eye, nrm, col, spec);
}

/* a vertex through the TCL: clip space position in x, y, z and rhw */
static void r200_tcl_vertex(ATIVGAState *s, const R200TclIn *in, ATIVertex *v)
{
    uint32_t sel2 = r3d(s, R200_SE_TCL_MATRIX_SEL(2));
    uint32_t tex0 = r3d(s, R200_SE_TCL_TEX_PROC_CTL_0);
    uint32_t out1 = r3d(s, R200_SE_TCL_OUTPUT_VTX_FMT_1);
    float clip[4];
    int n;

    memset(v, 0, sizeof(*v));
    r200_tcl_mul(s, sel2 & 0xff, in->pos, clip);
    v->x = clip[0];
    v->y = clip[1];
    v->z = clip[2];
    v->rhw = clip[3];
    if (r3d(s, R200_SE_TCL_LIGHT_MODEL_CTL_0) & R200_LIGHTING_ENABLE) {
        float eye[4], n4[4] = { in->nrm[0], in->nrm[1], in->nrm[2], 0 };
        float nrm[4];

        r200_tcl_mul(s, r3d(s, R200_SE_TCL_MATRIX_SEL(0)) & 0xff, in->pos,
                     eye);
        if (eye[3] != 0.0f && eye[3] != 1.0f) {
            eye[0] /= eye[3];
            eye[1] /= eye[3];
            eye[2] /= eye[3];
        }
        r200_tcl_mul(s, r3d(s, R200_SE_TCL_MATRIX_SEL(1)) & 0xff, n4, nrm);
        if (r3d(s, R200_SE_TCL_LIGHT_MODEL_CTL_0) & R200_NORMALIZE_NORMALS) {
            normalize3(nrm);
        }
        r200_tcl_light(s, in, eye, nrm, v->c, v->spec);
    } else {
        memcpy(v->c, in->col[0], sizeof(v->c));
        memcpy(v->spec, in->col[1], 3 * sizeof(float));
    }
    v->spec[3] = 1.0f;
    for (n = 0; n < ATI_3D_UNITS; n++) {
        float tc[4];

        if (!((out1 >> (n * 3)) & 7)) {
            continue;
        }
        memcpy(tc, in->tex[n], sizeof(tc));
        if (tex0 & BIT(8 + n)) {
            /* TEXMAT_n_ENABLE: MATRIX_SEL_3 and _4 pick the matrices */
            uint32_t sel = r3d(s, R200_SE_TCL_MATRIX_SEL(3 + n / 4));
            float out[4];

            r200_tcl_mul(s, (sel >> ((n % 4) * 8)) & 0xff, tc, out);
            memcpy(tc, out, sizeof(tc));
        }
        v->tc[n][0] = tc[0];
        v->tc[n][1] = tc[1];
        v->tc[n][2] = tc[3] != 0.0f ? tc[3] : 1.0f;
    }
}

/* 3D_LOAD_VBPNTR: the number of arrays, then size, stride and address */
static void r200_load_vbpntr(ATIVGAState *s, const uint32_t *d, uint32_t n)
{
    ATITCLState *t = &s->tcl;
    uint32_t i, k = 1;

    if (!n) {
        return;
    }
    t->naos = MIN(d[0] & 0x1f, ATI_AOS_MAX);
    for (i = 0; i < t->naos; i += 2) {
        uint32_t attr;

        if (k >= n) {
            t->naos = i;
            return;
        }
        attr = d[k++];
        t->aos[i].size = attr & 0xff;
        t->aos[i].stride = (attr >> 8) & 0xff;
        t->aos[i].addr = k < n ? d[k++] : 0;
        if (i + 1 < t->naos) {
            t->aos[i + 1].size = (attr >> 16) & 0xff;
            t->aos[i + 1].stride = attr >> 24;
            t->aos[i + 1].addr = k < n ? d[k++] : 0;
        }
    }
}

/* the attributes of a vertex, in the order of its dwords */
typedef struct R200Attr {
    int count;
} R200Attr;

/*
 * The R100's, as the TCL takes them: position, blend weights, normal,
 * colour, specular colour, fog, then the texture coordinates with their q.
 */
static int r100_vtx_attrs(uint32_t fmt, R200Attr *attr)
{
    static const uint32_t st[4] = { VTX_ST0, VTX_ST1, VTX_ST2, VTX_ST3 };
    static const uint32_t q[4] = { VTX_Q0, VTX_Q1, VTX_Q2, VTX_Q3 };
    int n = 0, i;

    attr[n++].count = 2 + !!(fmt & VTX_Z) + !!(fmt & VTX_W0);
    if (VTX_WEIGHTS(fmt)) {
        attr[n++].count = VTX_WEIGHTS(fmt);
    }
    if (fmt & VTX_N0) {
        attr[n++].count = 3;
    }
    if (fmt & (VTX_FPCOLOR | VTX_FPALPHA)) {
        attr[n++].count = (fmt & VTX_FPCOLOR ? 3 : 0) + !!(fmt & VTX_FPALPHA);
    }
    if (fmt & VTX_PKCOLOR) {
        attr[n++].count = 1;
    }
    if (fmt & VTX_FPSPEC) {
        attr[n++].count = 3;
    }
    if (fmt & VTX_FPFOG) {
        attr[n++].count = 1;
    }
    if (fmt & VTX_PKSPEC) {
        attr[n++].count = 1;
    }
    for (i = 0; i < 4; i++) {
        if (fmt & (st[i] | q[i])) {
            attr[n++].count = (fmt & st[i] ? 2 : 0) + !!(fmt & q[i]);
        }
    }
    return n;
}

static int r200_vtx_attrs(uint32_t fmt0, uint32_t fmt1, R200Attr *attr)
{
    static const int color[4] = { 0, 1, 3, 4 };
    int n = 0, i;

    attr[n++].count = 2 + !!(fmt0 & R200_VTX_Z0) + !!(fmt0 & R200_VTX_W0);
    if (R200_VTX_WEIGHTS(fmt0)) {
        attr[n++].count = R200_VTX_WEIGHTS(fmt0);
    }
    if (fmt0 & R200_VTX_N0) {
        attr[n++].count = 3;
    }
    if (fmt0 & R200_VTX_POINT_SIZE) {
        attr[n++].count = 1;
    }
    if (fmt0 & R200_VTX_DISCRETE_FOG) {
        attr[n++].count = 1;
    }
    if (fmt0 & R200_VTX_SHININESS_0) {
        attr[n++].count = 1;
    }
    if (fmt0 & R200_VTX_SHININESS_1) {
        attr[n++].count = 1;
    }
    for (i = 0; i < 8; i++) {
        if (R200_VTX_COLOR(fmt0, i)) {
            attr[n++].count = color[R200_VTX_COLOR(fmt0, i)];
        }
    }
    for (i = 0; i < 6; i++) {
        if ((fmt1 >> (i * 3)) & 7) {
            attr[n++].count = (fmt1 >> (i * 3)) & 7;
        }
    }
    return n;
}

/*
 * 3D_DRAW_VBUF_2 and 3D_DRAW_INDX_2 on the R200, 3D_DRAW_VBUF and
 * 3D_DRAW_INDX with the vertex format fmt on the R100: the vertices are
 * gathered from the arrays, an array for each attribute of the vertex
 * format, with the byte swap of SE_VAP_CNTL_STATUS (SE_CNTL_STATUS on the
 * R100), then go through the TCL if it is on.
 */
static void ati_draw_aos(ATIVGAState *s, uint32_t fmt, uint32_t vf,
                         const uint32_t *idx, uint32_t nidx)
{
    ATITCLState *t = &s->tcl;
    bool r200 = ati_is_r200(s);
    uint32_t fmt0 = r200 ? r3d(s, R200_SE_VTX_FMT_0) : fmt;
    uint32_t fmt1 = r200 ? r3d(s, R200_SE_VTX_FMT_1) : 0;
    int swap = r3d(s, R200_SE_VAP_CNTL_STATUS) & 3;
    uint32_t count = MIN(VF_NUM_VERTICES(vf), ATI_3D_MAX_VERTS);
    uint32_t nvtx = 0, size, i, j, a;
    uint32_t *arr[ATI_AOS_MAX] = { NULL }, *buf;
    R200Attr attr[32];
    int nattr;
    ATI3DCtx c;
    ATIVertex *v;

    if (idx) {
        count = MIN(count, nidx * 2);
    }
    trace_ati_3d_vbuf(vf, fmt0, t->naos,
                      t->aos[0].size | t->aos[0].stride << 8,
                      t->aos[1].size | t->aos[1].stride << 8,
                      t->aos[2].size | t->aos[2].stride << 8,
                      t->aos[3].size | t->aos[3].stride << 8);
    if (!count || !t->naos) {
        return;
    }
    /* how many vertices the arrays must hold */
    for (i = 0; i < count; i++) {
        j = idx ? (idx[i / 2] >> ((i & 1) * 16)) & 0xffff : i;
        nvtx = MAX(nvtx, j + 1);
    }
    for (a = 0; a < t->naos; a++) {
        uint32_t len = ((nvtx - 1) * t->aos[a].stride + t->aos[a].size) * 4;

        arr[a] = g_malloc0(len);
        ati_gpu_rw(s, t->aos[a].addr, arr[a], len, false);
        for (j = 0; j < len / 4; j++) {
            uint32_t w = le32_to_cpu(arr[a][j]);

            arr[a][j] = swap == 2 ? bswap32(w) :
                        swap == 1 ? ((w & 0x00ff00ff) << 8) |
                                    ((w >> 8) & 0x00ff00ff) : w;
        }
    }
    /*
     * Each attribute of SE_VTX_FMT_0 and _1 comes from its own array, with
     * as many components as the array has; the ones it lacks get their
     * defaults (z 0, w 1, alpha 1), as the vertex format expects them.
     */
    nattr = r200 ? r200_vtx_attrs(fmt0, fmt1, attr) :
                   r100_vtx_attrs(fmt0, attr);
    size = r200 ? r200_vtx_dwords(fmt0, fmt1) : ati_vtx_dwords(fmt0);
    buf = g_new0(uint32_t, (size_t)count * size + 64);
    for (i = 0; i < count; i++) {
        uint32_t *dst = buf + (size_t)i * size;

        j = idx ? (idx[i / 2] >> ((i & 1) * 16)) & 0xffff : i;
        for (a = 0; a < nattr; a++) {
            int k, have = a < t->naos ? t->aos[a].size : 0;

            for (k = 0; k < attr[a].count; k++) {
                dst[k] = k < have ? arr[a][j * t->aos[a].stride + k] :
                         f2u(k == 3 ? 1.0f : 0.0f);
            }
            dst += attr[a].count;
        }
    }
    for (a = 0; a < t->naos; a++) {
        g_free(arr[a]);
    }

    if (!r200) {
        /* through the TCL if VF_CNTL and SE_CNTL_STATUS say so */
        ati_3d_draw(s, fmt0, (vf & 0xffff) | count << 16, buf, count * size,
                    NULL, 0);
        g_free(buf);
        return;
    }
    if (!(r3d(s, R200_SE_VAP_CNTL) & R200_VAP_TCL_ENABLE)) {
        /* already transformed: the arrays hold the vertices to draw */
        ati_3d_draw(s, fmt0, (vf & 0xffff) | count << 16, buf,
                    count * size, NULL, 0);
        g_free(buf);
        return;
    }
    if (!ati_3d_setup(s, &c)) {
        g_free(buf);
        return;
    }
    c.tcl = true;
    c.vte_cntl = r3d(s, R200_SE_VTE_CNTL);
    for (i = 0; i < 6; i++) {
        c.vport[i] = u2f(r3d(s, R200_SE_VPORT_XSCALE + i * 4));
    }
    v = g_new(ATIVertex, count);
    for (i = 0; i < count; i++) {
        R200TclIn in;

        r200_tcl_in(fmt0, fmt1, vf, buf + (size_t)i * size, &in);
        r200_tcl_vertex(s, &in, &v[i]);
        trace_ati_3d_tcl_vertex(in.col[0][0] * 1000, in.col[0][1] * 1000,
                                in.col[0][2] * 1000, v[i].x * 1000,
                                v[i].y * 1000, v[i].z * 1000,
                                v[i].rhw * 1000, v[i].c[0] * 255,
                                v[i].c[1] * 255, v[i].c[2] * 255);
    }
    trace_ati_3d_draw(VF_PRIM(vf), count, fmt0, c.tex[0].enabled);
    ati_prims(&c, VF_PRIM(vf), v, count);
    g_free(v);
    g_free(buf);
    ati_3d_finish(&c);
}

/*
 * R100 transform and lighting. A draw goes through it when its VF_CNTL
 * asks for it (VF_TCL_ENABLE, as Mesa's hardware TCL path does) and
 * SE_CNTL_STATUS does not bypass it; otherwise the vertices are already
 * transformed, as with Mesa's software TCL and the MorphOS and AmigaOS
 * drivers. The matrices, lights and the global ambient are in the vector and
 * scalar memories as on the R200, at other places; the material is in
 * registers. Matrix n is at vector 4n, a row in each vector.
 */
#define R100_SE_CNTL_STATUS             0x2140
#define R100_TCL_BYPASS                 BIT(8)
#define R100_SE_TCL_MATERIAL_EMISSIVE   0x2210  /* then ambient, diffuse */
                                                /* and specular: RGBA */
#define R100_SE_TCL_SHININESS           0x2250
#define R100_SE_TCL_OUTPUT_VTX_SEL      0x2258
#define R100_SE_TCL_MATRIX_SELECT_0     0x225c
#define R100_SE_TCL_MATRIX_SELECT_1     0x2260
#define R100_SE_TCL_TEXTURE_PROC_CTL    0x2268
#define R100_SE_TCL_LIGHT_MODEL_CTL     0x226c

/* LIGHT_MODEL_CTL: the low bits are the R200's LIGHT_MODEL_CTL_0's */
#define R100_LIGHT_IN_MODELSPACE        BIT(1)
#define R100_RESCALE_NORMALS            BIT(4)
#define R100_LM_SOURCE(lm, i)           (((lm) >> (16 + (i) * 2)) & 3)

/* TEXTURE_PROC_CTL, for each texture unit n */
#define R100_TEXGEN_TEXMAT_ENABLE(n)    BIT(n)
#define R100_TEXMAT_ENABLE(n)           BIT(4 + (n))
#define R100_TEXGEN_INPUT(v, n)         (((v) >> (16 + (n) * 4)) & 0xf)
/* OUTPUT_VTX_SEL: the input of each output, + 8 when it is computed */
#define R100_TEX_OUTPUT(v, n)           (((v) >> (16 + (n) * 4)) & 0xf)

/* the vector memory */
#define R100_VS_LIGHT_AMBIENT           64
#define R100_VS_LIGHT_DIFFUSE           72
#define R100_VS_LIGHT_SPECULAR          80
#define R100_VS_LIGHT_DIRPOS            88
#define R100_VS_LIGHT_HWVSPOT           96
#define R100_VS_LIGHT_ATTENUATION       104
#define R100_VS_GLOBAL_AMBIENT          122
#define R100_VS_EYE_VECTOR              124     /* w: the normal rescale */
/* the scalar memory */
#define R100_SS_LIGHT_SPOT_EXPONENT     8
#define R100_SS_LIGHT_SPOT_CUTOFF       16
#define R100_SS_LIGHT_RANGE_CUTOFF      32
#define R100_SS_LIGHT_ATT_CONST_INV     40

#define R100_TCL_UNITS                  3

static bool r100_tcl_on(ATIVGAState *s, uint32_t vf)
{
    return (vf & VF_TCL_ENABLE) &&
           !(r3d(s, R100_SE_CNTL_STATUS) & R100_TCL_BYPASS);
}

/*
 * A vertex in the order r100_vtx_attrs gives. The texture coordinates go
 * in as s, t, 0, q (q 1 when the vertex has none), as Mesa's r100 driver
 * lays out its texture matrices for them.
 */
static void r100_tcl_in(uint32_t fmt, uint32_t vf, const uint32_t *d,
                        R200TclIn *in)
{
    static const uint32_t st[4] = { VTX_ST0, VTX_ST1, VTX_ST2, VTX_ST3 };
    static const uint32_t q[4] = { VTX_Q0, VTX_Q1, VTX_Q2, VTX_Q3 };
    int i = 0, n, k;

    memset(in, 0, sizeof(*in));
    in->pos[0] = u2f(d[i++]);
    in->pos[1] = u2f(d[i++]);
    in->pos[2] = fmt & VTX_Z ? u2f(d[i++]) : 0.0f;
    in->pos[3] = fmt & VTX_W0 ? u2f(d[i++]) : 1.0f;
    i += VTX_WEIGHTS(fmt);
    in->nrm[2] = 1.0f;
    if (fmt & VTX_N0) {
        for (k = 0; k < 3; k++) {
            in->nrm[k] = u2f(d[i++]);
        }
    }
    for (k = 0; k < 4; k++) {
        in->col[0][k] = 1.0f;
    }
    if (fmt & VTX_FPCOLOR) {
        for (k = 0; k < 3; k++) {
            in->col[0][k] = u2f(d[i++]);
        }
    }
    if (fmt & VTX_FPALPHA) {
        in->col[0][3] = u2f(d[i++]);
    }
    if (fmt & VTX_PKCOLOR) {
        ati_unpack_color(d[i++], vf & VF_COLOR_ORDER_RGBA, in->col[0]);
    }
    in->col[1][3] = 1.0f;
    if (fmt & VTX_FPSPEC) {
        for (k = 0; k < 3; k++) {
            in->col[1][k] = u2f(d[i++]);
        }
    }
    if (fmt & VTX_FPFOG) {
        in->col[1][3] = u2f(d[i++]);
    }
    if (fmt & VTX_PKSPEC) {
        ati_unpack_color(d[i++], vf & VF_COLOR_ORDER_RGBA, in->col[1]);
    }
    for (n = 0; n < 4; n++) {
        in->tex[n][3] = 1.0f;
        if (fmt & st[n]) {
            in->tex[n][0] = u2f(d[i++]);
            in->tex[n][1] = u2f(d[i++]);
        }
        if (fmt & q[n]) {
            in->tex[n][3] = u2f(d[i++]);
        }
    }
}

static void r100_tcl_mul(ATIVGAState *s, int mtx, const float in[4],
                         float out[4])
{
    const float (*m)[4] = &s->tcl.vec[(mtx & 0xf) * 4];
    int i;

    for (i = 0; i < 4; i++) {
        out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2] +
                 m[i][3] * in[3];
    }
}

/*
 * The source of a material colour (LIGHT_MODEL_CTL bits 16 to 23):
 * premultiplied into the light colours and the global ambient, so 1 (and
 * no emission of its own), the material registers, or a vertex colour.
 */
static const float *r100_tcl_src(ATIVGAState *s, uint32_t lm, int which,
                                 const R200TclIn *in, float mat[4][4])
{
    static const float one[4] = { 1, 1, 1, 1 };
    static const float zero[4] = { 0, 0, 0, 0 };

    switch (R100_LM_SOURCE(lm, which)) {
    case 0:
        return which ? one : zero;
    case 1:
        return mat[which];
    default:
        return in->col[R100_LM_SOURCE(lm, which) - 2];
    }
}

/* a vertex through the TCL: clip space position in x, y, z and rhw */
static void r100_tcl_vertex(ATIVGAState *s, const R200TclIn *in, ATIVertex *v)
{
    uint32_t sel0 = r3d(s, R100_SE_TCL_MATRIX_SELECT_0);
    uint32_t sel1 = r3d(s, R100_SE_TCL_MATRIX_SELECT_1);
    uint32_t lm = r3d(s, R100_SE_TCL_LIGHT_MODEL_CTL);
    uint32_t tpc = r3d(s, R100_SE_TCL_TEXTURE_PROC_CTL);
    uint32_t vsel = r3d(s, R100_SE_TCL_OUTPUT_VTX_SEL);
    float clip[4], eye[4], enrm[4], n4[4] = { in->nrm[0], in->nrm[1],
                                              in->nrm[2], 0 };
    int n, k;

    memset(v, 0, sizeof(*v));
    /* MODELPROJECT_0, MODELVIEW_0 and IT_MODELVIEW_0 */
    r100_tcl_mul(s, sel1, in->pos, clip);
    v->x = clip[0];
    v->y = clip[1];
    v->z = clip[2];
    v->rhw = clip[3];
    r100_tcl_mul(s, sel0, in->pos, eye);
    if (eye[3] != 0.0f && eye[3] != 1.0f) {
        eye[0] /= eye[3];
        eye[1] /= eye[3];
        eye[2] /= eye[3];
        eye[3] = 1.0f;
    }
    r100_tcl_mul(s, sel0 >> 16, n4, enrm);
    if (lm & R100_RESCALE_NORMALS) {
        float f = s->tcl.vec[R100_VS_EYE_VECTOR][3];

        enrm[0] *= f;
        enrm[1] *= f;
        enrm[2] *= f;
    }
    if (lm & R200_NORMALIZE_NORMALS) {
        normalize3(enrm);
    }

    if (lm & R200_LIGHTING_ENABLE) {
        float mat[4][4], pos[4], nrm[3];
        uint32_t i;
        ATITclLights L = {
            .lm = lm,
            .gamb = s->tcl.vec[R100_VS_GLOBAL_AMBIENT],
            .shin = u2f(r3d(s, R100_SE_TCL_SHININESS)),
            .vs_amb = R100_VS_LIGHT_AMBIENT,
            .vs_dif = R100_VS_LIGHT_DIFFUSE,
            .vs_spc = R100_VS_LIGHT_SPECULAR,
            .vs_pos = R100_VS_LIGHT_DIRPOS,
            .vs_dir = R100_VS_LIGHT_HWVSPOT,
            .vs_att = R100_VS_LIGHT_ATTENUATION,
            .ss_spot_exp = R100_SS_LIGHT_SPOT_EXPONENT,
            .ss_spot_cut = R100_SS_LIGHT_SPOT_CUTOFF,
            .ss_const_inv = R100_SS_LIGHT_ATT_CONST_INV,
            .ss_range = R100_SS_LIGHT_RANGE_CUTOFF,
        };

        /* emissive, ambient, diffuse and specular, one after another */
        for (i = 0; i < 16; i++) {
            mat[i / 4][i % 4] =
                u2f(r3d(s, R100_SE_TCL_MATERIAL_EMISSIVE + i * 4));
        }
        L.emis = r100_tcl_src(s, lm, 0, in, mat);
        L.amb = r100_tcl_src(s, lm, 1, in, mat);
        L.dif = r100_tcl_src(s, lm, 2, in, mat);
        L.spc = r100_tcl_src(s, lm, 3, in, mat);
        /* the lights are in the space the driver chose: model or eye */
        if (lm & R100_LIGHT_IN_MODELSPACE) {
            memcpy(pos, in->pos, sizeof(pos));
            memcpy(nrm, in->nrm, sizeof(nrm));
            if (lm & R200_NORMALIZE_NORMALS) {
                normalize3(nrm);
            }
        } else {
            memcpy(pos, eye, sizeof(pos));
            memcpy(nrm, enrm, sizeof(nrm));
        }
        ati_tcl_light(s, &L, pos, nrm, v->c, v->spec);
    } else {
        memcpy(v->c, in->col[0], sizeof(v->c));
        memcpy(v->spec, in->col[1], 3 * sizeof(float));
    }
    v->spec[3] = 1.0f;

    for (n = 0; n < R100_TCL_UNITS; n++) {
        int out = R100_TEX_OUTPUT(vsel, n), u = out & 3;
        float tc[4], res[4];
        bool mtx = false;

        memcpy(tc, in->tex[u], sizeof(tc));
        if (out & 8) {
            /* computed: texgen, then the texture matrix of its unit */
            if (tpc & R100_TEXGEN_TEXMAT_ENABLE(u)) {
                int src = R100_TEXGEN_INPUT(tpc, u);

                switch (src) {
                case 0 ... 3:
                    memcpy(tc, in->tex[src], sizeof(tc));
                    break;
                case 4: /* object position */
                    memcpy(tc, in->pos, sizeof(tc));
                    break;
                case 5: /* eye position */
                case 8: /* normalized */
                    memcpy(tc, eye, sizeof(tc));
                    if (src == 8) {
                        normalize3(tc);
                    }
                    break;
                case 6: /* eye normal */
                    memcpy(tc, enrm, 3 * sizeof(float));
                    tc[3] = 1.0f;
                    break;
                case 7: { /* the eye vector reflected */
                    float e[3] = { eye[0], eye[1], eye[2] }, d;

                    normalize3(e);
                    d = 2.0f * dot3(enrm, e);
                    for (k = 0; k < 3; k++) {
                        tc[k] = e[k] - d * enrm[k];
                    }
                    tc[3] = 1.0f;
                    break;
                }
                }
            }
            if (tpc & (R100_TEXGEN_TEXMAT_ENABLE(u) | R100_TEXMAT_ENABLE(u))) {
                r100_tcl_mul(s, sel1 >> (16 + u * 4), tc, res);
                memcpy(tc, res, sizeof(tc));
                mtx = true;
            }
        }
        /* Mesa puts the q row of a texture matrix third */
        v->tc[n][0] = tc[0];
        v->tc[n][1] = tc[1];
        v->tc[n][2] = mtx ? tc[2] : tc[3];
        if (v->tc[n][2] == 0.0f) {
            v->tc[n][2] = 1.0f;
        }
    }
}

static void r100_tcl_draw(ATIVGAState *s, uint32_t fmt, uint32_t vf,
                          const uint32_t *d, uint32_t n, const uint32_t *idx,
                          uint32_t nidx)
{
    int size = ati_vtx_dwords(fmt);
    int count = idx ? MIN(VF_NUM_VERTICES(vf), nidx * 2) :
                      MIN(VF_NUM_VERTICES(vf), n / size);
    int i, k = 0;
    ATI3DCtx c;
    ATIVertex *v;

    if (!count || !ati_3d_setup(s, &c)) {
        return;
    }
    count = MIN(count, ATI_3D_MAX_VERTS);
    /* the setup engine divides by w and maps to the viewport */
    c.tcl = true;
    c.vte_cntl = (c.se_cntl & SE_VPORT_XY_XFORM ? 0xf : 0) |
                 (c.se_cntl & SE_VPORT_Z_XFORM ? 0x30 : 0) |
                 (c.coord_fmt & CF_XY_PRE_MULT ? R200_VTE_XY_FMT : 0) |
                 (c.coord_fmt & CF_Z_PRE_MULT ? R200_VTE_Z_FMT : 0);
    for (i = 0; i < 6; i++) {
        c.vport[i] = u2f(r3d(s, R3D_VPORT_XSCALE + i * 4));
    }
    v = g_new(ATIVertex, count);
    for (i = 0; i < count; i++) {
        uint32_t j = i;
        R200TclIn in;

        if (idx) {
            j = (idx[i / 2] >> ((i & 1) * 16)) & 0xffff;
            if ((j + 1) * size > n) {
                continue;
            }
        }
        r100_tcl_in(fmt, vf, d + j * size, &in);
        r100_tcl_vertex(s, &in, &v[k]);
        trace_ati_3d_tcl_vertex(in.col[0][0] * 1000, in.col[0][1] * 1000,
                                in.col[0][2] * 1000, v[k].x * 1000,
                                v[k].y * 1000, v[k].z * 1000,
                                v[k].rhw * 1000, v[k].c[0] * 255,
                                v[k].c[1] * 255, v[k].c[2] * 255);
        k++;
    }
    trace_ati_3d_draw(VF_PRIM(vf), k, fmt, c.tex[0].enabled);
    ati_prims(&c, VF_PRIM(vf), v, k);
    g_free(v);
    ati_3d_finish(&c);
}

void ati_3d_packet(ATIVGAState *s, uint8_t op, const uint32_t *d, uint32_t n)
{
    switch (op) {
    case 0x23: /* 3D_RNDR_GEN_INDX_PRIM */
        if (n >= 4) {
            ati_3d_draw_buffer(s, d, n);
        }
        break;
    case 0x29: /* 3D_DRAW_IMMD: vertex format, VF_CNTL, vertices */
        if (n >= 2) {
            ati_3d_draw_vertices(s, d[0], d[1], d + 2, n - 2);
        }
        break;
    case 0x2f: /* 3D_LOAD_VBPNTR */
        r200_load_vbpntr(s, d, n);
        break;
    case 0x28: /* 3D_DRAW_VBUF on the R100: vertex format, VF_CNTL */
        if (n >= 2 && !ati_is_r200(s)) {
            ati_draw_aos(s, d[0], d[1], NULL, 0);
        }
        break;
    case 0x2a: /* 3D_DRAW_INDX on the R100: format, VF_CNTL, indices */
        if (n >= 2 && !ati_is_r200(s)) {
            ati_draw_aos(s, d[0], d[1], d + 2, n - 2);
        }
        break;
    case 0x34: /* 3D_DRAW_VBUF_2: VF_CNTL */
        if (n >= 1) {
            ati_draw_aos(s, 0, d[0], NULL, 0);
        }
        break;
    case 0x36: /* 3D_DRAW_INDX_2: VF_CNTL, then the indices */
        if (n >= 1) {
            ati_draw_aos(s, 0, d[0], d + 1, n - 1);
        }
        break;
    case 0x35: /* 3D_DRAW_IMMD_2: VF_CNTL, vertices */
        if (n >= 1) {
            ati_3d_draw_vertices(s, r3d(s, ati_is_r200(s) ? R200_SE_VTX_FMT_0 :
                                                            R3D_SE_VTX_FMT),
                                 d[0], d + 1, n - 1);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "ati_3d: packet 0x%02x\n", op);
        break;
    }
}
