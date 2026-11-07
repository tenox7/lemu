/*
 * Interface to the capstone disassembler.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/ctype.h"
#include "disas/dis-asm.h"
#include "disas/capstone.h"


/*
 * Temporary storage for the capstone library.  This will be alloced via
 * malloc with a size private to the library; thus there's no reason not
 * to share this across calls and across host vs target disassembly.
 */
static __thread cs_insn *cap_insn;

/*
 * The capstone library always skips 2 bytes for S390X.
 * This is less than ideal, since we can tell from the first two bits
 * the size of the insn and thus stay in sync with the insn stream.
 */
static size_t CAPSTONE_API
cap_skipdata_s390x_cb(const uint8_t *code, size_t code_size,
                      size_t offset, void *user_data)
{
    size_t ilen;

    /* See get_ilen() in target/s390x/internal.h.  */
    switch (code[offset] >> 6) {
    case 0:
        ilen = 2;
        break;
    case 1:
    case 2:
        ilen = 4;
        break;
    default:
        ilen = 6;
        break;
    }

    return ilen;
}

static const cs_opt_skipdata cap_skipdata_s390x = {
    .mnemonic = ".byte",
    .callback = cap_skipdata_s390x_cb
};

/* Similarly for RISCV */
static size_t CAPSTONE_API
cap_skipdata_riscv_cb(const uint8_t *code, size_t code_size,
                      size_t offset, void *user_data)
{
    /* See insn_len() from target/riscv/internals.h */
    return (code[offset] & 3) == 3 ? 4 : 2;
}

static const cs_opt_skipdata cap_skipdata_riscv = {
    .mnemonic = ".byte",
    .callback = cap_skipdata_riscv_cb
};

/*
 * Initialize the Capstone library.
 *
 * ??? It would be nice to cache this.  We would need one handle for the
 * host and one for the target.  For most targets we can reset specific
 * parameters via cs_option(CS_OPT_MODE, new_mode), but we cannot change
 * CS_ARCH_* in this way.  Thus we would need to be able to close and
 * re-open the target handle with a different arch for the target in order
 * to handle AArch64 vs AArch32 mode switching.
 */
static cs_err cap_disas_start(disassemble_info *info, csh *handle)
{
    cs_mode cap_mode = info->cap_mode;
    cs_err err;

    cap_mode += (info->endian == BFD_ENDIAN_BIG ? CS_MODE_BIG_ENDIAN
                 : CS_MODE_LITTLE_ENDIAN);

    err = cs_open(info->cap_arch, cap_mode, handle);
    if (err != CS_ERR_OK) {
        return err;
    }

    /* "Disassemble" unknown insns as ".byte W,X,Y,Z".  */
    cs_option(*handle, CS_OPT_SKIPDATA, CS_OPT_ON);

    switch (info->cap_arch) {
    case CS_ARCH_RISCV:
        cs_option(*handle, CS_OPT_SKIPDATA_SETUP,
                  (uintptr_t)&cap_skipdata_riscv);
        break;

    case CS_ARCH_SYSTEMZ:
        cs_option(*handle, CS_OPT_SKIPDATA_SETUP,
                  (uintptr_t)&cap_skipdata_s390x);
        break;

    case CS_ARCH_X86:
        /*
         * We don't care about errors (if for some reason the library
         * is compiled without AT&T syntax); the user will just have
         * to deal with the Intel syntax.
         */
        cs_option(*handle, CS_OPT_SYNTAX, CS_OPT_SYNTAX_ATT);
        break;
    }

    /* Allocate temp space for cs_disasm_iter.  */
    if (cap_insn == NULL) {
        cap_insn = cs_malloc(*handle);
        if (cap_insn == NULL) {
            cs_close(handle);
            return CS_ERR_MEM;
        }
    }
    return CS_ERR_OK;
}

static void cap_dump_insn_units(disassemble_info *info, cs_insn *insn,
                                int i, int n)
{
    fprintf_function print = info->fprintf_func;
    FILE *stream = info->stream;
    int unit = MIN(info->cap_insn_unit, n - i);

    switch (unit) {
    case 4:
        if (info->endian == BFD_ENDIAN_BIG) {
            for (; i < n; i += 4) {
                print(stream, " %08x", ldl_be_p(insn->bytes + i));

            }
        } else {
            for (; i < n; i += 4) {
                print(stream, " %08x", ldl_le_p(insn->bytes + i));
            }
        }
        break;

    case 2:
        if (info->endian == BFD_ENDIAN_BIG) {
            for (; i < n; i += 2) {
                print(stream, " %04x", lduw_be_p(insn->bytes + i));
            }
        } else {
            for (; i < n; i += 2) {
                print(stream, " %04x", lduw_le_p(insn->bytes + i));
            }
        }
        break;

    default:
        for (; i < n; i++) {
            print(stream, " %02x", insn->bytes[i]);
        }
        break;
    }

    if (unit < info->cap_insn_unit) {
        int width = (info->cap_insn_unit - unit) * 2;
        print(stream, "%*s", width, "");
    }
}

