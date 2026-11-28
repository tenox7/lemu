/*
 * MC68030 paged memory management unit
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The 68030 has its own MMU, not the 68040's: a translation control
 * register (TC), CPU and supervisor root pointers (CRP, SRP), two
 * transparent translation registers (TT0, TT1) and a status register
 * (MMUSR), reached with PMOVE. Tables have up to four levels (A to D),
 * optionally after a level indexed by the function code, with short
 * (4-byte) or long (8-byte) descriptors; a page descriptor above the
 * last level ends the walk early, and an indirect descriptor at the last
 * level points at the page descriptor. Written from the MC68030 User's
 * Manual, chapter 9.
 *
 * QEMU's TLB caches the translations, so PFLUSH and changes to the
 * registers flush it. A page goes into the TLB writable only once its
 * modified bit is set, so that the first write sets it.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/cpu-loop.h"
#include "system/memory.h"
#include "exec/helper-proto.h"
#include "qemu/log.h"

/* TC */
#define TC_E            0x80000000u
#define TC_SRE          0x02000000u
#define TC_FCL          0x01000000u

/* root pointers and long descriptors, upper long word */
#define RP_LU           0x80000000u
#define DESC_S          0x00000100u

/* descriptor bits */
#define DESC_CI         0x00000040u
#define DESC_M          0x00000010u
#define DESC_U          0x00000008u
#define DESC_WP         0x00000004u

#define DT_INVALID      0
#define DT_PAGE         1
#define DT_SHORT        2
#define DT_LONG         3

/* TTx */
#define TT_E            0x00008000u
#define TT_CI           0x00000400u
#define TT_RW           0x00000200u
#define TT_RWM          0x00000100u

/* MMUSR */
#define MMUSR_B         0x8000
#define MMUSR_L         0x4000
#define MMUSR_S         0x2000
#define MMUSR_W         0x0800
#define MMUSR_I         0x0400
#define MMUSR_M         0x0200
#define MMUSR_T         0x0040

/* 68030 special status word, in the format $A and $B frames */
#define SSW030_FB       0x4000
#define SSW030_RB       0x1000
#define SSW030_DF       0x0100
#define SSW030_RW       0x0040
#define SSW030_SIZE_BYTE 0x0010
#define SSW030_SIZE_WORD 0x0020

bool m68k_mmu030_enabled(CPUM68KState *env)
{
    return env->mmu030.tc & TC_E;
}

static int mmu030_fc(int access_type)
{
    return ((access_type & ACCESS_SUPER) ? 4 : 0) |
           ((access_type & ACCESS_CODE) ? 2 : 1);
}

static bool mmu030_tt_match(uint32_t tt, uint32_t addr, int fc, bool read)
{
    uint32_t mask = (tt >> 16) & 0xff;

    if (!(tt & TT_E)) {
        return false;
    }
    if (((addr >> 24) ^ (tt >> 24)) & ~mask & 0xff) {
        return false;
    }
    if ((fc ^ (tt >> 4)) & ~tt & 7) {
        return false;
    }
    return (tt & TT_RWM) || !!(tt & TT_RW) == read;
}

static bool mmu030_read(CPUM68KState *env, hwaddr addr, uint32_t *val)
{
    MemTxResult res;

    *val = address_space_ldl(env_cpu(env)->as, addr, MEMTXATTRS_UNSPECIFIED,
                             &res);
    return res == MEMTX_OK;
}

static void mmu030_write(CPUM68KState *env, hwaddr addr, uint32_t val)
{
    address_space_stl(env_cpu(env)->as, addr, val, MEMTXATTRS_UNSPECIFIED,
                      NULL);
}

/*
 * A TC the MMU can work with: a page of 256 bytes or more, and the
 * initial shift, the table indexes up to the first that is 0, and the
 * page size together cover the 32 bits of an address
 */
static bool mmu030_tc_valid(uint32_t tc)
{
    int bits = ((tc >> 16) & 15) + ((tc >> 20) & 15);

    if (((tc >> 20) & 15) < 8) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        int w = (tc >> (12 - 4 * i)) & 15;

        if (!w) {
            break;
        }
        bits += w;
    }
    return bits == 32;
}

/*
 * Translate addr. Returns the MMUSR the walk ends with: 0 (or only the
 * level count, the M and T bits) means the access may go on. max_level
 * limits the walk for PTEST (7: the whole table); *desc gets the address
 * of the last descriptor read. update says whether to set the U and M
 * bits in the tables.
 */
static uint16_t mmu030_walk(CPUM68KState *env, uint32_t addr, int access_type,
                            int max_level, bool update, hwaddr *phys,
                            int *prot, target_ulong *page_size,
                            uint32_t *desc)
{
    uint32_t tc = env->mmu030.tc;
    bool super = access_type & ACCESS_SUPER;
    bool write = access_type & ACCESS_STORE;
    int fc = mmu030_fc(access_type);
    uint64_t root = (super && (tc & TC_SRE)) ? env->mmu030.srp
                                             : env->mmu030.crp;
    uint32_t upper = root >> 32;
    uint32_t table = (uint32_t)root & 0xFFFFFFF0;
    int dt = upper & 3;
    bool have_limit = true;     /* the root pointer has a limit */
    uint32_t limit_word = upper;
    int ps = (tc >> 20) & 15;
    int is = (tc >> 16) & 15;
    int widths[5], nlevels = 0;
    int shift = 32 - is;        /* bits of addr still to be indexed */
    bool wp = false, sonly = false;
    uint16_t mmusr = 0;
    uint32_t page = 0;
    bool ci = false, modified = false;
    uint32_t page_desc_addr = 0;
    int level = 0;

    *desc = 0;
    if (!mmu030_tc_valid(tc)) {
        /* PTEST with a TC that PMOVE would not take */
        mmusr = MMUSR_I;
        goto done;
    }
    if (tc & TC_FCL) {
        widths[nlevels++] = 3;      /* the function code level */
    }
    for (int i = 0; i < 4; i++) {
        int w = (tc >> (12 - 4 * i)) & 15;

        if (!w) {
            break;
        }
        widths[nlevels++] = w;
    }

    for (int l = 0; l < nlevels; l++) {
        uint32_t index, d0, d1 = 0, da;
        int size;
        bool fcl = (tc & TC_FCL) && l == 0;

        if (dt == DT_PAGE || dt == DT_INVALID) {
            break;
        }
        if (level >= max_level) {
            break;
        }
        if (fcl) {
            index = fc;
        } else {
            shift -= widths[l];
            index = extract32(addr, shift, widths[l]);
        }
        /* limit of the descriptor that points at this table */
        if (have_limit) {
            uint32_t lim = (limit_word >> 16) & 0x7fff;

            if ((limit_word & RP_LU) ? index < lim : index > lim) {
                mmusr |= MMUSR_L | MMUSR_I;
                goto done;
            }
        }
        size = dt == DT_LONG ? 8 : 4;
        da = table + index * size;
        *desc = da;
        level++;
        if (!mmu030_read(env, da, &d0) ||
            (size == 8 && !mmu030_read(env, da + 4, &d1))) {
            mmusr |= MMUSR_B | MMUSR_I;
            goto done;
        }
        dt = d0 & 3;
        if (dt == DT_INVALID) {
            mmusr |= MMUSR_I;
            goto done;
        }
        wp |= d0 & DESC_WP;
        if (size == 8) {
            sonly |= d0 & DESC_S;
            have_limit = true;
            limit_word = d0;
        } else {
            have_limit = false;
        }
        if (update && !(d0 & DESC_U)) {
            mmu030_write(env, da, d0 | DESC_U);
            d0 |= DESC_U;
        }
        if (dt == DT_PAGE) {
            page = size == 8 ? d1 : d0;
            ci = d0 & DESC_CI;
            modified = d0 & DESC_M;
            page_desc_addr = da;
            break;
        }
        table = (size == 8 ? d1 : d0) & 0xFFFFFFF0;
        if (l == nlevels - 1) {
            /* indirect: the descriptor points at the page descriptor */
            int psize = dt == DT_LONG ? 8 : 4;

            *desc = table;
            if (!mmu030_read(env, table, &d0) ||
                (psize == 8 && !mmu030_read(env, table + 4, &d1))) {
                mmusr |= MMUSR_B | MMUSR_I;
                goto done;
            }
            if ((d0 & 3) != DT_PAGE) {
                mmusr |= MMUSR_I;
                goto done;
            }
            wp |= d0 & DESC_WP;
            if (psize == 8) {
                sonly |= d0 & DESC_S;
            }
            if (update && !(d0 & DESC_U)) {
                mmu030_write(env, table, d0 | DESC_U);
                d0 |= DESC_U;
            }
            page = psize == 8 ? d1 : d0;
            ci = d0 & DESC_CI;
            modified = d0 & DESC_M;
            page_desc_addr = table;
            dt = DT_PAGE;
            break;
        }
    }

    if (dt == DT_INVALID) {
        mmusr |= MMUSR_I;
        goto done;
    }
    if (dt != DT_PAGE) {
        /* PTEST stopped above the page */
        goto done;
    }
    if (level == 0) {
        /* the root pointer is the page descriptor */
        page = table;
    }

    if (sonly && !super) {
        mmusr |= MMUSR_S;
    }
    if (wp) {
        mmusr |= MMUSR_W;
    }
    if (mmusr & MMUSR_S || (write && wp)) {
        goto done;
    }
    if (update && write && !modified && page_desc_addr) {
        uint32_t d0;

        if (mmu030_read(env, page_desc_addr, &d0)) {
            mmu030_write(env, page_desc_addr, d0 | DESC_M | DESC_U);
        }
        modified = true;
    }
    if (modified) {
        mmusr |= MMUSR_M;
    }

    /* what the walk did not index is offset in the page */
    if (shift < ps) {
        shift = ps;
    }
    {
        uint32_t off_mask = shift >= 32 ? 0xFFFFFFFF : (1u << shift) - 1;

        *phys = (page & ~off_mask) | (addr & off_mask);
        /* QEMU's TLB needs no more than this for one entry */
        *page_size = 1u << MIN(shift, 24);
    }
    *prot = PAGE_READ | PAGE_EXEC;
    if (!wp && modified) {
        *prot |= PAGE_WRITE;
    }
    (void)ci;

done:
    return mmusr | (level & 7);
}