/*
 * LEMU: the operands of an instruction, with a name at the end of each
 * operand that holds an address with one, as in "0x80123400 <panic>".
 * @branch: the instruction jumps or calls, so its numbers are places,
 * and a place in a symbol gets a name too, "panic+0x1c".  Elsewhere a
 * number may be no place, so only a symbol's own address gets one.
 * Numbers are 0x1234, or $1234 in Motorola's syntax; not a displacement
 * from a register, "0x10(%eax)", nor an immediate in AT&T's, "$0x10".
 * A 64-bit MIPS prints 0xffffffffbfc00000 as -0x40400000.
 */
static char *cap_name_numbers(const char *op_str, bool branch,
                              bool motorola, char *(*name)(uint64_t))
{
    GString *s = g_string_new(NULL);
    g_autofree char *pending = NULL;
    const char *p = op_str, *digits, *q;

    for (;;) {
        bool word = p > op_str && (qemu_isalnum(p[-1]) || p[-1] == '_');

        if ((!*p || *p == ',') && pending) {
            g_string_append_printf(s, " <%s>", pending);
            g_clear_pointer(&pending, g_free);
        }
        if (!*p) {
            break;
        }
        if (!word && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
            (p == op_str || p[-1] != '$')) {
            digits = p + 2;
        } else if (!word && motorola && p[0] == '$' && qemu_isxdigit(p[1])) {
            digits = p + 1;
        } else {
            g_string_append_c(s, *p++);
            continue;
        }
        for (q = digits; qemu_isxdigit(*q); q++) {
            /* the digits */
        }
        g_string_append_len(s, p, q - p);
        if (q > digits && *q != '(' && !qemu_isalnum(*q) && *q != '_' &&
            !pending) {
            uint64_t addr = g_ascii_strtoull(digits, NULL, 16);

            pending = name(p > op_str && p[-1] == '-' ? -addr : addr);
            if (pending && !branch && strstr(pending, "+0x")) {
                g_clear_pointer(&pending, g_free);
            }
        }
        p = q;
    }
    return g_string_free(s, false);
}

/*
 * One instruction.  With @name (LEMU, for the monitor), its address
 * and the addresses in it get names.
 */
static void cap_dump_insn_named(disassemble_info *info, cs_insn *insn,
                                csh handle, char *(*name)(uint64_t))
{
    fprintf_function print = info->fprintf_func;
    FILE *stream = info->stream;
    g_autofree char *at = name ? name(insn->address) : NULL;
    g_autofree char *ops = NULL;
    int i, n, split;

    if (name) {
        /*
         * Capstone 5 gives MIPS jal no group, but a call goes to a
         * function's own address, which gets its name anyway.
         */
        bool branch = insn->detail &&
            (cs_insn_group(handle, insn, CS_GRP_JUMP) ||
             cs_insn_group(handle, insn, CS_GRP_CALL));

        ops = cap_name_numbers(insn->op_str, branch,
                               info->cap_arch == CS_ARCH_M68K, name);
    }
    print(stream, "0x%08" PRIx64 "%s%s%s: ", insn->address, at ? " <" : "",
          at ? at : "", at ? ">" : "");

    n = insn->size;
    split = info->cap_insn_split;

    /* Dump the first SPLIT bytes of the instruction.  */
    cap_dump_insn_units(info, insn, 0, MIN(n, split));

    /* Add padding up to SPLIT so that mnemonics line up.  */
    if (n < split) {
        int width = (split - n) / info->cap_insn_unit;
        width *= (2 * info->cap_insn_unit + 1);
        print(stream, "%*s", width, "");
    }

    /* Print the actual instruction.  */
    print(stream, "  %-8s %s\n", insn->mnemonic, ops ? ops : insn->op_str);

    /* Dump any remaining part of the insn on subsequent lines.  */
    for (i = split; i < n; i += split) {
        print(stream, "0x%08" PRIx64 ": ", insn->address + i);
        cap_dump_insn_units(info, insn, i, MIN(n, i + split));
        print(stream, "\n");
    }
}

static void cap_dump_insn(disassemble_info *info, cs_insn *insn)
{
    cap_dump_insn_named(info, insn, 0, NULL);
}