/* Returns true when the access may go on */
static bool mmu030_translate(CPUM68KState *env, uint32_t addr,
                             int access_type, bool update, hwaddr *phys,
                             int *prot, target_ulong *page_size,
                             uint16_t *mmusr_out)
{
    bool read = !(access_type & ACCESS_STORE);
    int fc = mmu030_fc(access_type);
    uint32_t desc;
    uint16_t mmusr;

    for (int i = 0; i < 2; i++) {
        if (mmu030_tt_match(env->mmu030.tt[i], addr, fc, read)) {
            *phys = addr;
            *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            *page_size = TARGET_PAGE_SIZE;
            if (mmusr_out) {
                *mmusr_out = MMUSR_T;
            }
            return true;
        }
    }
    mmusr = mmu030_walk(env, addr, access_type, 7, update, phys, prot,
                        page_size, &desc);
    if (mmusr_out) {
        *mmusr_out = mmusr;
    }
    if (mmusr & (MMUSR_B | MMUSR_L | MMUSR_S | MMUSR_I)) {
        return false;
    }
    if ((mmusr & MMUSR_W) && !read) {
        return false;
    }
    return true;
}

bool m68k_mmu030_get_phys(CPUM68KState *env, uint32_t addr, int access_type,
                          hwaddr *phys)
{
    int prot;
    target_ulong page_size;

    if (!m68k_mmu030_enabled(env)) {
        *phys = addr;
        return true;
    }
    return mmu030_translate(env, addr, access_type, false, phys, &prot,
                            &page_size, NULL);
}

void m68k_mmu030_fault_ssw(CPUM68KState *env, uint32_t addr, int size,
                           MMUAccessType qemu_access_type, int mmu_idx)
{
    uint32_t ssw;
    /* the function code of the access: MOVES uses the user's */
    int fc = (mmu_idx != MMU_USER_IDX ? 4 : 0) |
             (qemu_access_type == MMU_INST_FETCH ? 2 : 1);

    if (qemu_access_type == MMU_INST_FETCH) {
        ssw = SSW030_FB | SSW030_RB | fc;
    } else {
        ssw = SSW030_DF | fc;
        if (qemu_access_type == MMU_DATA_LOAD) {
            ssw |= SSW030_RW;
        }
        if (size == 1) {
            ssw |= SSW030_SIZE_BYTE;
        } else if (size == 2) {
            ssw |= SSW030_SIZE_WORD;
        }
    }
    env->mmu.ssw = ssw;
    env->mmu.ar = addr;
}

bool m68k_mmu030_tlb_fill(CPUState *cs, vaddr address, int size,
                          MMUAccessType qemu_access_type, int mmu_idx,
                          bool probe, uintptr_t retaddr)
{
    CPUM68KState *env = cpu_env(cs);
    int access_type;
    hwaddr phys;
    int prot;
    target_ulong page_size;

    if (!m68k_mmu030_enabled(env)) {
        tlb_set_page(cs, address & TARGET_PAGE_MASK,
                     address & TARGET_PAGE_MASK,
                     PAGE_READ | PAGE_WRITE | PAGE_EXEC,
                     mmu_idx, TARGET_PAGE_SIZE);
        return true;
    }

    access_type = qemu_access_type == MMU_INST_FETCH ? ACCESS_CODE
                                                     : ACCESS_DATA;
    if (qemu_access_type == MMU_DATA_STORE) {
        access_type |= ACCESS_STORE;
    }
    if (mmu_idx != MMU_USER_IDX) {
        access_type |= ACCESS_SUPER;
    }

    if (mmu030_translate(env, address, access_type, !probe, &phys, &prot,
                         &page_size, NULL)) {
        /*
         * The QEMU page of address, at the physical address it maps to:
         * QEMU looks a page smaller than its own up again on each access.
         */
        tlb_set_page(cs, address & TARGET_PAGE_MASK,
                     phys - (address & ~TARGET_PAGE_MASK), prot, mmu_idx,
                     page_size);
        return true;
    }
    if (probe) {
        return false;
    }

    m68k_mmu030_fault_ssw(env, address, size, qemu_access_type, mmu_idx);
    cs->exception_index = EXCP_ACCESS;
    cpu_loop_exit_restore(cs, retaddr);
}

/* ---- instructions ---- */

static int mmu030_ext_fc(CPUM68KState *env, uint16_t ext)
{
    int f = ext & 0x1f;

    if (f & 0x10) {
        return f & 7;
    }
    if (f & 0x08) {
        return env->dregs[f & 7] & 7;
    }
    return f ? env->dfc & 7 : env->sfc & 7;
}

/*
 * The MMU configuration exception, after the PMOVE as a trap: a format
 * $2 frame with the PMOVE's address, returning to the next instruction
 */
static G_NORETURN void mmu030_config_exception(CPUM68KState *env,
                                               uint32_t next_pc,
                                               uintptr_t ra)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = EXCP_MMU_CONF;
    cpu_restore_state(cs, ra);
    env->cc_op = CC_OP_FLAGS;
    env->mmu.ar = env->pc;
    env->pc = next_pc;
    cpu_loop_exit(cs);
}

/*
 * PMOVE: ext gives the register and the direction; addr the memory;
 * next_pc is the instruction after it
 */