/* Disassemble SIZE bytes at PC for the target.  */
bool cap_disas_target(disassemble_info *info, uint64_t pc, size_t size)
{
    uint8_t cap_buf[1024];
    csh handle;
    cs_insn *insn;
    size_t csize = 0;

    if (cap_disas_start(info, &handle) != CS_ERR_OK) {
        return false;
    }
    insn = cap_insn;

    while (1) {
        size_t tsize = MIN(sizeof(cap_buf) - csize, size);
        const uint8_t *cbuf = cap_buf;

        if (info->read_memory_func(pc + csize, cap_buf + csize, tsize, info) == 0) {
            csize += tsize;
            size -= tsize;

            while (cs_disasm_iter(handle, &cbuf, &csize, &pc, insn)) {
                cap_dump_insn(info, insn);
            }

            /* If the target memory is not consumed, go back for more... */
            if (size != 0) {
                /*
                 * ... taking care to move any remaining fractional insn
                 * to the beginning of the buffer.
                 */
                if (csize != 0) {
                    memmove(cap_buf, cbuf, csize);
                }
                continue;
            }

            /*
             * Since the target memory is consumed, we should not have
             * a remaining fractional insn.
             */
            if (csize != 0) {
                info->fprintf_func(info->stream,
                                   "Disassembler disagrees with translator "
                                   "over instruction decoding\n"
                                   "Please report this to qemu-devel@nongnu.org\n");
            }
            break;

        } else {
            info->fprintf_func(info->stream,
                               "0x%08" PRIx64 ": unable to read memory\n", pc);
            break;
        }
    }

    cs_close(&handle);
    return true;
}

/* Disassemble SIZE bytes at CODE for the host.  */
bool cap_disas_host(disassemble_info *info, const void *code, size_t size)
{
    csh handle;
    const uint8_t *cbuf;
    cs_insn *insn;
    uint64_t pc;

    if (cap_disas_start(info, &handle) != CS_ERR_OK) {
        return false;
    }
    insn = cap_insn;

    cbuf = code;
    pc = (uintptr_t)code;

    while (cs_disasm_iter(handle, &cbuf, &size, &pc, insn)) {
        cap_dump_insn(info, insn);
    }
    if (size != 0) {
        info->fprintf_func(info->stream,
            "Disassembler disagrees with TCG over instruction encoding\n"
            "Please report this to qemu-devel@nongnu.org\n");
    }

    cs_close(&handle);
    return true;
}

/*
 * Disassemble COUNT insns at PC for the target.  LEMU: with @name,
 * addresses get names (see monitor_disas_symbol).
 */
bool cap_disas_monitor(disassemble_info *info, uint64_t pc, int count,
                       char *(*name)(uint64_t addr))
{
    uint8_t cap_buf[32];
    csh handle;
    cs_insn *insn, *own = NULL;
    size_t csize = 0;

    if (cap_disas_start(info, &handle) != CS_ERR_OK) {
        return false;
    }
    insn = cap_insn;
    /* the groups tell a jump or a call; cap_insn has no room for them */
    if (name && cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK) {
        own = cs_malloc(handle);
        insn = own ? own : insn;
    }

    while (1) {
        /*
         * We want to read memory for one insn, but generically we do not
         * know how much memory that is.  We have a small buffer which is
         * known to be sufficient for all supported targets.  Try to not
         * read beyond the page, Just In Case.  For even more simplicity,
         * ignore the actual target page size and use a 1k boundary.  If
         * that turns out to be insufficient, we'll come back around the
         * loop and read more.
         */
        uint64_t epc = QEMU_ALIGN_UP(pc + csize + 1, 1024);
        size_t tsize = MIN(sizeof(cap_buf) - csize, epc - pc);
        const uint8_t *cbuf = cap_buf;

        /* Make certain that we can make progress.  */
        assert(tsize != 0);
        if (info->read_memory_func(pc + csize, cap_buf + csize,
                                   tsize, info) == 0)
        {
            csize += tsize;

            if (cs_disasm_iter(handle, &cbuf, &csize, &pc, insn)) {
                cap_dump_insn_named(info, insn, handle, name);
                if (--count <= 0) {
                    break;
                }
            }
            memmove(cap_buf, cbuf, csize);
        } else {
            info->fprintf_func(info->stream,
                               "0x%08" PRIx64 ": unable to read memory\n", pc);
            break;
        }
    }
    if (own) {
        cs_free(own, 1);
    }

    cs_close(&handle);
    return true;
}

/* Disassemble a single instruction directly into plugin output */
bool cap_disas_plugin(disassemble_info *info, uint64_t pc, size_t size)
{
    uint8_t cap_buf[32];
    const uint8_t *cbuf = cap_buf;
    csh handle;

    if (cap_disas_start(info, &handle) != CS_ERR_OK) {
        return false;
    }

    assert(size < sizeof(cap_buf));
    info->read_memory_func(pc, cap_buf, size, info);

    if (cs_disasm_iter(handle, &cbuf, &size, &pc, cap_insn)) {
        info->fprintf_func(info->stream, "%s %s",
                           cap_insn->mnemonic, cap_insn->op_str);
    }

    cs_close(&handle);
    return true;
}