void HELPER(pmove)(CPUM68KState *env, uint32_t ext, uint32_t addr,
                   uint32_t next_pc)
{
    CPUState *cs = env_cpu(env);
    uintptr_t ra = GETPC();
    bool to_mem = ext & 0x0200;
    bool flush = !(ext & 0x0100);
    int type = ext >> 13;
    int preg = (ext >> 10) & 7;

    switch (type) {
    case 0:     /* TT0, TT1 */
        if (preg != 2 && preg != 3) {
            break;
        }
        if (to_mem) {
            cpu_stl_be_data_ra(env, addr, env->mmu030.tt[preg - 2], ra);
        } else {
            env->mmu030.tt[preg - 2] = cpu_ldl_be_data_ra(env, addr, ra);
            if (flush) {
                tlb_flush(cs);
            }
        }
        return;
    case 2:     /* TC, SRP, CRP */
        if (preg == 0) {
            if (to_mem) {
                cpu_stl_be_data_ra(env, addr, env->mmu030.tc, ra);
            } else {
                env->mmu030.tc = cpu_ldl_be_data_ra(env, addr, ra);
                tlb_flush(cs);
                if ((env->mmu030.tc & TC_E) &&
                    !mmu030_tc_valid(env->mmu030.tc)) {
                    /* the MMU stays off */
                    env->mmu030.tc &= ~TC_E;
                    mmu030_config_exception(env, next_pc, ra);
                }
            }
            return;
        }
        if (preg == 2 || preg == 3) {
            uint64_t *rp = preg == 2 ? &env->mmu030.srp : &env->mmu030.crp;

            if (to_mem) {
                cpu_stl_be_data_ra(env, addr, *rp >> 32, ra);
                cpu_stl_be_data_ra(env, addr + 4, *rp, ra);
            } else {
                uint64_t hi = cpu_ldl_be_data_ra(env, addr, ra);

                *rp = (hi << 32) | cpu_ldl_be_data_ra(env, addr + 4, ra);
                if (flush) {
                    tlb_flush(cs);
                }
            }
            return;
        }
        break;
    case 3:     /* MMUSR */
        if (to_mem) {
            cpu_stw_be_data_ra(env, addr, env->mmu030.mmusr, ra);
        } else {
            env->mmu030.mmusr = cpu_lduw_be_data_ra(env, addr, ra);
        }
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "pmove: unknown register (ext %04x)\n",
                  ext);
}

/* PFLUSH and PLOAD */
void HELPER(pflush030)(CPUM68KState *env, uint32_t ext, uint32_t addr)
{
    if ((ext >> 10) & 7) {
        tlb_flush(env_cpu(env));
    }
    /* PLOAD only loads the cache of translations, which QEMU does itself */
}

/* PTEST: walk the tables for addr and leave the result in the MMUSR */
void HELPER(ptest030)(CPUM68KState *env, uint32_t ext, uint32_t addr)
{
    int level = (ext >> 10) & 7;
    bool read = ext & 0x0200;
    int fc = mmu030_ext_fc(env, ext);
    int access_type = (fc & 4 ? ACCESS_SUPER : 0) |
                      ((fc & 3) == 2 ? ACCESS_CODE : ACCESS_DATA) |
                      (read ? 0 : ACCESS_STORE);
    uint16_t mmusr = 0;
    uint32_t desc = 0;
    hwaddr phys;
    int prot;
    target_ulong page_size;

    if (level == 0) {
        /* the cache of translations: answer as for a full walk */
        level = 7;
    }
    for (int i = 0; i < 2; i++) {
        if (mmu030_tt_match(env->mmu030.tt[i], addr, fc, read)) {
            mmusr = MMUSR_T;
            goto out;
        }
    }
    mmusr = mmu030_walk(env, addr, access_type, level, false, &phys, &prot,
                        &page_size, &desc);
out:
    env->mmu030.mmusr = mmusr;
    if (ext & 0x0100) {
        env->aregs[(ext >> 5) & 7] = desc;
    }
}
