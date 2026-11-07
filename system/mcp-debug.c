/*
 * Debug tools for MCP clients: run to an address, single steps, a
 * profile of where the CPUs run, memory writes and memory search (see
 * qapi/mcp.json).
 * They do what agents used to do with GDB scripts on the GDB stub.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "disas/disas.h"
#include "exec/gdbstub.h"
#include "exec/replay-core.h"
#include "exec/target_page.h"
#include "hw/core/cpu.h"
#include "monitor/mcp.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-mcp.h"
#include "qemu/coroutine.h"
#include "qemu/ctype.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/bswap.h"
#include "qemu/rcu.h"
#include "qemu/target-info.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "ui/input.h"
#ifdef CONFIG_TCG
#include "accel/tcg/cpu-ops.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"
#include "exec/debug-cond.h"
#include "exec/excp-watch.h"
#include "exec/tb-flush.h"
#include "exec/tb-cover.h"
#include "exec/tb-history.h"
#include "exec/watchpoint.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "qemu/thread.h"
#include "system/ram-write-hook.h"
#include "system/io-access-hook.h"
#endif

#define WRITE_MAX_BYTES         1024

/*
 * The MCP tools are reading or writing guest memory themselves, as
 * memory-write, a condition or a trace does: the debug tools don't
 * take that for a device's or a CPU's access
 */
static __thread int mcp_dev_self;

/* "0x..." is hex, anything else decimal, as qapi/mcp.json says */
static bool mcp_parse_address(const char *s, const char *what,
                              uint64_t *addr, Error **errp)
{
    int base = 10;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
    }
    if (s[0] == '-' || qemu_strtou64(s, NULL, base, addr) < 0) {
        error_setg(errp, "%s '%s' is not a number: write hex with 0x, or "
                   "decimal", what, s);
        return false;
    }
    return true;
}

/*
 * The CPU asked for, else the one that stopped last in a debug tool,
 * else the first.
 */
static int mcp_debug_last_cpu = -1;

static CPUState *mcp_debug_cpu(bool has_cpu, int64_t index, Error **errp)
{
    CPUState *cpu = NULL;

    if (has_cpu) {
        if (index >= 0 && index <= INT_MAX) {
            cpu = qemu_get_cpu(index);
        }
        if (!cpu) {
            error_setg(errp, "there is no CPU %" PRId64, index);
        }
        return cpu;
    }
    if (mcp_debug_last_cpu >= 0) {
        cpu = qemu_get_cpu(mcp_debug_last_cpu);
    }
    cpu = cpu ? cpu : first_cpu;
    if (!cpu) {
        error_setg(errp, "this VM has no CPU");
    }
    return cpu;
}

/*
 * Symbols from debug-symbols: names for guest addresses.  Only the main
 * loop uses them.
 */

#define SYM_MAX                 2000000
#define SYM_FILE_MAX            (256 * MiB)
#define SYM_NEAR                (64 * KiB)  /* past one of no size */
#define SYM_LOOKUPS_MAX         64
#define SYM_LINE_SHOWN          80

typedef struct McpSym {
    uint64_t addr;
    uint64_t size;              /* 0 when the list did not say */
    char type;                  /* nm's letter, or '?' */
    char name[];
} McpSym;

static struct {
    GPtrArray *all;             /* McpSym, in the order loaded */
    GPtrArray *by_addr;         /* the ones shown for an address, sorted */
    GHashTable *by_name;        /* the first McpSym of each name */
    int64_t dup_names;          /* names given at several addresses */
} mcp_syms;

static bool mcp_sym_is_code(const McpSym *s)
{
    return s->type == 'T' || s->type == 't' || s->type == 'W' ||
           s->type == 'w';
}

/* Which of several at one address to show: global code first */
static int mcp_sym_rank(const McpSym *s)
{
    return (qemu_isupper(s->type) ? 2 : 0) + mcp_sym_is_code(s);
}

static gint mcp_sym_cmp(gconstpointer a, gconstpointer b)
{
    const McpSym *x = *(McpSym *const *)a;
    const McpSym *y = *(McpSym *const *)b;

    if (x->addr != y->addr) {
        return x->addr < y->addr ? -1 : 1;
    }
    return mcp_sym_rank(y) - mcp_sym_rank(x);
}

static int64_t mcp_syms_count(void)
{
    return mcp_syms.all ? mcp_syms.all->len : 0;
}

/*
 * A 64-bit CPU that runs a 32-bit system may extend its addresses with
 * the sign: 0x80123400 in the system's symbol list is 0xffffffff80123400
 * on the CPU.
 */
static bool mcp_is_sext32(uint64_t addr)
{
    return addr >> 31 == 0x1ffffffffULL;
}

/* Seen a CPU run at such an address */
static bool mcp_sext_seen;

/*
 * The 32-bit address @addr as the CPUs use it: extended with the sign
 * if the guest's MMU maps only that, or if a CPU has run at one; else
 * as it is.  Other CPUs extend with zeros, or have 32-bit addresses.
 */
static uint64_t mcp_sym_cpu_addr(uint64_t addr)
{
    uint64_t sext = (uint64_t)(int64_t)(int32_t)addr;
    bool maps = false, maps_sext = false;
    TranslateForDebugResult t;
    CPUState *cpu;

    if (addr >> 31 != 1 || target_long_bits() < 64) {
        return addr;
    }
    CPU_FOREACH(cpu) {
        maps |= cpu_translate_for_debug(cpu, addr, &t);
        maps_sext |= cpu_translate_for_debug(cpu, sext, &t);
        mcp_sext_seen |= mcp_is_sext32(cpu->cc->get_pc(cpu));
    }
    if (maps) {
        return addr;
    }
    return maps_sext || mcp_sext_seen ? sext : addr;
}

/* Where the symbol is, as the CPUs use it; absolute ones are numbers */
static uint64_t mcp_sym_place(const McpSym *s)
{
    return s->type == 'A' || s->type == 'a' ? s->addr
                                            : mcp_sym_cpu_addr(s->addr);
}

/* "panic+0x1c" for @addr, or NULL when no symbol is there */
static char *mcp_sym_name_at(uint64_t addr)
{
    GPtrArray *a = mcp_syms.by_addr;
    const McpSym *s;
    uint64_t size;
    guint lo = 0, hi, i;

    if (!a || !a->len) {
        return NULL;
    }
    /* the first symbol past @addr */
    hi = a->len;
    while (lo < hi) {
        guint mid = lo + (hi - lo) / 2;

        if (((McpSym *)g_ptr_array_index(a, mid))->addr <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (!lo) {
        return NULL;
    }
    /* of the ones at that address, the best comes first */
    i = lo - 1;
    s = g_ptr_array_index(a, i);
    size = s->size;
    while (i > 0 &&
           ((McpSym *)g_ptr_array_index(a, i - 1))->addr == s->addr) {
        s = g_ptr_array_index(a, --i);
        size = MAX(size, s->size);
    }
    if (addr - s->addr >= (size ? size : SYM_NEAR)) {
        return NULL;
    }
    if (addr == s->addr) {
        return g_strdup(s->name);
    }
    return g_strdup_printf("%s+0x%" PRIx64, s->name, addr - s->addr);
}

/* "panic+0x1c" for @addr, or NULL; also for an address with its sign */
static char *mcp_sym_name(uint64_t addr)
{
    char *name = mcp_sym_name_at(addr);

    if (!name && mcp_is_sext32(addr)) {
        name = mcp_sym_name_at((uint32_t)addr);
    }
    return name;
}

/* "0x80123456 <panic+0x1c>", or "0x80123456" when no symbol is there */
static char *mcp_addr_str(uint64_t addr)
{
    g_autofree char *name = mcp_sym_name(addr);

    if (name) {
        return g_strdup_printf("0x%" PRIx64 " <%s>", addr, name);
    }
    return g_strdup_printf("0x%" PRIx64, addr);
}

/* Watched memory: as mcp_addr_str(), or "physical 0x8000" */
static char *mcp_watch_str(uint64_t addr, bool physical)
{
    if (physical) {
        return g_strdup_printf("physical 0x%" PRIx64, addr);
    }
    return mcp_addr_str(addr);
}

/*
 * The symbol called @name, else _name, else .name, as compilers put
 * those in front.  @code: a function goes before data of that name.
 */
static const McpSym *mcp_sym_find(const char *name, bool code)
{
    static const char *const prefix[] = { "", "_", "." };
    const McpSym *found = NULL;
    size_t i;

    if (!mcp_syms.by_name) {
        return NULL;
    }
    for (i = 0; i < ARRAY_SIZE(prefix); i++) {
        g_autofree char *n = g_strconcat(prefix[i], name, NULL);
        const McpSym *s = g_hash_table_lookup(mcp_syms.by_name, n);

        if (s && (!code || mcp_sym_is_code(s))) {
            return s;
        }
        found = found ? found : s;
    }
    return found;
}

/*
 * A symbol with an offset, such as panic, panic+0x1c or panic-4: its
 * symbol into *@sym, and the address.  The offset is hex with 0x, or
 * decimal.
 */
static bool mcp_sym_resolve(const char *s, bool code, const McpSym **sym,
                            uint64_t *addr)
{
    g_autofree char *name = NULL;
    const char *p;
    uint64_t off;

    *sym = mcp_sym_find(s, code);
    if (*sym) {
        *addr = mcp_sym_place(*sym);
        return true;
    }
    for (p = s + strlen(s); p > s && p[-1] != '+' && p[-1] != '-'; p--) {
        /* to the last + or - */
    }
    if (p <= s + 1) {
        return false;
    }
    name = g_strstrip(g_strndup(s, p - 1 - s));
    while (qemu_isspace(*p)) {
        p++;
    }
    if (qemu_strtou64(p, NULL, p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
                      ? 16 : 10, &off) < 0) {
        return false;
    }
    *sym = mcp_sym_find(name, code);
    if (!*sym) {
        return false;
    }
    *addr = mcp_sym_place(*sym);
    *addr = p[-1] == '-' ? *addr - off : *addr + off;
    return true;
}

/*
 * An address: a number (hex with 0x, or decimal), or a symbol with an
 * offset.  @code: it is code, so a function goes before data.
 */
static bool mcp_parse_place(const char *s, const char *what, bool code,
                            uint64_t *addr, Error **errp)
{
    const McpSym *sym;

    if (qemu_isdigit(s[0])) {
        return mcp_parse_address(s, what, addr, errp);
    }
    if (!mcp_syms_count()) {
        error_setg(errp, "%s '%s' is not a number: write hex with 0x, or"
                   " decimal; for names, load symbols with debug-symbols",
                   what, s);
        return false;
    }
    if (!mcp_sym_resolve(s, code, &sym, addr)) {
        error_setg(errp, "%s '%s' is not a number or a symbol: write hex"
                   " with 0x, decimal, or a name that debug-symbols loaded,"
                   " as in panic+0x1c", what, s);
        return false;
    }
    return true;
}

static void mcp_syms_clear(void)
{
    monitor_disas_symbol = NULL;
    g_clear_pointer(&mcp_syms.by_name, g_hash_table_unref);
    g_clear_pointer(&mcp_syms.by_addr, g_ptr_array_unref);
    g_clear_pointer(&mcp_syms.all, g_ptr_array_unref);
    mcp_syms.dup_names = 0;
}

/* False when it has that symbol already */
static bool mcp_sym_add(uint64_t addr, uint64_t size, char type,
                        const char *name)
{
    size_t len = strlen(name);
    McpSym *s, *old;

    if (!mcp_syms.all) {
        mcp_syms.all = g_ptr_array_new_with_free_func(g_free);
        mcp_syms.by_addr = g_ptr_array_new();
        mcp_syms.by_name = g_hash_table_new(g_str_hash, g_str_equal);
    }
    old = g_hash_table_lookup(mcp_syms.by_name, name);
    if (old && old->addr == addr) {
        return false;
    }
    s = g_malloc(sizeof(*s) + len + 1);
    s->addr = addr;
    s->size = size;
    s->type = type;
    memcpy(s->name, name, len + 1);
    g_ptr_array_add(mcp_syms.all, s);
    /* absolute symbols are numbers, not places */
    if (type != 'A' && type != 'a') {
        g_ptr_array_add(mcp_syms.by_addr, s);
    }
    if (old) {
        mcp_syms.dup_names++;
    } else {
        g_hash_table_insert(mcp_syms.by_name, s->name, s);
    }
    return true;
}

/* Hex, with or without 0x */
static bool mcp_sym_hex(const char *s, uint64_t *v)
{
    const char *p = s;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    if (!*p || strlen(p) > 16 || strspn(p, "0123456789abcdefABCDEF") !=
        strlen(p)) {
        return false;
    }
    *v = g_ascii_strtoull(p, NULL, 16);
    return true;
}

/*
 * Whether @num is the address and @other the name, when @num is hex: a
 * name such as "add" or "cafe" is hex too, but nm pads addresses with
 * zeros, so the longer one is the address.
 */
static bool mcp_sym_addr_first(const char *num, const char *other)
{
    uint64_t v;

    return !mcp_sym_hex(other, &v) || strlen(num) >= strlen(other);
}

/* nm's letter for the kind of symbol */
static bool mcp_sym_letter(const char *s)
{
    return s[0] && !s[1] && (qemu_isalpha(s[0]) || s[0] == '?' ||
                             s[0] == '-');
}

/* "0001:00000a10": a segment and an offset, in a Microsoft link map */
static bool mcp_sym_segoff(const char *s)
{
    const char *colon = strchr(s, ':');
    g_autofree char *seg = colon ? g_strndup(s, colon - s) : NULL;
    uint64_t v;

    return colon && mcp_sym_hex(seg, &v) && mcp_sym_hex(colon + 1, &v);
}

typedef enum {
    SYM_LINE_OK,
    SYM_LINE_UNDEFINED,         /* or not a place: a file, a section */
    SYM_LINE_UNKNOWN,
} McpSymLineKind;

typedef struct McpSymLine {
    uint64_t addr;
    uint64_t size;
    char type;
    const char *name;
} McpSymLine;

/*
 * A System V nm table line, with hex values (nm -x):
 * "[45] | 0x10004000 | 0x120 | FUNC | GLOB | 0 | 1 | panic", or the
 * HP-UX one, "panic | 0x00012345 | extern | entry | $CODE$"
 */
static McpSymLineKind mcp_sym_table_line(char **col, int n, McpSymLine *l)
{
    bool local = false, found = false, sized = false;
    char type = '?';
    int i;

    for (i = 0; i < n; i++) {
        const char *c = col[i];

        /* the value, then the size, if the table has one */
        if (!found && g_str_has_prefix(c, "0x") && mcp_sym_hex(c, &l->addr)) {
            found = true;
        } else if (found && !sized && g_str_has_prefix(c, "0x") &&
                   mcp_sym_hex(c, &l->size)) {
            sized = true;
        } else if (!g_ascii_strcasecmp(c, "undef") ||
                   !g_ascii_strcasecmp(c, "file") ||
                   !g_ascii_strcasecmp(c, "sect")) {
            return SYM_LINE_UNDEFINED;
        } else if (!g_ascii_strcasecmp(c, "func") ||
                   !g_ascii_strcasecmp(c, "entry") ||
                   !g_ascii_strcasecmp(c, "code") ||
                   !g_ascii_strcasecmp(c, "millicode")) {
            type = 'T';
        } else if (!g_ascii_strcasecmp(c, "objt") ||
                   !g_ascii_strcasecmp(c, "data")) {
            type = 'D';
        } else if (!g_ascii_strcasecmp(c, "abs") ||
                   !g_ascii_strcasecmp(c, "absolute")) {
            type = type == '?' ? 'A' : type;
        } else if (!g_ascii_strcasecmp(c, "locl") ||
                   !g_ascii_strcasecmp(c, "local") ||
                   !g_ascii_strcasecmp(c, "static")) {
            local = true;
        }
    }
    l->name = col[0][0] == '[' ? col[n - 1] : col[0];
    if (!found || !*l->name || strchr(l->name, ' ')) {
        return SYM_LINE_UNKNOWN;
    }
    l->type = local ? g_ascii_tolower(type) : type;
    return SYM_LINE_OK;
}

/* One line of a symbol list: see debug-symbols in qapi/mcp.json */
static McpSymLineKind mcp_sym_line(char *line, McpSymLine *l)
{
    char *f[7];
    char *p = line;
    int n = 0;

    memset(l, 0, sizeof(*l));
    l->type = '?';
    if (strchr(line, '|')) {
        g_auto(GStrv) col = g_strsplit(line, "|", 0);
        McpSymLineKind k = SYM_LINE_UNKNOWN;

        for (n = 0; col[n]; n++) {
            g_strstrip(col[n]);
        }
        if (n >= 3) {
            k = mcp_sym_table_line(col, n, l);
        }
        /* the name is in @col, which goes with this call */
        if (k == SYM_LINE_OK) {
            l->name = strcpy(line, l->name);
        }
        return k;
    }
    /* the fields, split in place */
    while (n < ARRAY_SIZE(f)) {
        p += strspn(p, " \t");
        if (!*p) {
            break;
        }
        f[n++] = p;
        p += strcspn(p, " \t");
        if (*p) {
            *p++ = '\0';
        }
    }
    if (n >= 3 && mcp_sym_hex(f[0], &l->addr) && mcp_sym_letter(f[1]) &&
        mcp_sym_addr_first(f[0], f[2])) {
        /* ADDR TYPE NAME */
        l->type = f[1][0];
        l->name = f[2];
    } else if (n >= 4 && mcp_sym_hex(f[0], &l->addr) &&
               mcp_sym_hex(f[1], &l->size) && mcp_sym_letter(f[2])) {
        /* ADDR SIZE TYPE NAME */
        l->type = f[2][0];
        l->name = f[3];
    } else if (n >= 3 && mcp_sym_letter(f[1]) &&
               mcp_sym_hex(f[2], &l->addr)) {
        /* NAME TYPE ADDR [SIZE] */
        l->type = f[1][0];
        l->name = f[0];
        if (n >= 4 && !mcp_sym_hex(f[3], &l->size)) {
            l->size = 0;
        }
    } else if (n >= 3 && mcp_sym_segoff(f[0]) &&
               mcp_sym_hex(f[2], &l->addr)) {
        /* a link map: SEG:OFF NAME ADDR [f] [OBJECT] */
        l->type = n >= 4 && !strcmp(f[3], "f") ? 'T' : 'D';
        l->name = f[1];
    } else if (n == 2 && mcp_sym_letter(f[0])) {
        /* "U name" */
        return SYM_LINE_UNDEFINED;
    } else if (n == 2 && mcp_sym_hex(f[0], &l->addr) &&
               mcp_sym_addr_first(f[0], f[1])) {
        /* ADDR NAME */
        l->name = f[1];
    } else if (n == 2 && mcp_sym_hex(f[1], &l->addr)) {
        /* NAME ADDR */
        l->name = f[0];
    } else {
        return SYM_LINE_UNKNOWN;
    }
    if (l->type == 'U' || l->type == 'N' || l->type == '-' ||
        (!l->addr && (l->type == 'w' || l->type == 'v'))) {
        return SYM_LINE_UNDEFINED;
    }
    return SYM_LINE_OK;
}

typedef struct McpSymLoad {
    int64_t loaded, undefined, had, unknown;
    char *first_unknown;        /* the first line not understood */
    bool full;
} McpSymLoad;

static void mcp_syms_load(const char *text, uint64_t base, McpSymLoad *ld)
{
    const char *line, *end;

    for (line = text; *line; line = *end ? end + 1 : end) {
        g_autofree char *copy = NULL;
        McpSymLine l;
        McpSymLineKind k;

        end = qemu_strchrnul(line, '\n');
        copy = g_strstrip(g_strndup(line, end - line));
        if (!*copy || copy[0] == '#') {
            continue;
        }
        if (mcp_syms_count() >= SYM_MAX) {
            ld->full = true;
            break;
        }
        k = mcp_sym_line(copy, &l);
        if (k == SYM_LINE_UNKNOWN) {
            if (!ld->unknown++) {
                /* mcp_sym_line() has cut @copy up */
                g_autofree char *orig = g_strstrip(g_strndup(line,
                                                             end - line));

                ld->first_unknown = g_strndup(orig, SYM_LINE_SHOWN);
            }
            continue;
        }
        if (k == SYM_LINE_UNDEFINED) {
            ld->undefined++;
        } else if (mcp_sym_add(l.addr + base, l.size, l.type, l.name)) {
            ld->loaded++;
        } else {
            ld->had++;
        }
    }
    if (mcp_syms.by_addr) {
        g_ptr_array_sort(mcp_syms.by_addr, mcp_sym_cmp);
    }
    monitor_disas_symbol = mcp_syms_count() ? mcp_sym_name : NULL;
}

/* "panic = 0x80123400 (T, 0x120 bytes)", "0x80123456 = panic+0x56" */
static char *mcp_sym_lookup(const char *q)
{
    const McpSym *sym;
    g_autofree char *name = NULL;
    g_autofree char *size = NULL;
    uint64_t addr;

    if (qemu_isdigit(q[0])) {
        if (mcp_parse_address(q, "address", &addr, NULL)) {
            name = mcp_sym_name(addr);
            return name ? g_strdup_printf("0x%" PRIx64 " = %s", addr, name)
                        : g_strdup_printf("0x%" PRIx64 ": no symbol there",
                                          addr);
        }
        return g_strdup_printf("%s: not a number (hex with 0x, or decimal)",
                               q);
    }
    if (!mcp_sym_resolve(q, false, &sym, &addr)) {
        return g_strdup_printf("%s: no such symbol", q);
    }
    if (sym->size) {
        size = g_strdup_printf(", 0x%" PRIx64 " bytes", sym->size);
    }
    /* "panic+0x10 = 0x80123410 (panic: T, 0x120 bytes)" */
    return g_strdup_printf("%s = 0x%" PRIx64 " (%s%s%c%s)", q, addr,
                           strcmp(q, sym->name) ? sym->name : "",
                           strcmp(q, sym->name) ? ": " : "", sym->type,
                           size ? size : "");
}

McpSymbols *qmp_debug_symbols(const char *file, const char *text,
                              const char *base, bool has_clear, bool clear,
                              bool has_lookup, strList *lookup,
                              Error **errp)
{
    g_autofree char *contents = NULL;
    g_autoptr(GString) s = g_string_new(NULL);
    McpSymLoad ld = { 0 };
    McpSymbols *res;
    strList *l, **tail;
    uint64_t off = 0;
    int n;

    if (file && text) {
        error_setg(errp, "give file or text, not both");
        return NULL;
    }
    if (base && !file && !text) {
        error_setg(errp, "base goes with file or text");
        return NULL;
    }
    if (base && !mcp_parse_address(base, "base", &off, errp)) {
        return NULL;
    }
    for (l = lookup, n = 0; l; l = l->next) {
        if (++n > SYM_LOOKUPS_MAX) {
            error_setg(errp, "look up at most %d at a time", SYM_LOOKUPS_MAX);
            return NULL;
        }
    }
    if (file) {
        g_autoptr(GError) err = NULL;
        struct stat st;
        gsize len;

        if (stat(file, &st) == 0 && st.st_size > SYM_FILE_MAX) {
            error_setg(errp, "%s is over 256 MiB: that is no symbol list",
                       file);
            return NULL;
        }
        if (!g_file_get_contents(file, &contents, &len, &err)) {
            error_setg(errp, "can't read the symbol list: %s", err->message);
            return NULL;
        }
        text = contents;
    }
    if (has_clear && clear) {
        mcp_syms_clear();
    }
    if (text) {
        mcp_syms_load(text, off, &ld);
    }

    res = g_new0(McpSymbols, 1);
    res->loaded = ld.loaded;
    res->count = mcp_syms_count();
    res->skipped = ld.undefined + ld.had + ld.unknown;
    if (text) {
        g_string_printf(s, "Loaded %" PRId64 " symbol%s%s%s", ld.loaded,
                        ld.loaded == 1 ? "" : "s", file ? " from " : "",
                        file ? file : "");
        if (ld.full) {
            g_string_append_printf(s, ", and stopped at %d", SYM_MAX);
        }
        g_string_append_printf(s, "; %" PRId64 " in all", res->count);
    } else if (has_clear && clear) {
        g_string_assign(s, "Forgot all symbols");
    } else {
        g_string_printf(s, "%" PRId64 " symbol%s", res->count,
                        res->count == 1 ? "" : "s");
    }
    if (mcp_syms.by_addr && mcp_syms.by_addr->len) {
        GPtrArray *a = mcp_syms.by_addr;

        g_string_append_printf(s, ", from 0x%" PRIx64 " to 0x%" PRIx64,
                               ((McpSym *)g_ptr_array_index(a, 0))->addr,
                               ((McpSym *)g_ptr_array_index(a, a->len - 1))
                               ->addr);
    }
    g_string_append_c(s, '.');
    if (ld.undefined || ld.had || ld.unknown) {
        g_string_append(s, " Skipped:");
        if (ld.undefined) {
            g_string_append_printf(s, " %" PRId64 " undefined (not in this"
                                   " file) or not a place;", ld.undefined);
        }
        if (ld.had) {
            g_string_append_printf(s, " %" PRId64 " loaded before;", ld.had);
        }
        if (ld.unknown) {
            g_string_append_printf(s, " %" PRId64 " line%s not understood,"
                                   " such as \"%s\";", ld.unknown,
                                   ld.unknown == 1 ? "" : "s",
                                   ld.first_unknown ? ld.first_unknown : "");
        }
        s->str[s->len - 1] = '.';
    }
    g_free(ld.first_unknown);
    if (mcp_syms.dup_names) {
        g_string_append_printf(s, " %" PRId64 " name%s given again at another"
                               " address: a name finds the first.",
                               mcp_syms.dup_names,
                               mcp_syms.dup_names == 1 ? " is" : "s are");
    }
    if (!res->count) {
        g_string_append(s, " Load a list from nm with file or text.");
    }
    tail = &res->lookups;
    for (l = lookup; l; l = l->next) {
        char *line = mcp_sym_lookup(l->value);

        g_string_append_printf(s, "\n%s", line);
        QAPI_LIST_APPEND(tail, line);
    }
    res->has_lookups = !!lookup;
    res->summary = g_strdup(s->str);
    return res;
}

static char *mcp_hex(const uint8_t *buf, size_t len)
{
    char *s = g_malloc(len * 2 + 1);
    size_t i;

    for (i = 0; i < len; i++) {
        sprintf(s + i * 2, "%02x", buf[i]);
    }
    s[len * 2] = '\0';
    return s;
}

/* Hex bytes, with spaces or commas between groups and 0x before them */
static GByteArray *mcp_parse_bytes(const char *s, Error **errp)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    g_auto(GStrv) groups = g_strsplit_set(s, " \t\r\n,", -1);
    int i;

    for (i = 0; groups[i]; i++) {
        const char *g = groups[i];
        size_t len, j;

        if (g[0] == '0' && (g[1] == 'x' || g[1] == 'X')) {
            g += 2;
        }
        len = strlen(g);
        if (len % 2) {
            error_setg(errp, "'%s' is not whole bytes: write two hex digits "
                       "for each byte", groups[i]);
            return NULL;
        }
        for (j = 0; j < len; j += 2) {
            int hi = g_ascii_xdigit_value(g[j]);
            int lo = g_ascii_xdigit_value(g[j + 1]);
            uint8_t v = hi << 4 | lo;

            if (hi < 0 || lo < 0) {
                error_setg(errp, "'%s' is not hex", groups[i]);
                return NULL;
            }
            g_byte_array_append(b, &v, 1);
        }
    }
    if (!b->len || b->len > WRITE_MAX_BYTES) {
        error_setg(errp, "give 1 to %d bytes, not %u", WRITE_MAX_BYTES,
                   b->len);
        return NULL;
    }
    return g_steal_pointer(&b);
}

/*
 * Where the bytes at @addr live: the address space, the physical
 * address and how many of @len bytes stay in that page and region.
 * Only RAM and ROM; a device could act on the access.
 */
static bool mcp_memory_chunk(CPUState *cpu, bool physical, uint64_t addr,
                             size_t len, AddressSpace **as, hwaddr *pa,
                             MemTxAttrs *attrs, size_t *n, Error **errp)
{
    MemoryRegion *mr;
    hwaddr xlat, l;

    if (physical) {
        *as = &address_space_memory;
        *pa = addr;
        *attrs = MEMTXATTRS_UNSPECIFIED;
        *n = len;
    } else {
        TranslateForDebugResult t;
        uint64_t page;

        if (!cpu_translate_for_debug(cpu, addr, &t)) {
            error_setg(errp, "0x%" PRIx64 " is not mapped (CPU %d)", addr,
                       cpu->cpu_index);
            return false;
        }
        page = t.lg_page_size < 64 ? 1ULL << t.lg_page_size : 0;
        *as = cpu_get_address_space(cpu, cpu_asidx_from_attrs(cpu, t.attrs));
        *pa = t.physaddr;
        *attrs = t.attrs;
        *n = page ? MIN(len, page - (addr & (page - 1))) : len;
    }

    RCU_READ_LOCK_GUARD();
    l = *n;
    mr = address_space_translate(*as, *pa, &xlat, &l, true, *attrs);
    if (!memory_region_supports_direct_access(mr) ||
        memory_region_is_ram_device(mr)) {
        error_setg(errp, "0x%" PRIx64 " is not RAM or ROM but %s; devices "
                   "are never written", addr, memory_region_name(mr));
        return false;
    }
    *n = MIN(*n, l);
    return true;
}

McpMemoryWriteResult *qmp_memory_write(const char *address,
                                       const char *bytes,
                                       bool has_physical, bool physical,
                                       bool has_cpu, int64_t cpu_index,
                                       Error **errp)
{
    g_autoptr(GByteArray) data = NULL;
    g_autofree uint8_t *before = NULL;
    g_autofree uint8_t *after = NULL;
    McpMemoryWriteResult *res;
    CPUState *cpu = NULL;
    AddressSpace *as;
    MemTxAttrs attrs;
    uint64_t addr;
    hwaddr pa, first_pa = 0;
    size_t done, n;

    physical = has_physical && physical;
    if (!mcp_parse_place(address, "address", false, &addr, errp)) {
        return NULL;
    }
    data = mcp_parse_bytes(bytes, errp);
    if (!data) {
        return NULL;
    }
    if (!physical) {
        cpu = mcp_debug_cpu(has_cpu, cpu_index, errp);
        if (!cpu) {
            return NULL;
        }
        cpu_synchronize_state(cpu);
    }
    before = g_malloc(data->len);
    after = g_malloc(data->len);

    /* check it all and keep the old bytes, then write */
    for (done = 0; done < data->len; done += n) {
        if (!mcp_memory_chunk(cpu, physical, addr + done, data->len - done,
                              &as, &pa, &attrs, &n, errp)) {
            return NULL;
        }
        if (!done) {
            first_pa = pa;
        }
        address_space_read(as, pa, attrs, before + done, n);
    }
    for (done = 0; done < data->len; done += n) {
        if (!mcp_memory_chunk(cpu, physical, addr + done, data->len - done,
                              &as, &pa, &attrs, &n, errp)) {
            return NULL;
        }
        /* RAM, and ROM too; drops translated code made from the old bytes */
        mcp_dev_self++;
        address_space_write_rom(as, pa, attrs, data->data + done, n);
        mcp_dev_self--;
        address_space_read(as, pa, attrs, after + done, n);
    }

    res = g_new0(McpMemoryWriteResult, 1);
    res->before = mcp_hex(before, data->len);
    res->after = mcp_hex(after, data->len);
    if (physical) {
        res->summary = g_strdup_printf("wrote %u byte%s at physical 0x%"
                                       PRIx64, data->len,
                                       data->len == 1 ? "" : "s", addr);
    } else {
        res->summary = g_strdup_printf("wrote %u byte%s at 0x%" PRIx64
                                       " (physical 0x%" HWADDR_PRIx ")",
                                       data->len, data->len == 1 ? "" : "s",
                                       addr, first_pa);
    }
    if (memcmp(after, data->data, data->len)) {
        g_autofree char *old = res->summary;

        res->summary = g_strdup_printf("%s, but it reads back different "
                                       "bytes (see after)", old);
    }
    return res;
}

/*
 * memory-search.  It copies guest RAM and ROM a piece at a time, never
 * reading a device, and looks for the bytes in the copy.  Every few
 * milliseconds it lets the main loop run, so the VM goes on and the
 * search can be cancelled.
 */

#define SEARCH_MAX_BYTES        256
#define SEARCH_CHUNK            (64 * KiB)
#define SEARCH_LENGTH           (1 * MiB)
#define SEARCH_VIRTUAL_MAX      (4 * GiB)
#define SEARCH_LIST             20
#define SEARCH_LIST_MAX         500
#define SEARCH_COUNT_MAX        100000
#define SEARCH_SHOWN            16      /* bytes shown from a match */
#define SEARCH_SLICE_MS         20      /* then the main loop runs */

/* Where a piece of a copy came from */
typedef struct McpSpan {
    size_t off;                 /* in the copy */
    hwaddr pa;                  /* the physical address of that byte */
    const char *region;
} McpSpan;

/* What to find; letters of text may match in either case */
typedef struct McpPattern {
    uint8_t bytes[SEARCH_MAX_BYTES];    /* lower case where @fold */
    bool fold[SEARCH_MAX_BYTES];
    size_t len;
    bool any_fold;
} McpPattern;

/* RAM or ROM that can be read without asking a device */
static bool mcp_ram_region(const MemoryRegion *mr)
{
    return memory_region_supports_direct_access(mr) && mr->ram_block &&
           !memory_region_is_ram_device(mr) &&
           !memory_region_is_protected(mr);
}

/*
 * Copy RAM and ROM at physical @pa of @as into @buf, up to @len bytes;
 * it stops at the first byte that is neither.  Adds where each piece
 * came from to @spans, if given, with @off added to its place in @buf.
 * Returns how many bytes it copied.
 */
static size_t mcp_ram_copy(AddressSpace *as, hwaddr pa, uint8_t *buf,
                           size_t len, GArray *spans, size_t off)
{
    size_t done = 0;

    while (done < len) {
        MemoryRegionSection s = memory_region_find(as->root, pa + done,
                                                   len - done);
        bool ok = s.mr && s.offset_within_address_space == pa + done &&
                  mcp_ram_region(s.mr);
        size_t n = ok ? int128_get64(s.size) : 0;

        if (ok) {
            McpSpan sp = {
                .off = off + done,
                .pa = pa + done,
                .region = memory_region_name(s.mr),
            };

            /* the reference memory_region_find took keeps the RAM */
            memcpy(buf + done, (uint8_t *)memory_region_get_ram_ptr(s.mr) +
                   s.offset_within_region, n);
            if (spans) {
                g_array_append_val(spans, sp);
            }
        }
        if (s.mr) {
            memory_region_unref(s.mr);
        }
        if (!ok) {
            break;
        }
        done += n;
    }
    return done;
}

/* The first RAM or ROM in [@pa, @end) of @as, if there is any */
static bool mcp_ram_next(AddressSpace *as, hwaddr pa, hwaddr end,
                         hwaddr *next)
{
    while (pa < end) {
        MemoryRegionSection s = memory_region_find(as->root, pa, end - pa);
        hwaddr start = s.offset_within_address_space;
        uint64_t size = s.mr ? int128_get64(s.size) : 0;
        bool ok = s.mr && mcp_ram_region(s.mr);

        if (s.mr) {
            memory_region_unref(s.mr);
        }
        if (!size) {
            return false;
        }
        if (ok) {
            *next = start;
            return true;
        }
        if (size >= end - start) {
            return false;
        }
        pa = start + size;
    }
    return false;
}

static AddressSpace *mcp_debug_as(CPUState *cpu,
                                  const TranslateForDebugResult *t)
{
    return cpu_get_address_space(cpu, cpu_asidx_from_attrs(cpu, t->attrs));
}

/* The size of the page @t is in, or 0 when it is all of memory */
static uint64_t mcp_page_size(const TranslateForDebugResult *t)
{
    if (!t->lg_page_size) {
        return qemu_target_page_size();
    }
    return t->lg_page_size < 64 ? 1ULL << t->lg_page_size : 0;
}

/* How many bytes from @addr on stay in a page of @page bytes */
static uint64_t mcp_page_left(uint64_t addr, uint64_t page, uint64_t len)
{
    return page ? MIN(len, page - (addr & (page - 1))) : len;
}

/*
 * Copy RAM and ROM from @addr on, up to @len bytes: physical, or
 * virtual through the MMU of @cpu.  Stops at the first byte that is not
 * mapped, or is not RAM or ROM.
 */
static size_t mcp_scan_copy(CPUState *cpu, uint64_t addr, uint8_t *buf,
                            size_t len, GArray *spans)
{
    size_t done = 0;

    if (spans) {
        g_array_set_size(spans, 0);
    }
    if (!cpu) {
        return mcp_ram_copy(&address_space_memory, addr, buf, len, spans, 0);
    }
    while (done < len) {
        TranslateForDebugResult t;
        size_t n, got;

        if (!cpu_translate_for_debug(cpu, addr + done, &t)) {
            break;
        }
        n = mcp_page_left(addr + done, mcp_page_size(&t), len - done);
        got = mcp_ram_copy(mcp_debug_as(cpu, &t), t.physaddr, buf + done, n,
                           spans, done);
        done += got;
        if (got < n) {
            break;
        }
    }
    return done;
}

/* Move @addr on to the next byte below @end that a copy can start at */
static bool mcp_scan_skip(CPUState *cpu, uint64_t *addr, uint64_t end)
{
    uint64_t a = *addr;

    if (!cpu) {
        hwaddr next;

        if (!mcp_ram_next(&address_space_memory, a, end, &next)) {
            return false;
        }
        *addr = next;
        return true;
    }
    while (a < end) {
        TranslateForDebugResult t;
        uint64_t page = qemu_target_page_size(), left;
        hwaddr next;

        if (cpu_translate_for_debug(cpu, a, &t)) {
            page = mcp_page_size(&t);
            left = mcp_page_left(a, page, end - a);
            if (mcp_ram_next(mcp_debug_as(cpu, &t), t.physaddr,
                             t.physaddr + left, &next)) {
                *addr = a + (next - t.physaddr);
                return true;
            }
        }
        left = mcp_page_left(a, page, end - a);
        if (left >= end - a) {
            return false;
        }
        a += left;
    }
    return false;
}

static bool mcp_pattern_at(const McpPattern *p, const uint8_t *m)
{
    size_t i;

    for (i = 0; i < p->len; i++) {
        uint8_t c = p->fold[i] ? g_ascii_tolower(m[i]) : m[i];

        if (c != p->bytes[i]) {
            return false;
        }
    }
    return true;
}

/* The first match in @buf that starts in [@from, @starts), else @starts */
static size_t mcp_pattern_find(const McpPattern *p, const uint8_t *buf,
                               size_t from, size_t starts)
{
    while (from < starts) {
        if (!p->fold[0]) {
            const uint8_t *q = memchr(buf + from, p->bytes[0], starts - from);

            if (!q) {
                return starts;
            }
            from = q - buf;
        } else if (g_ascii_tolower(buf[from]) != p->bytes[0]) {
            from++;
            continue;
        }
        if (p->any_fold ? mcp_pattern_at(p, buf + from)
                        : !memcmp(buf + from + 1, p->bytes + 1, p->len - 1)) {
            return from;
        }
        from++;
    }
    return starts;
}

static void mcp_pattern_add(McpPattern *p, uint8_t byte, bool letter)
{
    p->fold[p->len] = letter;
    p->bytes[p->len++] = letter ? g_ascii_tolower(byte) : byte;
    p->any_fold |= letter;
}

static bool mcp_parse_pattern(const char *bytes, const char *text, bool wide,
                              bool fold, bool big_endian, McpPattern *p,
                              Error **errp)
{
    size_t i, len;

    memset(p, 0, sizeof(*p));
    if (bytes) {
        g_autoptr(GByteArray) b = mcp_parse_bytes(bytes, errp);

        if (!b) {
            return false;
        }
        if (b->len > SEARCH_MAX_BYTES) {
            error_setg(errp, "find at most %d bytes, not %u",
                       SEARCH_MAX_BYTES, b->len);
            return false;
        }
        for (i = 0; i < b->len; i++) {
            mcp_pattern_add(p, b->data[i], false);
        }
        return true;
    }
    if (wide) {
        g_autoptr(GError) err = NULL;
        g_autofree gunichar2 *u = NULL;
        glong n;

        u = g_utf8_to_utf16(text, -1, NULL, &n, &err);
        if (!u) {
            error_setg(errp, "text is not UTF-8: %s", err->message);
            return false;
        }
        len = n * 2;
        if (len > SEARCH_MAX_BYTES) {
            error_setg(errp, "find at most %d characters with wide, not %ld",
                       SEARCH_MAX_BYTES / 2, n);
            return false;
        }
        for (i = 0; i < (size_t)n; i++) {
            bool letter = fold && u[i] < 0x80 && g_ascii_isalpha(u[i]);
            uint8_t hi = u[i] >> 8, lo = u[i];

            mcp_pattern_add(p, big_endian ? hi : lo, letter && !big_endian);
            mcp_pattern_add(p, big_endian ? lo : hi, letter && big_endian);
        }
    } else {
        len = strlen(text);
        if (len > SEARCH_MAX_BYTES) {
            error_setg(errp, "find at most %d bytes of text, not %zu",
                       SEARCH_MAX_BYTES, len);
            return false;
        }
        for (i = 0; i < len; i++) {
            mcp_pattern_add(p, text[i], fold && g_ascii_isalpha(text[i]));
        }
    }
    if (!p->len) {
        error_setg(errp, "text is empty");
        return false;
    }
    return true;
}

/* "Press spa.", or with @wide two bytes for each character */
static char *mcp_search_text(const uint8_t *buf, size_t len, bool wide,
                             bool big_endian)
{
    GString *s = g_string_new(NULL);
    size_t i;

    for (i = 0; i + wide < len; i += 1 + wide) {
        unsigned c = !wide ? buf[i] : big_endian ? lduw_be_p(buf + i)
                                                 : lduw_le_p(buf + i);

        g_string_append_c(s, c >= 0x20 && c < 0x7f ? c : '.');
    }
    return g_string_free(s, false);
}

/* "50 72 65 73" */
static char *mcp_search_bytes(const uint8_t *buf, size_t len)
{
    GString *s = g_string_new(NULL);
    size_t i;

    for (i = 0; i < len; i++) {
        g_string_append_printf(s, "%s%02x", i ? " " : "", buf[i]);
    }
    return g_string_free(s, false);
}

/*
 * @value as @size bytes in the byte order the CPU runs in: decimal, hex
 * with 0x, or negative
 */
static bool mcp_parse_value(const char *value, int64_t size, bool big_endian,
                            McpPattern *p, Error **errp)
{
    const char *digits = value + (value[0] == '-');
    int base = digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X') ?
               16 : 10;
    int bits = size * 8;
    uint64_t v;
    int i;

    if (size != 1 && size != 2 && size != 4 && size != 8) {
        error_setg(errp, "size must be 1, 2, 4 or 8");
        return false;
    }
    if (value[0] == '-') {
        int64_t sv;

        if (qemu_strtoi64(value, NULL, base, &sv) < 0) {
            error_setg(errp, "value '%s' is not a number", value);
            return false;
        }
        if (bits < 64 && sv < -(INT64_C(1) << (bits - 1))) {
            error_setg(errp, "value %s does not fit in %" PRId64 " byte%s",
                       value, size, size == 1 ? "" : "s");
            return false;
        }
        v = sv;
    } else {
        if (qemu_strtou64(value, NULL, base, &v) < 0) {
            error_setg(errp, "value '%s' is not a number", value);
            return false;
        }
        if (bits < 64 && v >> bits) {
            error_setg(errp, "value %s does not fit in %" PRId64 " byte%s",
                       value, size, size == 1 ? "" : "s");
            return false;
        }
    }
    memset(p, 0, sizeof(*p));
    for (i = 0; i < size; i++) {
        int shift = big_endian ? (size - 1 - i) * 8 : i * 8;

        mcp_pattern_add(p, v >> shift, false);
    }
    return true;
}

/*
 * The places the last memory-search found, or the last refine kept, for
 * the next refine
 */
static struct {
    bool valid;
    int cpu;                    /* whose MMU maps them, or -1: physical */
    bool wide;                  /* text is shown as 16-bit characters */
    bool capped;                /* the first search stopped counting */
    size_t len;                 /* bytes at each place */
    GArray *places;             /* uint64_t addresses */
    GByteArray *held;           /* @len bytes for each: what it held */
} mcp_search_last;

static void mcp_search_forget(void)
{
    g_clear_pointer(&mcp_search_last.places, g_array_unref);
    g_clear_pointer(&mcp_search_last.held, g_byte_array_unref);
    mcp_search_last.valid = false;
}

static void mcp_search_remember(int cpu, bool wide, bool capped, size_t len,
                                GArray *places, GByteArray *held)
{
    mcp_search_forget();
    mcp_search_last.valid = true;
    mcp_search_last.cpu = cpu;
    mcp_search_last.wide = wide;
    mcp_search_last.capped = capped;
    mcp_search_last.len = len;
    mcp_search_last.places = places;
    mcp_search_last.held = held;
}

/* A match at @at, physical @pa in @region, with the bytes from there */
static McpMemoryMatch *mcp_search_match(CPUState *virt, uint64_t at,
                                        hwaddr pa, const char *region,
                                        bool wide, bool big_endian)
{
    uint8_t shown[SEARCH_SHOWN * 2];
    size_t got = mcp_scan_copy(virt, at, shown, SEARCH_SHOWN << wide, NULL);
    McpMemoryMatch *m = g_new0(McpMemoryMatch, 1);

    m->address = g_strdup_printf("0x%" PRIx64, at);
    if (virt) {
        m->physical = g_strdup_printf("0x%" HWADDR_PRIx, pa);
        m->symbol = mcp_sym_name(at);
    }
    m->region = g_strdup(region);
    m->bytes = mcp_search_bytes(shown, got);
    m->text = mcp_search_text(shown, got, wide, big_endian);
    return m;
}

/* A line for each match: "0x2000 (pc.ram): 48 65 ...  |He...|" */
static void mcp_search_append_matches(GString *s, McpMemoryMatchList *list)
{
    for (; list; list = list->next) {
        McpMemoryMatch *m = list->value;

        g_string_append_printf(s, "%s%s%s%s (%s%s%s%s): %s  |%s|",
                               m->address, m->symbol ? " <" : "",
                               m->symbol ? m->symbol : "",
                               m->symbol ? ">" : "",
                               m->physical ? "physical " : "",
                               m->physical ? m->physical : "",
                               m->physical ? ", " : "", m->region, m->bytes,
                               m->text);
        if (m->before) {
            g_string_append_printf(s, "  was %s", m->before);
        }
        g_string_append_c(s, '\n');
    }
}

/*
 * memory-search with refine: look again at the places the last search
 * found or kept.  With a pattern, keep those that hold it now; else
 * those whose bytes changed (@changed) or did not.
 */
static McpMemorySearchResult *coroutine_fn
mcp_search_refine(const char *bytes, const char *text, const char *value,
                  int64_t size, bool wide, bool ignore_case, bool changed,
                  bool has_place, int64_t max, Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_current_input_ctx();
    g_autoptr(GArray) spans = g_array_new(false, false, sizeof(McpSpan));
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GArray) places = g_array_new(false, false, sizeof(uint64_t));
    g_autoptr(GByteArray) held = g_byte_array_new();
    g_autoptr(GArray) old = NULL;
    g_autoptr(GByteArray) was = NULL;
    g_autofree char *what = NULL;
    McpMemorySearchResult *res;
    McpMemoryMatchList **tail;
    McpPattern pat;
    CPUState *virt = NULL, *cpu;
    bool find = bytes || text || value;
    bool capped = mcp_search_last.capped;
    bool shown_wide, owide;
    size_t olen = mcp_search_last.len, nlen;
    int ocpu;
    int64_t count = 0, listed = 0, slice;
    uint64_t searched = 0;
    guint i;

    if (has_place) {
        error_setg(errp, "refine looks where the last search found its"
                   " bytes: give no address, length, physical or cpu");
        return NULL;
    }
    if (!mcp_search_last.valid) {
        error_setg(errp, "refine needs a memory-search before it: it looks"
                   " again where that one found its bytes");
        return NULL;
    }
    if (!mcp_search_last.places->len) {
        error_setg(errp, "the last search found nothing to refine: search"
                   " again without refine");
        return NULL;
    }
    if (mcp_search_last.cpu >= 0) {
        virt = qemu_get_cpu(mcp_search_last.cpu);
        if (!virt) {
            error_setg(errp, "CPU %d, whose memory the last search looked"
                       " at, is gone", mcp_search_last.cpu);
            return NULL;
        }
    }
    /* the byte order the CPU runs in, for a value and for wide text */
    cpu = virt ? virt : mcp_debug_cpu(false, 0, errp);
    if (!cpu) {
        return NULL;
    }
    cpu_synchronize_state(cpu);
    if (value ? !mcp_parse_value(value, size, cpu_internal_is_big_endian(cpu),
                                 &pat, errp)
              : find && !mcp_parse_pattern(bytes, text, wide, ignore_case,
                                           cpu_internal_is_big_endian(cpu),
                                           &pat, errp)) {
        return NULL;
    }
    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }
    nlen = find ? pat.len : olen;
    shown_wide = find ? wide : mcp_search_last.wide;
    if (find) {
        g_autofree char *hex = mcp_search_bytes(pat.bytes, pat.len);

        what = value ? g_strdup_printf("%s (%s)", value, hex)
             : text ? g_strdup_printf("\"%s\"", text)
             : g_steal_pointer(&hex);
    }

    /* the places are this call's now; another search may come meanwhile */
    ocpu = mcp_search_last.cpu;
    owide = mcp_search_last.wide;
    old = g_steal_pointer(&mcp_search_last.places);
    was = g_steal_pointer(&mcp_search_last.held);
    mcp_search_forget();

    res = g_new0(McpMemorySearchResult, 1);
    tail = &res->matches;
    slice = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    for (i = 0; i < old->len; i++) {
        uint64_t at = g_array_index(old, uint64_t, i);
        const uint8_t *before = was->data + i * olen;
        uint8_t now[SEARCH_MAX_BYTES];
        const McpSpan *sp;
        McpMemoryMatch *m;
        size_t got;

        if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - slice >=
            SEARCH_SLICE_MS) {
            qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                      SCALE_MS);
            slice = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        }
        if (ctx->cancelled) {
            /* the places stay for the next refine, unless replaced */
            if (!mcp_search_last.valid) {
                mcp_search_remember(ocpu, owide, capped, olen,
                                    g_steal_pointer(&old),
                                    g_steal_pointer(&was));
            }
            qapi_free_McpMemorySearchResult(res);
            error_setg(errp, "cancelled");
            return NULL;
        }
        /* a place that is no longer RAM or ROM, or not mapped, goes */
        got = mcp_scan_copy(virt, at, now, nlen, spans);
        searched += got;
        if (got < nlen ||
            (find ? !mcp_pattern_at(&pat, now)
                  : (memcmp(now, before, olen) == 0) == changed)) {
            continue;
        }
        g_array_append_val(places, at);
        g_byte_array_append(held, now, nlen);
        if (++count > max) {
            continue;
        }
        sp = &g_array_index(spans, McpSpan, 0);
        m = mcp_search_match(virt, at, sp->pa, sp->region, shown_wide,
                             cpu_internal_is_big_endian(cpu));
        m->before = mcp_search_bytes(before, olen);
        QAPI_LIST_APPEND(tail, m);
        listed++;
    }
    res->count = count;
    res->searched = searched;
    res->has_places = true;
    res->places = old->len;
    mcp_search_remember(virt ? virt->cpu_index : -1, shown_wide, capped, nlen,
                        g_steal_pointer(&places), g_steal_pointer(&held));

    if (count) {
        g_string_printf(s, "Kept %" PRId64 " of the %u place%s the last"
                        " search found; %s ", count, old->len,
                        old->len == 1 ? "" : "s", count == 1 ? "it" : "they");
        if (what) {
            g_string_append_printf(s, "hold%s %s now", count == 1 ? "s" : "",
                                   what);
        } else {
            g_string_append(s, changed ? "changed" : "stayed the same");
        }
        if (listed < count) {
            g_string_append_printf(s, ". The first %" PRId64, listed);
        }
        g_string_append(s, listed ? ":\n" : ".");
    } else {
        if (old->len == 1) {
            g_string_assign(s, "The place the last search found ");
        } else {
            g_string_printf(s, "None of the %u places the last search found ",
                            old->len);
        }
        if (what) {
            g_string_append_printf(s, "%s %s now", old->len == 1 ?
                                   "does not hold" : "holds", what);
        } else if (old->len == 1) {
            g_string_append(s, changed ? "did not change" : "changed");
        } else {
            g_string_append(s, changed ? "changed" : "stayed the same");
        }
        g_string_append(s, ". To start again, search without refine.");
    }
    mcp_search_append_matches(s, res->matches);
    if (capped) {
        g_string_append(s, s->str[s->len - 1] == '\n' ? "\n" : "\n\n");
        g_string_append_printf(s, "The first search stopped counting at %d"
                               " places, so the ones past those were not"
                               " looked at.", SEARCH_COUNT_MAX);
    }
    if (count > 1) {
        g_string_append(s, s->str[s->len - 1] == '\n' ? "\n" : "\n\n");
        g_string_append(s, "Refine again when the value changes again.");
    }
    res->summary = g_strdup(g_strchomp(s->str));
    return res;
}

McpMemorySearchResult *coroutine_fn
qmp_memory_search(const char *bytes, const char *text, const char *value,
                  bool has_size, int64_t size, bool has_wide, bool wide,
                  bool has_ignore_case, bool ignore_case, bool has_refine,
                  bool refine, bool has_changed, bool changed,
                  const char *address, bool has_length, uint64_t length,
                  bool has_physical, bool physical, bool has_cpu,
                  int64_t cpu_index, bool has_max, int64_t max, Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_current_input_ctx();
    g_autoptr(GArray) spans = g_array_new(false, false, sizeof(McpSpan));
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GArray) places = g_array_new(false, false, sizeof(uint64_t));
    g_autoptr(GByteArray) held = g_byte_array_new();
    g_autofree uint8_t *buf = NULL;
    g_autofree char *where = NULL;
    g_autofree char *amount = NULL;
    McpMemorySearchResult *res;
    McpMemoryMatchList **tail;
    McpPattern pat;
    CPUState *cpu = NULL, *virt = NULL;
    uint64_t start = 0, end = UINT64_MAX, pos, searched = 0;
    int64_t count = 0, listed = 0, slice;
    bool big_endian = target_big_endian();
    int finds = !!bytes + !!text + !!value;
    size_t want;

    wide = has_wide && wide;
    ignore_case = has_ignore_case && ignore_case;
    refine = has_refine && refine;
    physical = has_physical && physical;
    max = has_max ? max : SEARCH_LIST;
    if (finds > 1) {
        error_setg(errp, "give one of bytes, text and value, not more");
        return NULL;
    }
    if (!finds && !refine) {
        error_setg(errp, "give bytes, text or value to find");
        return NULL;
    }
    if (!text && (wide || ignore_case)) {
        error_setg(errp, "wide and ignore-case go with text");
        return NULL;
    }
    if (has_size && !value) {
        error_setg(errp, "size goes with value");
        return NULL;
    }
    if (has_changed && (!refine || finds)) {
        error_setg(errp, "changed goes with refine, when there is nothing"
                   " to find");
        return NULL;
    }
    if (max < 0 || max > SEARCH_LIST_MAX) {
        error_setg(errp, "max must be 0 to %d", SEARCH_LIST_MAX);
        return NULL;
    }
    if (refine) {
        return mcp_search_refine(bytes, text, value, has_size ? size : 4,
                                 wide, ignore_case,
                                 !has_changed || changed,
                                 address || has_length || has_physical ||
                                 has_cpu, max, errp);
    }
    if (address) {
        if (!mcp_parse_place(address, "address", false, &start, errp)) {
            return NULL;
        }
        length = has_length ? length : SEARCH_LENGTH;
        if (!length || length - 1 > UINT64_MAX - start) {
            error_setg(errp, "length must be at least 1 and stay below "
                       "2^64");
            return NULL;
        }
        if (!physical && length > SEARCH_VIRTUAL_MAX) {
            error_setg(errp, "search at most 4 GiB of virtual memory at a "
                       "time");
            return NULL;
        }
        /* the end, past the last byte; 2^64 is UINT64_MAX, one short */
        end = length > UINT64_MAX - start ? UINT64_MAX : start + length;
    } else if (has_length) {
        error_setg(errp, "length goes with address");
        return NULL;
    }
    /* the byte order the CPU runs in, for a value and for wide text */
    if ((address && !physical) || wide || value) {
        cpu = mcp_debug_cpu(has_cpu, cpu_index, errp);
        if (!cpu) {
            return NULL;
        }
        cpu_synchronize_state(cpu);
        big_endian = cpu_internal_is_big_endian(cpu);
    }
    if (address && !physical) {
        virt = cpu;
    }
    if (value ? !mcp_parse_value(value, has_size ? size : 4, big_endian,
                                 &pat, errp)
              : !mcp_parse_pattern(bytes, text, wide, ignore_case,
                                   big_endian, &pat, errp)) {
        return NULL;
    }
    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }

    res = g_new0(McpMemorySearchResult, 1);
    tail = &res->matches;
    buf = g_malloc(SEARCH_CHUNK + pat.len - 1);
    slice = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    for (pos = start; pos < end && count < SEARCH_COUNT_MAX; ) {
        size_t n, starts, adv, o;
        bool full;

        if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - slice >=
            SEARCH_SLICE_MS) {
            qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                      SCALE_MS);
            slice = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        }
        if (ctx->cancelled) {
            qapi_free_McpMemorySearchResult(res);
            error_setg(errp, "cancelled");
            return NULL;
        }
        want = MIN(SEARCH_CHUNK + pat.len - 1, end - pos);
        n = mcp_scan_copy(virt, pos, buf, want, spans);
        if (!n) {
            uint64_t from = pos;

            if (!mcp_scan_skip(virt, &pos, end)) {
                break;
            }
            /* never stand still */
            pos = MAX(pos, from + 1);
            continue;
        }
        /* a match must fit; the last bytes of a full copy come again */
        full = n == SEARCH_CHUNK + pat.len - 1;
        starts = full ? SEARCH_CHUNK : n >= pat.len ? n - pat.len + 1 : 0;
        adv = full ? SEARCH_CHUNK : n;
        for (o = mcp_pattern_find(&pat, buf, 0, starts); o < starts;
             o = mcp_pattern_find(&pat, buf, o + 1, starts)) {
            const McpSpan *sp = NULL;
            uint64_t at = pos + o;
            size_t k;

            /* for a refine: where, and what it holds */
            g_array_append_val(places, at);
            g_byte_array_append(held, buf + o, pat.len);
            if (++count > max) {
                if (count >= SEARCH_COUNT_MAX) {
                    break;
                }
                continue;
            }
            for (k = 0; k < spans->len; k++) {
                if (g_array_index(spans, McpSpan, k).off <= o) {
                    sp = &g_array_index(spans, McpSpan, k);
                }
            }
            QAPI_LIST_APPEND(tail, mcp_search_match(virt, at,
                                                    sp->pa + (o - sp->off),
                                                    sp->region, wide,
                                                    big_endian));
            listed++;
        }
        searched += adv;
        pos += adv;
    }
    res->count = count;
    res->searched = searched;
    mcp_search_remember(virt ? virt->cpu_index : -1, wide,
                        count >= SEARCH_COUNT_MAX, pat.len,
                        g_steal_pointer(&places), g_steal_pointer(&held));

    amount = size_to_str(searched);
    if (!address) {
        where = g_strdup_printf("all RAM and ROM (%s)", amount);
    } else {
        where = g_strdup_printf("%s0x%" PRIx64 "..0x%" PRIx64 " (%s of it %s"
                                "RAM or ROM)", virt ? "" : "physical ",
                                start, end - 1, amount, virt ? "mapped " : "");
    }
    if (value) {
        g_autofree char *hex = mcp_search_bytes(pat.bytes, pat.len);

        /* the byte order the CPU runs in */
        g_string_printf(s, "%s is %s in memory. ", value, hex);
    }
    if (!count) {
        g_string_append_printf(s, "Not found in %s", where);
    } else {
        g_string_append_printf(s, "Found %" PRId64 "%s in %s", count,
                               count >= SEARCH_COUNT_MAX ? " or more"
                               " (counting stops there)" : "", where);
        if (listed < count) {
            g_string_append_printf(s, "; the first %" PRId64, listed);
        }
    }
    g_string_append(s, listed ? ":\n" : ".");
    mcp_search_append_matches(s, res->matches);
    if (count > 1 && !text) {
        g_string_append(s, listed ? "\n" : "\n\n");
        g_string_append(s, "To narrow them down, let the guest change the"
                        " value, then search for the new one with refine:"
                        " true.");
    }
    res->summary = g_strdup(g_strchomp(s->str));
    return res;
}

#ifdef CONFIG_TCG

#define RUN_DEFAULT_MS          30000
#define RUN_HITS_MAX            1000000
#define RUN_ADDRS_MAX           64
#define RUN_WATCHES_MAX         16
#define RUN_LISTED              8       /* addresses a summary names */
#define STEP_MAX                10000
#define STEP_SHOWN              200
#define STEP_WAIT_MS            1000
#define MEMORY_WORDS            8
#define MEMORY_WORDS_MAX        256
#define MEMORY_RANGES_MAX       16
#define PROFILE_DEFAULT_MS      5000
#define PROFILE_MIN_MS          100
#define PROFILE_TOP             20
#define PROFILE_TOP_MAX         100

/*
 * One debug-run or debug-step at a time.  Its context is woken when the
 * VM stops, so it need not poll.
 */
static struct {
    bool busy;
    QemuInputCtx *ctx;
    QEMUBH *wake_bh;
} mcp_debug;

static void mcp_debug_wake_bh(void *opaque)
{
    if (mcp_debug.ctx) {
        qemu_co_sleep_wake(&mcp_debug.ctx->sleep);
    }
}

/*
 * Wake the tool from a BH, not from here: the VM is in the middle of
 * stopping, and the other handlers have yet to run.
 */
static void mcp_debug_vm_state(void *opaque, bool running, RunState state)
{
    if (!running && mcp_debug.ctx) {
        qemu_bh_schedule(mcp_debug.wake_bh);
    }
}

static bool mcp_debug_begin(QemuInputCtx *ctx, Error **errp)
{
    RunState state = runstate_get();

    if (!tcg_enabled()) {
        error_setg(errp, "the debug tools need TCG (-accel tcg)");
        return false;
    }
    if (replay_mode != REPLAY_MODE_NONE) {
        error_setg(errp, "the debug tools don't work while QEMU records or "
                   "replays");
        return false;
    }
    if (mcp_debug.busy) {
        error_setg(errp, "another debug-run or debug-step is running");
        return false;
    }
    switch (state) {
    case RUN_STATE_RUNNING:
    case RUN_STATE_PAUSED:
    case RUN_STATE_DEBUG:
    case RUN_STATE_PRELAUNCH:
        break;
    default:
        error_setg(errp, "the VM is %s; the debug tools need it running or "
                   "paused", RunState_str(state));
        return false;
    }
    if (!mcp_debug.wake_bh) {
        mcp_debug.wake_bh = qemu_bh_new(mcp_debug_wake_bh, NULL);
        qemu_add_vm_change_state_handler(mcp_debug_vm_state, NULL);
    }
    mcp_debug.busy = true;
    mcp_debug.ctx = ctx;
    mcp_quiet_runstate_events();
    return true;
}

static void mcp_debug_end(void)
{
    mcp_debug.busy = false;
    mcp_debug.ctx = NULL;
}

typedef struct McpDebugBh {
    void (*fn)(void *opaque);
    void *opaque;
    Coroutine *co;
} McpDebugBh;

static void mcp_debug_bh(void *opaque)
{
    McpDebugBh *b = opaque;

    b->fn(b->opaque);
    aio_co_wake(b->co);
}

/*
 * Run @fn in the main loop, outside this coroutine: vm_stop() waits for
 * the vCPUs and flushes the disks.  QMP runs its commands that are not
 * coroutines the same way.
 */
static void coroutine_fn mcp_debug_outside(void (*fn)(void *opaque),
                                           void *opaque)
{
    McpDebugBh b = { fn, opaque, qemu_coroutine_self() };

    aio_bh_schedule_oneshot(qemu_get_aio_context(), mcp_debug_bh, &b);
    qemu_coroutine_yield();
}

static void mcp_debug_stop(void *opaque)
{
    vm_stop(*(RunState *)opaque);
}

static void mcp_debug_start(void *opaque)
{
    vm_start();
}

/* Wait until the VM stops.  False when the time ran out or on cancel. */
static bool coroutine_fn mcp_debug_wait_stop(QemuInputCtx *ctx, int64_t end,
                                             CPUState *step_cpu)
{
    int halted = 0;

    while (runstate_is_running()) {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

        if (ctx->cancelled || now >= end) {
            return false;
        }
        /*
         * A step that halts the CPU waits for an interrupt, and a step
         * keeps interrupts away.
         */
        if (step_cpu) {
            halted = qatomic_read(&step_cpu->halted) ? halted + 1 : 0;
            if (halted > 1) {
                return false;
            }
        }
        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  MIN(step_cpu ? 10 : 1000, end - now)
                                  * SCALE_MS);
    }
    return true;
}

typedef enum {
    STEP_DONE,          /* the VM stopped after one instruction */
    STEP_HALTED,        /* the CPU halted; the VM was stopped */
    STEP_STOPPED,       /* the VM stopped for another reason */
    STEP_CANCELLED,     /* the VM was stopped */
    STEP_NOT_STARTED,
} McpStepResult;

typedef struct McpStep {
    CPUState *cpu;
    int ret;
} McpStep;

/* What gdbstub does to step: only this CPU runs, until its debug stop */
static void mcp_debug_step_start(void *opaque)
{
    McpStep *st = opaque;

    st->ret = vm_prepare_start(true);
    if (st->ret) {
        return;
    }
    cpu_single_step(st->cpu, SSTEP_ENABLE | SSTEP_NOIRQ | SSTEP_NOTIMER);
    cpu_resume(st->cpu);
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
}

/* One instruction on @cpu, with the VM stopped before and after */
static McpStepResult coroutine_fn mcp_debug_step(QemuInputCtx *ctx,
                                                 CPUState *cpu)
{
    McpStep st = { .cpu = cpu };
    RunState debug = RUN_STATE_DEBUG;
    bool stopped;

    mcp_debug_outside(mcp_debug_step_start, &st);
    if (st.ret) {
        return STEP_NOT_STARTED;
    }
    stopped = mcp_debug_wait_stop(ctx, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                                  + STEP_WAIT_MS, cpu);
    if (!stopped) {
        mcp_debug_outside(mcp_debug_stop, &debug);
    }
    cpu_single_step(cpu, 0);
    if (!stopped) {
        return ctx->cancelled ? STEP_CANCELLED : STEP_HALTED;
    }
    return runstate_check(RUN_STATE_DEBUG) ? STEP_DONE : STEP_STOPPED;
}

/* The address of a breakpoint at @addr, as the CPU compares it */
static vaddr mcp_debug_bp_pc(CPUState *cpu, uint64_t addr)
{
    if (cpu->cc->gdb_adjust_breakpoint) {
        return cpu->cc->gdb_adjust_breakpoint(cpu, addr);
    }
    return addr;
}

/* The thread a device's access to watched memory came in */
typedef enum McpDevFrom {
    DEV_FROM_CPU,               /* a CPU's I/O access, or QEMU for a CPU */
    DEV_FROM_MAIN,              /* the main loop: timers, transfers ending */
    DEV_FROM_IOTHREAD,
    DEV_FROM_OTHER,
} McpDevFrom;

#define DEV_REGION_MAX          64

/*
 * What the device side saw: a device's write to watched RAM (DMA), or
 * an access to watched registers of a device, whoever made it
 */
typedef struct McpDevHit {
    int area;                   /* the watched area */
    uint64_t addr;              /* the first watched byte, as watched */
    bool has_phys;
    uint64_t phys;              /* that byte's physical address */
    uint64_t len;               /* RAM: all it wrote; registers: the size */
    bool reg;                   /* registers, not RAM */
    bool write;
    /* registers: */
    uint64_t value;             /* what was read or written */
    uint64_t offset;            /* where the access began in its region */
    char region[DEV_REGION_MAX]; /* the region: its name, its owner's type */
    bool nothing;               /* nothing is mapped there */
    bool ok;                    /* the region took the access */
    McpDevFrom from;
    int cpu;                    /* DEV_FROM_CPU: which */
} McpDevHit;

/* What touched watched memory */
typedef struct McpWatchSeen {
    CPUState *cpu;
    int area;                   /* the watched area */
    uint64_t addr;              /* the first watched byte, as watched */
    /*
     * The instruction that did it, when @by_insn: a CPU's watchpoint.
     * Else the CPU stopped there, after it.
     */
    uint64_t pc;
    bool by_insn;
    bool write;
    bool device;                /* no CPU did it: @dev says what */
    bool has_dev;               /* @dev holds what the device side saw */
    McpDevHit dev;
} McpWatchSeen;

#define SPACE_NAME_MAX          32

/* Memory that a debug tool watches */
typedef struct McpWatchArea {
    uint64_t addr;
    uint64_t len;
    int flags;                  /* BP_MEM_READ, BP_MEM_WRITE or both */
    bool physical;              /* @addr is physical */
    /* @addr is in this other address space, which no CPU watches */
    AddressSpace *as;
    char space[SPACE_NAME_MAX];
    bool devices;               /* devices' accesses too */
    /* what it was as the run began */
    bool has_ram;               /* RAM that devices' writes are seen to */
    bool has_regs;              /* devices' registers */
    bool anything;              /* RAM, ROM or a device */
} McpWatchArea;

/*
 * A CPU watchpoint: on a watched area, or on where a CPU sees registers
 * of an area in another address space, such as I/O ports in a window
 */
typedef struct McpCpuWatch {
    uint64_t addr;
    uint64_t len;
    int flags;                  /* with BP_GDB */
    int area;
    uint64_t at;                /* @addr as the area has it */
} McpCpuWatch;

#define RUN_CW_MAX              (RUN_WATCHES_MAX * 4)

/* Whether CPUs watch @a, with a watchpoint */
static bool mcp_watch_by_cpu(const McpWatchArea *a)
{
    return !a->as;
}

/* The flags of the CPU watchpoint on @a, besides BP_GDB */
static int mcp_watch_flags(const McpWatchArea *a)
{
    return a->flags | (a->physical ? BP_PHYS : 0);
}

/* "0x2000 <buf>", "physical 0x2000" or "I/O 0x60", as @a has it */
static char *mcp_area_str(const McpWatchArea *a, uint64_t addr)
{
    if (a->as) {
        return g_strdup_printf("%s 0x%" PRIx64, a->space, addr);
    }
    return mcp_watch_str(addr, a->physical);
}

/* An exception a CPU stopped for */
typedef struct McpExcpSeen {
    CPUState *cpu;
    int32_t number;
    uint64_t from;              /* the instruction that took it */
    uint64_t to;                /* the guest's handler */
} McpExcpSeen;

/*
 * A condition on the hits of debug-run and debug-trace, such as
 * "eax == 5 && u32[sp+4] != 0": numbers, registers, symbols, memory read
 * with u8[...] to u64[...], the low bits of a value with u8(...) to
 * u64(...), and C's operators, on 64-bit numbers.  They are unsigned;
 * s8 to s64 in place of u8 to u64 extend the sign and make them signed,
 * and a comparison, /, % or >> with a signed side works signed.  The
 * CPU's own thread works it out at each hit (debug-cond.h), so it reads
 * only that CPU and memory: symbols and register names become numbers
 * as it is parsed.
 */

#define COND_TEXT_MAX           512
#define COND_NODES_MAX          128
#define COND_DEPTH_MAX          32

typedef enum McpCondOp {
    COND_NUM, COND_PC, COND_REG, COND_MEM, COND_CONV,
    COND_NEG, COND_NOT, COND_INV,
    COND_MUL, COND_DIV, COND_MOD, COND_ADD, COND_SUB, COND_SHL, COND_SHR,
    COND_LT, COND_LE, COND_GT, COND_GE, COND_EQ, COND_NE,
    COND_AND, COND_XOR, COND_OR, COND_LAND, COND_LOR,
} McpCondOp;

typedef struct McpCondNode {
    McpCondOp op;
    int size;                   /* COND_MEM, COND_CONV: bytes, < 0 signed */
    bool sgn;                   /* its value is signed */
    int a, b;                   /* the operands, as node numbers */
    uint64_t num;               /* COND_NUM: it; COND_REG: GDB's number */
} McpCondNode;

typedef enum McpCondFail {
    COND_OK,
    COND_NO_MEMORY,             /* at @where */
    COND_NO_REGISTER,           /* GDB's number @where */
    COND_ZERO_DIVIDE,
} McpCondFail;

typedef struct McpCond {
    CPUDebugCond base;
    CPUState *cpu;              /* one like the CPUs, for register names */
    McpCondNode node[COND_NODES_MAX];
    int n_nodes;
    int root;
    /* the CPUs' threads count as they work it out */
    uint64_t n_false;
    uint64_t n_failed;
    int failed;                 /* @fail and @where are set */
    McpCondFail fail;           /* the first time it could not */
    uint64_t where;
    /*
     * By CPU index: the pc + 1 where that CPU's thread last found that it
     * holds, or 0.  A CPU that stopped for it stopped rightly, even if
     * another CPU has changed its memory since.
     */
    int n_met;
    uint64_t *met;
} McpCond;

static bool mcp_cond_test(CPUState *cpu, CPUDebugCond *base);
static bool mcp_cond_holds(McpCond *c, CPUState *cpu);

typedef struct McpBreak {
    CPUState *only;
    int n_addrs;                /* the addresses to stop at */
    uint64_t addrs[RUN_ADDRS_MAX];
    bool has_mask;
    uint64_t mask;              /* the bits of each address compared */
    int hits;                   /* CPUs there as the time ran out */
    int n_watches;              /* the watched memory */
    McpWatchArea watches[RUN_WATCHES_MAX];
    int n_cw;                   /* the CPU watchpoints on it */
    McpCpuWatch cw[RUN_CW_MAX];
    CPUState *wcpu;             /* whose mappings the watched addresses use */
    int whits;                  /* CPUs that touched it as the time ran out */
    McpWatchSeen seen;
    int dhits;                  /* devices' accesses as the time ran out */
    /* the first watched byte devices' accesses are not seen at, and why */
    const char *dev_skip;       /* NULL when there is none */
    uint64_t dev_skip_at;
    int dev_skip_area;
    /* the first watched area where nothing was as the run began */
    bool dev_none;
    int dev_none_area;
    bool has_excp;              /* the exceptions to stop at */
    CPUExcpWatch excp;          /* each CPU watched gets a copy */
    int ehits;                  /* CPUs that took one as the time ran out */
    McpExcpSeen eseen;
    McpCond *cond;              /* a hit counts only when this holds */
} McpBreak;

/*
 * Whether the hit of @cpu, stopped, meets the condition of @b, if it has
 * one.  Not counted: its thread counted it.
 */
static bool mcp_debug_cond(McpBreak *b, CPUState *cpu)
{
    return !b->cond || mcp_cond_holds(b->cond, cpu);
}

/*
 * Which address of @b @cpu stands at, about to run the instruction
 * there, or -1 when none or the CPU is not watched.  A halted CPU there
 * has not got there yet: it goes on with an interrupt.
 */
static int mcp_debug_addr_index(McpBreak *b, CPUState *cpu)
{
    vaddr pc;
    int i;

    if (!b->n_addrs || (b->only && cpu != b->only) || cpu->halted) {
        return -1;
    }
    pc = cpu->cc->get_pc(cpu);
    for (i = 0; i < b->n_addrs; i++) {
        if (((pc ^ mcp_debug_bp_pc(cpu, b->addrs[i])) & b->mask) == 0) {
            return i;
        }
    }
    return -1;
}

static bool mcp_debug_is_at(McpBreak *b, CPUState *cpu)
{
    return mcp_debug_addr_index(b, cpu) >= 0;
}

/* At the address, and the condition holds: an arrival */
static bool mcp_debug_is_hit(McpBreak *b, CPUState *cpu)
{
    return mcp_debug_is_at(b, cpu) && mcp_debug_cond(b, cpu);
}

/* The address a CPU of @b stopped at, as the summary shows it */
static uint64_t mcp_debug_reached(McpBreak *b, CPUState *cpu)
{
    int i = mcp_debug_addr_index(b, cpu);

    return b->has_mask || i < 0 ? cpu->cc->get_pc(cpu) : b->addrs[i];
}

/* Whether debug-trace counts the arrivals by address */
static bool mcp_debug_by_addr(McpBreak *b)
{
    return b->has_mask || b->n_addrs > 1;
}

/*
 * "a", "a or b", "a, b or c"; past RUN_LISTED of them, the first ones
 * and "or 9 more".  @items are freed.
 */
static void mcp_append_list(GString *s, GPtrArray *items, const char *conj)
{
    guint n = items->len;
    guint shown = n > RUN_LISTED ? RUN_LISTED - 1 : n;
    guint i;

    for (i = 0; i < shown; i++) {
        if (i && i == n - 1) {
            g_string_append_printf(s, " %s ", conj);
        } else if (i) {
            g_string_append(s, ", ");
        }
        g_string_append(s, g_ptr_array_index(items, i));
    }
    if (shown < n) {
        g_string_append_printf(s, " %s %u more", conj, n - shown);
    }
    g_ptr_array_free(items, true);
}

/* "0x1000", "0x1000 or 0x2000", then " (mask 0xffff)" */
static void mcp_debug_append_addrs(GString *s, McpBreak *b, const char *conj)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    int i;

    for (i = 0; i < b->n_addrs; i++) {
        g_ptr_array_add(items, mcp_addr_str(b->addrs[i]));
    }
    mcp_append_list(s, items, conj);
    if (b->has_mask) {
        g_string_append_printf(s, " (mask 0x%" PRIx64 ")", b->mask);
    }
}

/*
 * The device side of watched memory.  Devices' writes to watched RAM
 * (DMA), and QEMU's for a CPU, as for a page table: physmem calls
 * mcp_dma_hook after each write to RAM that is not a CPU's own store.
 * Accesses to watched registers of a device, memory-mapped or in
 * another address space such as the I/O ports, whoever makes them:
 * memory.c calls mcp_io_hook for each access to memory that is not RAM.
 * The watched areas become RAM addresses and pieces of devices' regions
 * as a run begins, with the VM stopped, so an access that reaches them
 * through an alias, a bus window or an IOMMU counts too.  The hooks run
 * in the thread that made the access: they note it and ask the VM to
 * stop, and the run takes what they noted.  An access that a CPU's
 * watchpoint caught stops the VM anyway: the hook only keeps what it
 * did for that hit.
 */
#define DEV_RANGES_MAX          (RUN_WATCHES_MAX * 8)
#define DEV_PIECES_MAX          (RUN_WATCHES_MAX * 8)
#define DEV_PENDING_MAX         64

/* Watched RAM */
typedef struct McpDmaRange {
    uint64_t ram;               /* the ram_addr_t of its first byte */
    uint64_t len;
    uint64_t addr;              /* that byte as watched */
    uint64_t phys;
    int area;
} McpDmaRange;

/* Watched registers: a piece of a device's region */
typedef struct McpIoPiece {
    MemoryRegion *mr;           /* holds a reference */
    uint64_t off;               /* the first watched byte, in @mr */
    uint64_t len;
    uint64_t addr;              /* that byte as watched */
    bool has_phys;
    uint64_t phys;              /* its physical address */
    int area;
    int flags;                  /* BP_MEM_READ, BP_MEM_WRITE or both */
    bool devices;               /* accesses by no CPU count too */
    bool nothing;               /* nothing is mapped there */
    char region[DEV_REGION_MAX];
} McpIoPiece;

/*
 * What a CPU's watchpoint does not see: the last register read ([0])
 * and write ([1]) of the instruction it caught
 */
typedef struct McpIoSeen {
    bool has[2];
    McpDevHit acc[2];
} McpIoSeen;

static struct {
    bool ready;
    QemuMutex lock;             /* for what follows */
    int n_ranges;
    McpDmaRange ranges[DEV_RANGES_MAX];
    int n_pieces;
    McpIoPiece pieces[DEV_PIECES_MAX];
    int n_cw;                   /* the run's CPU watchpoints */
    McpCpuWatch cw[RUN_CW_MAX];
    int n_seen;                 /* by CPU index */
    McpIoSeen *seen;
    int n_pending;              /* the accesses no run took yet */
    McpDevHit pending[DEV_PENDING_MAX];
    uint64_t n_lost;            /* more of them, not noted */
} mcp_dev;

static void mcp_dev_init(void)
{
    if (!mcp_dev.ready) {
        qemu_mutex_init(&mcp_dev.lock);
        mcp_dev.ready = true;
    }
}

static McpDevFrom mcp_dev_from(void)
{
    AioContext *ctx;

    if (qemu_in_vcpu_thread()) {
        return DEV_FROM_CPU;
    }
    ctx = qemu_get_current_aio_context();
    if (ctx == qemu_get_aio_context()) {
        return DEV_FROM_MAIN;
    }
    return ctx ? DEV_FROM_IOTHREAD : DEV_FROM_OTHER;
}

/*
 * Note an access that no CPU watchpoint caught, and ask the VM to stop
 * for it.  The lock is held.
 */
static void mcp_dev_note(const McpDevHit *h)
{
    if (mcp_dev.n_pending == DEV_PENDING_MAX) {
        mcp_dev.n_lost++;
    } else {
        mcp_dev.pending[mcp_dev.n_pending++] = *h;
        /*
         * In a CPU's I/O access, that CPU stops at the end of its block
         * of code
         */
        if (h->from == DEV_FROM_CPU) {
            cpu_set_interrupt(current_cpu, CPU_INTERRUPT_DEBUG);
            qemu_cpu_kick(current_cpu);
        }
    }
    /*
     * The main loop stops the VM in any case: a CPU that halts next
     * would never get to the end of its block.  This is done with the
     * lock held, so no access asks for a stop once mcp_dev_unhook() has
     * returned, and each stop asked for has its access noted.
     */
    if (runstate_is_running()) {
        qemu_system_debug_request();
    }
}

static void mcp_dma_hook(uint64_t ram, uint64_t len)
{
    McpDmaRange *r = NULL;
    uint64_t first = UINT64_MAX;
    int i;

    if (mcp_dev_self || !len) {
        return;
    }
    qemu_mutex_lock(&mcp_dev.lock);
    /* the first watched byte it wrote */
    for (i = 0; i < mcp_dev.n_ranges; i++) {
        McpDmaRange *o = &mcp_dev.ranges[i];

        if (ram < o->ram + o->len && o->ram < ram + len &&
            MAX(ram, o->ram) < first) {
            first = MAX(ram, o->ram);
            r = o;
        }
    }
    if (r) {
        McpDevHit h = {
            .area = r->area,
            .addr = r->addr + (first - r->ram),
            .has_phys = true,
            .phys = r->phys + (first - r->ram),
            .len = len,
            .write = true,
            .from = mcp_dev_from(),
            .cpu = -1,
        };

        if (h.from == DEV_FROM_CPU) {
            h.cpu = current_cpu->cpu_index;
        }
        mcp_dev_note(&h);
    }
    qemu_mutex_unlock(&mcp_dev.lock);
}

/*
 * Whether @wp, the watchpoint a CPU hit, is one of the run's on area
 * @area.  The lock is held.
 */
static bool mcp_dev_cw_is(CPUWatchpoint *wp, int area)
{
    int i;

    for (i = 0; i < mcp_dev.n_cw; i++) {
        McpCpuWatch *c = &mcp_dev.cw[i];

        if (c->area == area && wp->vaddr == c->addr && wp->len == c->len &&
            (wp->flags & ~BP_WATCHPOINT_HIT) == c->flags) {
            return true;
        }
    }
    return false;
}

static void mcp_io_hook(MemoryRegion *mr, hwaddr addr, uint64_t data,
                        unsigned size, bool write, bool ok)
{
    int flag = write ? BP_MEM_WRITE : BP_MEM_READ;
    McpIoPiece *p = NULL;
    CPUState *cpu = NULL;
    McpDevHit h;
    int i;

    if (mcp_dev_self) {
        return;
    }
    qemu_mutex_lock(&mcp_dev.lock);
    for (i = 0; i < mcp_dev.n_pieces; i++) {
        McpIoPiece *o = &mcp_dev.pieces[i];

        if (o->mr == mr && (o->flags & flag) && addr < o->off + o->len &&
            o->off < addr + size) {
            p = o;
            break;
        }
    }
    if (!p) {
        qemu_mutex_unlock(&mcp_dev.lock);
        return;
    }
    h = (McpDevHit) {
        .area = p->area,
        .addr = p->addr + (MAX(addr, p->off) - p->off),
        .has_phys = p->has_phys,
        .phys = p->phys + (MAX(addr, p->off) - p->off),
        .len = size,
        .reg = true,
        .write = write,
        .value = data,
        .offset = addr,
        .nothing = p->nothing,
        .ok = ok,
        .from = mcp_dev_from(),
        .cpu = -1,
    };
    memcpy(h.region, p->region, sizeof(h.region));
    if (h.from == DEV_FROM_CPU) {
        cpu = current_cpu;
        h.cpu = cpu->cpu_index;
    }
    if (cpu && cpu->watchpoint_hit &&
        mcp_dev_cw_is(cpu->watchpoint_hit, p->area)) {
        /* its watchpoint stops the VM: keep what it did for the hit */
        if (cpu->cpu_index < mcp_dev.n_seen) {
            McpIoSeen *s = &mcp_dev.seen[cpu->cpu_index];

            s->has[write] = true;
            s->acc[write] = h;
        }
    } else if (cpu || p->devices) {
        mcp_dev_note(&h);
    }
    qemu_mutex_unlock(&mcp_dev.lock);
}

/* Note the first watched byte that devices' accesses are not seen at */
static void mcp_dev_skip(McpBreak *b, int area, uint64_t addr,
                         const char *why)
{
    if (!b->dev_skip) {
        b->dev_skip = why;
        b->dev_skip_at = addr;
        b->dev_skip_area = area;
    }
}

/* The device that has @mr, else its owner, or NULL.  The BQL is held. */
static Object *mcp_region_device(MemoryRegion *mr)
{
    Object *o;

    for (o = mr->owner; o; o = o->parent) {
        if (object_dynamic_cast(o, TYPE_DEVICE)) {
            return o;
        }
    }
    /* a list of I/O ports owns its regions and puts them in its device */
    for (o = OBJECT(mr)->parent; o; o = o->parent) {
        if (object_dynamic_cast(o, TYPE_DEVICE)) {
            return o;
        }
    }
    return mr->owner;
}

/*
 * @mr as summaries name it, with the type of the device that has it:
 * "serial (isa-serial)".  The BQL is held.
 */
static void mcp_region_name(MemoryRegion *mr, char *buf, size_t size)
{
    const char *name = memory_region_name(mr);
    Object *o = mcp_region_device(mr);

    if (!name || !*name) {
        name = "an unnamed region";
    }
    if (o) {
        snprintf(buf, size, "%s (%s)", name, object_get_typename(o));
    } else {
        snprintf(buf, size, "%s", name);
    }
}

typedef struct McpWindowScan {
    McpBreak *b;
    McpIoPiece *p;
} McpWindowScan;

/* A CPU watchpoint where a CPU sees the registers of the piece */
static bool mcp_dev_window(Int128 start, Int128 len, const MemoryRegion *mr,
                           hwaddr off, void *opaque)
{
    McpWindowScan *w = opaque;
    McpIoPiece *p = w->p;
    McpBreak *b = w->b;
    uint64_t n = int128_gethi(len) ? UINT64_MAX - off : int128_getlo(len);
    uint64_t lo, hi;
    McpCpuWatch *c;

    if (mr != p->mr) {
        return false;
    }
    lo = MAX(off, p->off);
    hi = MIN(off + n, p->off + p->len);
    if (lo >= hi) {
        return false;
    }
    if (b->n_cw == RUN_CW_MAX) {
        return true;
    }
    c = &b->cw[b->n_cw++];
    c->addr = int128_getlo(start) + (lo - off);
    c->len = hi - lo;
    c->flags = BP_GDB | b->watches[p->area].flags | BP_PHYS;
    c->area = p->area;
    c->at = p->addr + (lo - p->off);
    return false;
}

/*
 * A piece of area @i at @addr, @len bytes at @xlat in @mr.  The lock and
 * the RCU read lock are held.
 */
static void mcp_dev_add_piece(McpBreak *b, CPUState *cpu, int i,
                              MemoryRegion *mr, hwaddr xlat, uint64_t len,
                              uint64_t addr, uint64_t phys)
{
    McpWatchArea *a = &b->watches[i];
    McpIoPiece *p;

    if (!memory_region_is_unassigned(mr)) {
        a->anything = true;
    }
    if (memory_region_is_ram(mr) && !memory_region_is_ram_device(mr)) {
        McpDmaRange *r;

        if (a->as) {
            mcp_dev_skip(b, i, addr, "it is RAM, which only a CPU's"
                         " watchpoint sees: give the physical address where"
                         " CPUs see it");
            return;
        }
        /* devices' reads of RAM are not seen */
        if (!a->devices || !(a->flags & BP_MEM_WRITE)) {
            return;
        }
        if (mcp_dev.n_ranges == DEV_RANGES_MAX) {
            mcp_dev_skip(b, i, addr, "the watches cover too many pieces of"
                         " RAM");
            return;
        }
        r = &mcp_dev.ranges[mcp_dev.n_ranges++];
        r->ram = memory_region_get_ram_addr(mr) + xlat;
        r->len = len;
        r->addr = addr;
        r->phys = phys;
        r->area = i;
        a->has_ram = true;
        return;
    }
    if (mcp_dev.n_pieces == DEV_PIECES_MAX) {
        mcp_dev_skip(b, i, addr, "the watches cover too many pieces of"
                     " devices' memory");
        return;
    }
    p = &mcp_dev.pieces[mcp_dev.n_pieces++];
    memory_region_ref(mr);
    p->mr = mr;
    p->off = xlat;
    p->len = len;
    p->addr = addr;
    p->has_phys = !a->as;
    p->phys = phys;
    p->area = i;
    p->flags = a->flags;
    p->devices = a->devices;
    p->nothing = memory_region_is_unassigned(mr);
    mcp_region_name(mr, p->region, sizeof(p->region));
    a->has_regs = true;
    /* where CPUs see it, they stop right after the instruction */
    if (a->as) {
        McpWindowScan w = { .b = b, .p = p };

        flatview_for_each_range(address_space_to_flatview(cpu->as),
                                mcp_dev_window, &w);
    }
}

typedef struct McpRangeEnd {
    uint64_t addr;
    uint64_t end;               /* of what is at @addr, or of the gap there */
} McpRangeEnd;

static bool mcp_range_end(Int128 start, Int128 len, const MemoryRegion *mr,
                          hwaddr off, void *opaque)
{
    McpRangeEnd *e = opaque;
    Int128 end = int128_add(start, len);
    uint64_t lo = int128_getlo(start);
    uint64_t hi = int128_gethi(end) ? UINT64_MAX : int128_getlo(end);

    /* the ranges come in order */
    if (e->addr < lo) {
        e->end = lo;
        return true;
    }
    if (e->addr < hi) {
        e->end = hi;
        return true;
    }
    return false;
}

/*
 * What area @i of @b is as the run begins, for @cpu: RAM into
 * mcp_dev.ranges, devices' registers into mcp_dev.pieces.  The lock is
 * held.
 */
static void mcp_dev_add(McpBreak *b, CPUState *cpu, int i)
{
    McpWatchArea *a = &b->watches[i];
    uint64_t addr = a->addr, left = a->len;

    a->has_ram = false;
    a->has_regs = false;
    a->anything = false;
    while (left) {
        MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
        AddressSpace *as = a->as ? a->as : cpu->as;
        uint64_t page = qemu_target_page_size();
        uint64_t phys = addr, chunk = left;
        TranslateForDebugResult t;
        MemoryRegion *mr;
        hwaddr xlat, l;

        if (!a->physical && !a->as) {
            if (!cpu_translate_for_debug(cpu, addr, &t)) {
                mcp_dev_skip(b, i, addr, "it was not mapped as the run"
                             " began");
                chunk = MIN(left, page - (addr & (page - 1)));
                addr += chunk;
                left -= chunk;
                continue;
            }
            page = t.lg_page_size < 64 ? 1ULL << t.lg_page_size : 0;
            as = cpu_get_address_space(cpu, cpu_asidx_from_attrs(cpu,
                                                                 t.attrs));
            attrs = t.attrs;
            phys = t.physaddr;
            chunk = page ? MIN(left, page - (addr & (page - 1))) : left;
        }
        WITH_RCU_READ_LOCK_GUARD() {
            McpRangeEnd e = { .addr = phys, .end = UINT64_MAX };

            /*
             * Translating cuts the length at the end of RAM, not at the
             * end of a device's region: the memory map does
             */
            flatview_for_each_range(address_space_to_flatview(as),
                                    mcp_range_end, &e);
            chunk = MIN(chunk, e.end - phys);
            l = chunk;
            mr = address_space_translate(as, phys, &xlat, &l, true, attrs);
            chunk = l ? MIN(l, chunk) : chunk;
            mcp_dev_add_piece(b, cpu, i, mr, xlat, chunk, addr, phys);
        }
        addr += chunk;
        left -= chunk;
    }
    /* nothing at all there: the device may be somewhere else */
    if (a->has_regs && !a->anything && !b->dev_none) {
        b->dev_none = true;
        b->dev_none_area = i;
    }
}

/*
 * Watch the memory of @b on the device side, through the mappings of
 * the CPU its addresses were read for; areas in another address space
 * get CPU watchpoints where CPUs see them, into @b.  The VM is stopped.
 */
static void mcp_dev_insert(McpBreak *b)
{
    CPUState *cpu;
    int i, n = 0, n_ranges, n_pieces;

    qemu_mutex_lock(&mcp_dev.lock);
    mcp_dev.n_ranges = 0;
    mcp_dev.n_pieces = 0;
    mcp_dev.n_pending = 0;
    mcp_dev.n_lost = 0;
    b->dev_skip = NULL;
    b->dev_none = false;
    for (i = 0; i < b->n_watches; i++) {
        mcp_dev_add(b, b->wcpu, i);
    }
    /* for the I/O hook to tell the accesses CPUs' watchpoints caught */
    mcp_dev.n_cw = b->n_cw;
    memcpy(mcp_dev.cw, b->cw, b->n_cw * sizeof(b->cw[0]));
    CPU_FOREACH(cpu) {
        n = MAX(n, cpu->cpu_index + 1);
    }
    if (n > mcp_dev.n_seen) {
        mcp_dev.seen = g_renew(McpIoSeen, mcp_dev.seen, n);
        mcp_dev.n_seen = n;
    }
    memset(mcp_dev.seen, 0, mcp_dev.n_seen * sizeof(McpIoSeen));
    n_ranges = mcp_dev.n_ranges;
    n_pieces = mcp_dev.n_pieces;
    qemu_mutex_unlock(&mcp_dev.lock);
    if (n_ranges) {
        qatomic_set(&ram_write_hook, mcp_dma_hook);
    }
    if (n_pieces) {
        qatomic_set(&io_access_hook, mcp_io_hook);
    }
}

/* No more accesses are noted; those noted stay for the run to take */
static void mcp_dev_unhook(void)
{
    MemoryRegion *mrs[DEV_PIECES_MAX];
    int n, i;

    qatomic_set(&ram_write_hook, NULL);
    qatomic_set(&io_access_hook, NULL);
    qemu_mutex_lock(&mcp_dev.lock);
    n = mcp_dev.n_pieces;
    for (i = 0; i < n; i++) {
        mrs[i] = mcp_dev.pieces[i].mr;
    }
    mcp_dev.n_ranges = 0;
    mcp_dev.n_pieces = 0;
    mcp_dev.n_cw = 0;
    qemu_mutex_unlock(&mcp_dev.lock);
    for (i = 0; i < n; i++) {
        memory_region_unref(mrs[i]);
    }
}

/* Whether accesses were noted that no run took yet */
static bool mcp_dev_pending(void)
{
    bool any;

    qemu_mutex_lock(&mcp_dev.lock);
    any = mcp_dev.n_pending || mcp_dev.n_lost;
    qemu_mutex_unlock(&mcp_dev.lock);
    return any;
}

/*
 * Take the accesses noted into @w, which has room for DEV_PENDING_MAX;
 * *@lost gets how many more came.  The CPUs they came from need not
 * stop for them any more: under the lock, so that an access noted later
 * keeps its stop.  The BQL is held.
 */
static int mcp_dev_take(McpDevHit *w, uint64_t *lost)
{
    int n, i;

    qemu_mutex_lock(&mcp_dev.lock);
    n = mcp_dev.n_pending;
    memcpy(w, mcp_dev.pending, n * sizeof(*w));
    *lost = mcp_dev.n_lost;
    mcp_dev.n_pending = 0;
    mcp_dev.n_lost = 0;
    for (i = 0; i < n; i++) {
        CPUState *cpu = w[i].from == DEV_FROM_CPU ? qemu_get_cpu(w[i].cpu)
                                                  : NULL;

        /* a watchpoint hit may still need it */
        if (cpu && !cpu->watchpoint_hit) {
            cpu_reset_interrupt(cpu, CPU_INTERRUPT_DEBUG);
        }
    }
    qemu_mutex_unlock(&mcp_dev.lock);
    return n;
}

/* Forget the accesses noted that no hit took: how many */
static uint64_t mcp_dev_drop(void)
{
    g_autofree McpDevHit *w = g_new(McpDevHit, DEV_PENDING_MAX);
    uint64_t lost;

    return mcp_dev_take(w, &lost) + lost;
}

/*
 * What the access to registers of area @area that the watchpoint of
 * @cpu caught did, a write or a read, into *@h; false when it saw none.
 * That uses it up.
 */
static bool mcp_dev_take_seen(CPUState *cpu, int area, bool write,
                              McpDevHit *h)
{
    bool found = false;

    qemu_mutex_lock(&mcp_dev.lock);
    if (cpu->cpu_index < mcp_dev.n_seen) {
        McpIoSeen *s = &mcp_dev.seen[cpu->cpu_index];

        if (s->has[write] && s->acc[write].area == area) {
            *h = s->acc[write];
            found = true;
        }
        memset(s, 0, sizeof(*s));
    }
    qemu_mutex_unlock(&mcp_dev.lock);
    return found;
}

/*
 * Breakpoints are checked when a CPU looks for the next block of
 * translated code, and chained blocks don't look: flush them all, so
 * that every way to the addresses looks again.  Watchpoints flush the
 * TLB for their pages themselves.  The VM is stopped.
 */
static void mcp_debug_insert(McpBreak *b)
{
    CPUState *cpu;
    int i;

    /* the watched areas CPUs see, then where they see the others */
    b->n_cw = 0;
    for (i = 0; i < b->n_watches; i++) {
        McpWatchArea *w = &b->watches[i];

        if (mcp_watch_by_cpu(w)) {
            b->cw[b->n_cw++] = (McpCpuWatch) {
                .addr = w->addr,
                .len = w->len,
                .flags = BP_GDB | mcp_watch_flags(w),
                .area = i,
                .at = w->addr,
            };
        }
    }
    mcp_dev_insert(b);
    CPU_FOREACH(cpu) {
        if (b->only && cpu != b->only) {
            continue;
        }
        for (i = 0; i < b->n_addrs; i++) {
            CPUBreakpoint *bp;

            cpu_breakpoint_insert(cpu, b->addrs[i], BP_GDB, &bp);
            bp->mask = b->mask;
        }
        for (i = 0; i < b->n_cw; i++) {
            McpCpuWatch *c = &b->cw[i];

            cpu_watchpoint_insert(cpu, c->addr, c->len, c->flags, NULL);
        }
        if (b->has_excp) {
            g_free(cpu->excp_watch);
            cpu->excp_watch = g_memdup2(&b->excp, sizeof(b->excp));
        }
        cpu->debug_cond = b->cond ? &b->cond->base : NULL;
    }
    if (b->n_addrs) {
        tb_flush__exclusive_or_serial();
    }
}

/*
 * Whether @cpu stopped for an exception, and which in *@seen.  That
 * uses it up.
 */
static bool mcp_excp_take(McpBreak *b, CPUState *cpu, McpExcpSeen *seen)
{
    CPUExcpWatch *w = cpu->excp_watch;

    if (!b->has_excp || !w || !w->hit) {
        return false;
    }
    seen->cpu = cpu;
    seen->number = w->number;
    seen->from = w->from;
    seen->to = w->to;
    w->hit = false;
    return true;
}

/*
 * Our watchpoint @c on @cpu, found again by what it watches: a GDB
 * client that detaches removes all BP_GDB watchpoints, ours too
 */
static CPUWatchpoint *mcp_watch_find(McpCpuWatch *c, CPUState *cpu)
{
    CPUWatchpoint *wp;

    QTAILQ_FOREACH(wp, &cpu->watchpoints, entry) {
        if (wp->vaddr == c->addr && wp->len == c->len &&
            (wp->flags & ~BP_WATCHPOINT_HIT) == c->flags) {
            return wp;
        }
    }
    return NULL;
}

/* Which CPU watchpoint of @b @wp on @cpu is, or -1 */
static int mcp_watch_ours(McpBreak *b, CPUState *cpu, CPUWatchpoint *wp)
{
    int i;

    for (i = 0; i < b->n_cw; i++) {
        if (mcp_watch_find(&b->cw[i], cpu) == wp) {
            return i;
        }
    }
    return -1;
}

/* A CPU's hit is dealt with: it must not stop the VM again */
static void mcp_watch_clear_hit(CPUState *cpu, CPUWatchpoint *wp)
{
    wp->flags &= ~BP_WATCHPOINT_HIT;
    cpu->watchpoint_hit = NULL;
    /* raised as the access ended; a single step may have stopped first */
    cpu_reset_interrupt(cpu, CPU_INTERRUPT_DEBUG);
}

/*
 * Whether @cpu touched the watched memory, and what it did in *@seen.
 * Its hit is cleared, as gdbstub does.
 */
static bool mcp_watch_take(McpBreak *b, CPUState *cpu, McpWatchSeen *seen)
{
    CPUWatchpoint *wp = cpu->watchpoint_hit;
    int i = wp ? mcp_watch_ours(b, cpu, wp) : -1;
    McpCpuWatch *c;

    if (i < 0) {
        return false;
    }
    c = &b->cw[i];
    /*
     * The CPU looks at the condition as it takes its debug interrupt.  If
     * the VM stopped first, it never will: look for it, and count it.
     */
    if (b->cond && cpu_test_interrupt(cpu, CPU_INTERRUPT_DEBUG) &&
        !mcp_cond_test(cpu, &b->cond->base)) {
        mcp_watch_clear_hit(cpu, wp);
        return false;
    }
    memset(seen, 0, sizeof(*seen));
    seen->cpu = cpu;
    seen->area = c->area;
    seen->addr = c->at + (wp->hitaddr - c->addr);
    seen->pc = wp->hitpc;
    seen->by_insn = true;
    seen->write = wp->flags & BP_WATCHPOINT_HIT_WRITE;
    /* what it did to a device's registers */
    seen->has_dev = mcp_dev_take_seen(cpu, c->area, seen->write,
                                      &seen->dev);
    mcp_watch_clear_hit(cpu, wp);
    return true;
}

/* How many CPUs touched it, with what the first of them did */
static int mcp_watch_hits(McpBreak *b, McpWatchSeen *seen)
{
    McpWatchSeen s;
    CPUState *cpu;
    int n = 0;

    CPU_FOREACH(cpu) {
        if (mcp_watch_take(b, cpu, &s) && !n++) {
            *seen = s;
        }
    }
    return n;
}

/* Our breakpoint at @addr on @cpu, found again by what it compares */
static CPUBreakpoint *mcp_bp_find(McpBreak *b, uint64_t addr, CPUState *cpu)
{
    vaddr pc = mcp_debug_bp_pc(cpu, addr);
    CPUBreakpoint *bp;

    QTAILQ_FOREACH(bp, &cpu->breakpoints, entry) {
        if (bp->pc == pc && bp->mask == b->mask && bp->flags == BP_GDB) {
            return bp;
        }
    }
    return NULL;
}

static void mcp_debug_remove(McpBreak *b)
{
    CPUState *cpu;
    int i;

    mcp_dev_unhook();
    CPU_FOREACH(cpu) {
        if (b->only && cpu != b->only) {
            continue;
        }
        for (i = 0; i < b->n_addrs; i++) {
            CPUBreakpoint *bp = mcp_bp_find(b, b->addrs[i], cpu);

            if (bp) {
                cpu_breakpoint_remove_by_ref(cpu, bp);
            }
        }
        for (i = 0; i < b->n_cw; i++) {
            CPUWatchpoint *wp = mcp_watch_find(&b->cw[i], cpu);

            if (wp) {
                /* a hit that has not stopped the VM yet never will */
                if (cpu->watchpoint_hit == wp) {
                    mcp_watch_clear_hit(cpu, wp);
                }
                cpu_watchpoint_remove_by_ref(cpu, wp);
            }
        }
        if (b->has_excp) {
            g_free(cpu->excp_watch);
            cpu->excp_watch = NULL;
        }
        cpu->debug_cond = NULL;
    }
}

/*
 * Take them away at the end, with the vCPUs paused if something else
 * started the VM: a CPU reads its exception watch as it runs
 */
static void mcp_debug_remove_paused(void *opaque)
{
    McpBreak *b = opaque;
    bool running = runstate_is_running();

    if (running) {
        pause_all_vcpus();
    }
    mcp_debug_remove(b);
    if (running) {
        resume_all_vcpus();
    }
}

/*
 * CPUs at an address, that touched the watched memory, or that took an
 * exception, and whether devices wrote to it.  Devices' writes are no
 * longer noted.
 */
static void mcp_debug_late_hits(McpBreak *b)
{
    McpExcpSeen s;
    CPUState *cpu;

    b->whits = mcp_watch_hits(b, &b->seen);
    b->hits = 0;
    b->ehits = 0;
    CPU_FOREACH(cpu) {
        b->hits += mcp_debug_is_hit(b, cpu);
        if (mcp_excp_take(b, cpu, &s) && !b->ehits++) {
            b->eseen = s;
        }
    }
    b->dhits = mcp_dev_pending();
}

/*
 * The time ran out with the VM running: take the breakpoints away with
 * the vCPUs paused.  A CPU may have got to the address just now; then
 * the VM stops there, as for any hit.  A CPU that hit the watchpoint
 * has done the access by now (its last instruction runs with
 * interrupts off) and has asked to stop the VM: it stops.  So does a
 * CPU that took an exception, and a device that wrote to the watched
 * memory.
 */
static void mcp_debug_remove_running(void *opaque)
{
    McpBreak *b = opaque;

    /* first, so that each access noted is counted */
    mcp_dev_unhook();
    if (!runstate_is_running()) {
        /* it may have stopped for a hit after the wait gave up */
        if (runstate_check(RUN_STATE_DEBUG)) {
            mcp_debug_late_hits(b);
        }
        mcp_debug_remove(b);
        return;
    }
    pause_all_vcpus();
    mcp_debug_late_hits(b);
    mcp_debug_remove(b);
    if (b->hits || b->whits || b->ehits || b->dhits) {
        qemu_system_debug_request();
    } else {
        resume_all_vcpus();
    }
}

/*
 * The GDB register number of @name on @cpu: a name from its GDB target
 * description, or rN for register N when the description has no names
 * for its core registers (alpha, hppa, mips).  -1 when it has none.
 */
static int mcp_reg_number(CPUState *cpu, const char *name)
{
    g_autoptr(GArray) regs = gdb_get_register_list(cpu);
    const char *end;
    unsigned int i, n;

    for (i = 0; i < regs->len; i++) {
        GDBRegDesc *r = &g_array_index(regs, GDBRegDesc, i);

        if (r->name && !g_ascii_strcasecmp(r->name, name)) {
            return r->gdb_reg;
        }
    }
    if (!cpu->cc->gdb_core_xml_file && (name[0] == 'r' || name[0] == 'R') &&
        qemu_isdigit(name[1]) && !qemu_strtoui(name + 1, &end, 10, &n) &&
        !*end && n < cpu->cc->gdb_num_core_regs) {
        return n;
    }
    return -1;
}

/* GDB's name of register @n, else rN */
static char *mcp_reg_name(GArray *names, int n)
{
    unsigned int i;

    for (i = 0; i < names->len; i++) {
        GDBRegDesc *r = &g_array_index(names, GDBRegDesc, i);

        if (r->gdb_reg == n && r->name) {
            return g_strdup(r->name);
        }
    }
    return g_strdup_printf("r%d", n);
}

/*
 * GDB's bytes of a register as a number, when it has 1 to 8 bytes: in
 * the byte order the CPU runs in now, as GDB wants it
 */
static bool mcp_reg_decode(CPUState *cpu, const GByteArray *b, uint64_t *val)
{
    bool be = cpu_internal_is_big_endian(cpu);
    unsigned int i;

    if (b->len != 1 && b->len != 2 && b->len != 4 && b->len != 8) {
        return false;
    }
    *val = 0;
    for (i = 0; i < b->len; i++) {
        *val = *val << 8 | b->data[be ? i : b->len - 1 - i];
    }
    return true;
}

/*
 * The value of register @name of @cpu, when it is an integer: pc is
 * where the debug tools say the CPU is, the others are GDB's
 */
static bool mcp_reg_value(CPUState *cpu, const char *name, uint64_t *val)
{
    g_autoptr(GByteArray) buf = NULL;
    int reg;

    if (!g_ascii_strcasecmp(name, "pc")) {
        *val = cpu->cc->get_pc(cpu);
        return true;
    }
    reg = mcp_reg_number(cpu, name);
    if (reg < 0) {
        return false;
    }
    buf = g_byte_array_new();
    gdb_read_register(cpu, buf, reg);
    return mcp_reg_decode(cpu, buf, val);
}

/* The registers an address may name, for an error */
static char *mcp_reg_names(CPUState *cpu)
{
    g_autoptr(GArray) regs = gdb_get_register_list(cpu);
    GString *s = g_string_new("pc");
    unsigned int i;

    for (i = 0; i < regs->len && i < 64; i++) {
        GDBRegDesc *r = &g_array_index(regs, GDBRegDesc, i);

        if (r->name && g_ascii_strcasecmp(r->name, "pc")) {
            g_string_append_printf(s, ", %s", r->name);
        }
    }
    if (i < regs->len) {
        g_string_append(s, " and more");
    }
    if (!cpu->cc->gdb_core_xml_file) {
        g_string_append_printf(s, "%s r0 to r%d (GDB's numbers: on most CPUs"
                               " rN is the general register N)",
                               regs->len ? " and" : ",",
                               cpu->cc->gdb_num_core_regs - 1);
    }
    return g_string_free(s, false);
}

#define REG_NAME_MAX    32
#define ADDR_TERMS_MAX  4
#define STRING_MAX      256

typedef struct McpAddrTerm {
    char reg[REG_NAME_MAX];     /* a register, or "" for @num */
    uint64_t num;
    bool minus;
} McpAddrTerm;

typedef struct McpMem {
    const char *text;           /* as asked */
    McpAddrTerm term[ADDR_TERMS_MAX];
    int n_terms;
    bool has_reg;
    int words;
    bool physical;
    bool string;
    bool watched;               /* a watched area: a device's is not read */
    bool skip;                  /* not shown */
} McpMem;

/*
 * A memory address: numbers, registers of the CPU that stops and
 * symbols, added and taken away, such as 0x1000, sp, sp+16, jiffies+4
 * or 0x657500000000+r26 (a space or segment base and an offset).  @cpu
 * is one like that CPU.
 */
static bool mcp_parse_mem_address(CPUState *cpu, const char *s, McpMem *m,
                                  Error **errp)
{
    const char *p = s;
    bool minus = false;

    m->text = s;
    for (;;) {
        McpAddrTerm *t = &m->term[m->n_terms];
        size_t len;
        g_autofree char *word = NULL;
        const McpSym *sym;
        const char *name;

        if (m->n_terms++ == ADDR_TERMS_MAX) {
            error_setg(errp, "memory address '%s' has more than %d parts",
                       s, ADDR_TERMS_MAX);
            return false;
        }
        while (qemu_isspace(*p)) {
            p++;
        }
        len = strcspn(p, "+- ");
        word = g_strndup(p, len);
        p += len;
        t->minus = minus;
        name = word[0] == '$' ? word + 1 : word;
        if (qemu_isdigit(word[0])) {
            if (!mcp_parse_address(word, "memory address", &t->num, errp)) {
                return false;
            }
        } else if (!cpu) {
            error_setg(errp, "memory address '%s': this VM has no CPU", s);
            return false;
        } else if (*name && strlen(name) < REG_NAME_MAX &&
                   (mcp_reg_number(cpu, name) >= 0 ||
                    !g_ascii_strcasecmp(name, "pc"))) {
            pstrcpy(t->reg, sizeof(t->reg), name);
            m->has_reg = true;
        } else {
            sym = *word ? mcp_sym_find(word, false) : NULL;
            if (!sym) {
                g_autofree char *names = mcp_reg_names(cpu);

                error_setg(errp, "memory address '%s': '%s' is not a number,"
                           " a register or a symbol (debug-symbols loads"
                           " them); the registers are %s", s, word, names);
                return false;
            }
            t->num = mcp_sym_place(sym);
        }
        while (qemu_isspace(*p)) {
            p++;
        }
        if (!*p) {
            return true;
        }
        minus = *p++ == '-';
    }
}

/*
 * The address @m names, with the registers of @cpu now.  *@bad is a
 * register that has no integer value.
 */
static bool mcp_mem_address(CPUState *cpu, McpMem *m, uint64_t *addr,
                            const char **bad)
{
    int i;

    *addr = 0;
    for (i = 0; i < m->n_terms; i++) {
        McpAddrTerm *t = &m->term[i];
        uint64_t v = t->num;

        if (t->reg[0] && !mcp_reg_value(cpu, t->reg, &v)) {
            *bad = t->reg;
            return false;
        }
        *addr += t->minus ? -v : v;
    }
    return true;
}

static McpMem *mcp_parse_memory(CPUState *cpu, McpDebugMemoryList *list,
                                int *count, Error **errp)
{
    g_autofree McpMem *mem = NULL;
    McpDebugMemoryList *l;
    int n = 0;

    for (l = list; l; l = l->next) {
        n++;
    }
    if (n > MEMORY_RANGES_MAX) {
        error_setg(errp, "ask for at most %d memory ranges",
                   MEMORY_RANGES_MAX);
        return NULL;
    }
    /* and the watched memory */
    mem = g_new0(McpMem, n + RUN_WATCHES_MAX);
    for (l = list, n = 0; l; l = l->next, n++) {
        McpDebugMemory *d = l->value;

        if (!mcp_parse_mem_address(cpu, d->address, &mem[n], errp)) {
            return NULL;
        }
        mem[n].words = d->has_words ? d->words : MEMORY_WORDS;
        if (mem[n].words < 1 || mem[n].words > MEMORY_WORDS_MAX) {
            error_setg(errp, "words must be 1 to %d", MEMORY_WORDS_MAX);
            return NULL;
        }
        mem[n].physical = d->has_physical && d->physical;
        mem[n].string = d->has_string && d->string;
    }
    *count = n;
    return g_steal_pointer(&mem);
}

/*
 * Node @i of @c for @cpu into *@v, or why it can't be worked out, with
 * the register or the address in *@where
 */
static McpCondFail mcp_cond_eval(CPUState *cpu, const McpCond *c, int i,
                                 uint64_t *v, uint64_t *where)
{
    const McpCondNode *n = &c->node[i];
    McpCondFail f;
    uint64_t a, b;
    bool sgn;

    switch (n->op) {
    case COND_NUM:
        *v = n->num;
        return COND_OK;
    case COND_PC:
        *v = cpu->cc->get_pc(cpu);
        return COND_OK;
    case COND_REG: {
        g_autoptr(GByteArray) buf = g_byte_array_new();

        gdb_read_register(cpu, buf, n->num);
        if (!mcp_reg_decode(cpu, buf, v)) {
            *where = n->num;
            return COND_NO_REGISTER;
        }
        return COND_OK;
    }
    case COND_MEM: {
        bool be = cpu_internal_is_big_endian(cpu);
        int size = abs(n->size), k;
        uint8_t m[8];

        f = mcp_cond_eval(cpu, c, n->a, &a, where);
        if (f) {
            return f;
        }
        mcp_dev_self++;
        k = cpu_memory_rw_debug(cpu, a, m, size, false);
        mcp_dev_self--;
        if (k) {
            *where = a;
            return COND_NO_MEMORY;
        }
        *v = 0;
        for (k = 0; k < size; k++) {
            *v = *v << 8 | m[be ? k : size - 1 - k];
        }
        if (n->size < 0) {
            *v = sextract64(*v, 0, size * 8);
        }
        return COND_OK;
    }
    case COND_CONV:
        f = mcp_cond_eval(cpu, c, n->a, &a, where);
        if (f) {
            return f;
        }
        *v = n->size < 0 ? sextract64(a, 0, -n->size * 8) :
                           extract64(a, 0, n->size * 8);
        return COND_OK;
    case COND_NEG:
    case COND_NOT:
    case COND_INV:
        f = mcp_cond_eval(cpu, c, n->a, &a, where);
        if (f) {
            return f;
        }
        *v = n->op == COND_NEG ? -a : n->op == COND_NOT ? !a : ~a;
        return COND_OK;
    case COND_LAND:
    case COND_LOR:
        f = mcp_cond_eval(cpu, c, n->a, &a, where);
        if (f) {
            return f;
        }
        /* && stops at a false one, || at a true one */
        if ((n->op == COND_LAND) == !a) {
            *v = !!a;
            return COND_OK;
        }
        f = mcp_cond_eval(cpu, c, n->b, &b, where);
        if (f) {
            return f;
        }
        *v = !!b;
        return COND_OK;
    default:
        break;
    }
    f = mcp_cond_eval(cpu, c, n->a, &a, where);
    if (!f) {
        f = mcp_cond_eval(cpu, c, n->b, &b, where);
    }
    if (f) {
        return f;
    }
    /* signed when a side is; a shift goes by the value it shifts */
    sgn = n->op == COND_SHL || n->op == COND_SHR ? c->node[n->a].sgn :
          c->node[n->a].sgn || c->node[n->b].sgn;
    switch (n->op) {
    case COND_MUL:
        *v = a * b;
        break;
    case COND_DIV:
    case COND_MOD:
        if (!b) {
            return COND_ZERO_DIVIDE;
        }
        if (!sgn) {
            *v = n->op == COND_DIV ? a / b : a % b;
        } else if (a == (uint64_t)INT64_MIN && b == UINT64_MAX) {
            /* the one that does not fit, and traps on some hosts */
            *v = n->op == COND_DIV ? a : 0;
        } else {
            *v = n->op == COND_DIV ? (int64_t)a / (int64_t)b :
                                     (int64_t)a % (int64_t)b;
        }
        break;
    case COND_ADD:
        *v = a + b;
        break;
    case COND_SUB:
        *v = a - b;
        break;
    case COND_SHL:
        *v = b < 64 ? a << b : 0;
        break;
    case COND_SHR:
        if (sgn) {
            *v = (int64_t)a >> MIN(b, 63);
        } else {
            *v = b < 64 ? a >> b : 0;
        }
        break;
    case COND_LT:
        *v = sgn ? (int64_t)a < (int64_t)b : a < b;
        break;
    case COND_LE:
        *v = sgn ? (int64_t)a <= (int64_t)b : a <= b;
        break;
    case COND_GT:
        *v = sgn ? (int64_t)a > (int64_t)b : a > b;
        break;
    case COND_GE:
        *v = sgn ? (int64_t)a >= (int64_t)b : a >= b;
        break;
    case COND_EQ:
        *v = a == b;
        break;
    case COND_NE:
        *v = a != b;
        break;
    case COND_AND:
        *v = a & b;
        break;
    case COND_XOR:
        *v = a ^ b;
        break;
    case COND_OR:
        *v = a | b;
        break;
    default:
        g_assert_not_reached();
    }
    return COND_OK;
}

/*
 * Whether the hit of @cpu meets the condition, counted: from the CPU's
 * own thread at each hit (debug-cond.h), or from the main loop for a hit
 * the CPU never looks at.  The CPU is not running either way.
 */
static bool mcp_cond_test(CPUState *cpu, CPUDebugCond *base)
{
    McpCond *c = container_of(base, McpCond, base);
    uint64_t v = 0, where = 0;
    McpCondFail f = mcp_cond_eval(cpu, c, c->root, &v, &where);
    bool met = !f && v;

    if (f) {
        qatomic_inc(&c->n_failed);
        if (!qatomic_xchg(&c->failed, 1)) {
            c->fail = f;
            c->where = where;
        }
    } else if (!v) {
        qatomic_inc(&c->n_false);
    }
    if (cpu->cpu_index < c->n_met) {
        qatomic_set(&c->met[cpu->cpu_index],
                    met ? (uint64_t)cpu->cc->get_pc(cpu) + 1 : 0);
    }
    return met;
}

/*
 * Whether it holds for @cpu, stopped: as its own thread found right here,
 * or else as things are now.  Not counted.
 */
static bool mcp_cond_holds(McpCond *c, CPUState *cpu)
{
    uint64_t v = 0, where;

    if (cpu->cpu_index < c->n_met &&
        qatomic_read(&c->met[cpu->cpu_index]) ==
        (uint64_t)cpu->cc->get_pc(cpu) + 1) {
        return true;
    }
    return !mcp_cond_eval(cpu, c, c->root, &v, &where) && v;
}

/* The hit of @cpu is dealt with: when it gets here again, look again */
static void mcp_cond_forget(McpCond *c, CPUState *cpu)
{
    if (c && cpu->cpu_index < c->n_met) {
        qatomic_set(&c->met[cpu->cpu_index], 0);
    }
}

static void mcp_cond_free(McpCond *c)
{
    if (c) {
        g_free(c->met);
        g_free(c);
    }
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(McpCond, mcp_cond_free)

typedef struct McpCondParser {
    McpCond *c;
    const char *text;
    const char *p;
    int depth;
    Error **errp;
} McpCondParser;

/* A new node, or -1 */
static int mcp_cond_node(McpCondParser *ps, McpCondOp op, int a, int b,
                         uint64_t num)
{
    McpCondNode *n;

    if (ps->c->n_nodes == COND_NODES_MAX) {
        error_setg(ps->errp, "condition '%s' has more than %d parts",
                   ps->text, COND_NODES_MAX);
        return -1;
    }
    n = &ps->c->node[ps->c->n_nodes];
    n->op = op;
    n->a = a;
    n->b = b;
    n->num = num;
    /* signed when a side is, as for the comparisons */
    switch (op) {
    case COND_NEG:
    case COND_INV:
    case COND_SHL:
    case COND_SHR:
        n->sgn = ps->c->node[a].sgn;
        break;
    case COND_MUL:
    case COND_DIV:
    case COND_MOD:
    case COND_ADD:
    case COND_SUB:
    case COND_AND:
    case COND_XOR:
    case COND_OR:
        n->sgn = ps->c->node[a].sgn || ps->c->node[b].sgn;
        break;
    default:
        n->sgn = false;
        break;
    }
    return ps->c->n_nodes++;
}

/* The operator at the place the parser got to, the longest, or NULL */
static const char *mcp_cond_peek(McpCondParser *ps)
{
    static const char *const ops[] = {
        "||", "&&", "==", "!=", "<=", ">=", "<<", ">>",
        "|", "^", "&", "<", ">", "+", "-", "*", "/", "%", "!", "~",
        "(", ")", "[", "]",
    };
    size_t i;

    while (qemu_isspace(*ps->p)) {
        ps->p++;
    }
    for (i = 0; i < ARRAY_SIZE(ops); i++) {
        if (g_str_has_prefix(ps->p, ops[i])) {
            return ops[i];
        }
    }
    return NULL;
}

/* Whether operator @op comes next; it is then taken */
static bool mcp_cond_take(McpCondParser *ps, const char *op)
{
    const char *t = mcp_cond_peek(ps);

    if (!t || strcmp(t, op)) {
        return false;
    }
    ps->p += strlen(op);
    return true;
}

static int mcp_cond_binary(McpCondParser *ps, int min_prec);

/*
 * The bytes that @word reads, as in u32[...], or keeps, as in u32(...);
 * negative when signed.  0 when it is no size.
 */
static int mcp_cond_size(const char *word)
{
    static const struct {
        const char *name;
        int size;
    } sizes[] = {
        { "u8", 1 }, { "u16", 2 }, { "u32", 4 }, { "u64", 8 },
        { "s8", -1 }, { "s16", -2 }, { "s32", -4 }, { "s64", -8 },
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(sizes); i++) {
        if (!g_ascii_strcasecmp(word, sizes[i].name)) {
            return sizes[i].size;
        }
    }
    return 0;
}

/* A register or a symbol */
static int mcp_cond_name(McpCondParser *ps, const char *word)
{
    const char *name = word[0] == '$' ? word + 1 : word;
    const McpSym *sym;
    int reg;

    if (!g_ascii_strcasecmp(name, "pc")) {
        return mcp_cond_node(ps, COND_PC, -1, -1, 0);
    }
    reg = *name ? mcp_reg_number(ps->c->cpu, name) : -1;
    if (reg >= 0) {
        return mcp_cond_node(ps, COND_REG, -1, -1, reg);
    }
    sym = mcp_sym_find(word, false);
    if (sym) {
        return mcp_cond_node(ps, COND_NUM, -1, -1, mcp_sym_place(sym));
    }
    {
        g_autofree char *names = mcp_reg_names(ps->c->cpu);

        error_setg(ps->errp, "condition '%s': '%s' is not a register or a"
                   " symbol (debug-symbols loads them); the registers are %s",
                   ps->text, word, names);
    }
    return -1;
}

/* A number, a name, u32[...], u32(...) or (...) */
static int mcp_cond_primary(McpCondParser *ps)
{
    static const char name_chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789_.$@?";
    g_autofree char *word = NULL;
    const char *start, *close;
    McpCondOp op;
    uint64_t num;
    size_t len;
    int size, a, r;

    if (mcp_cond_take(ps, "(")) {
        r = mcp_cond_binary(ps, 1);
        if (r >= 0 && !mcp_cond_take(ps, ")")) {
            error_setg(ps->errp, "condition '%s': a ')' is missing",
                       ps->text);
            return -1;
        }
        return r;
    }
    mcp_cond_peek(ps);
    start = ps->p;
    if (qemu_isdigit(*start)) {
        len = strspn(start, "0123456789abcdefABCDEFxX");
        word = g_strndup(start, len);
        ps->p += len;
        if (!mcp_parse_address(word, "number", &num, NULL)) {
            error_setg(ps->errp, "condition '%s': '%s' is not a number:"
                       " write hex with 0x, or decimal", ps->text, word);
            return -1;
        }
        return mcp_cond_node(ps, COND_NUM, -1, -1, num);
    }
    len = strspn(start, name_chars);
    if (!len) {
        if (*start) {
            error_setg(ps->errp, "condition '%s': a number, a register or a"
                       " symbol must come before '%s'", ps->text, start);
        } else {
            error_setg(ps->errp, "condition '%s': a number, a register or a"
                       " symbol is missing at the end", ps->text);
        }
        return -1;
    }
    word = g_strndup(start, len);
    ps->p += len;
    size = mcp_cond_size(word);
    if (mcp_cond_take(ps, "[")) {
        op = COND_MEM;
        close = "]";
    } else if (size && mcp_cond_take(ps, "(")) {
        op = COND_CONV;
        close = ")";
    } else {
        return mcp_cond_name(ps, word);
    }
    if (!size) {
        error_setg(ps->errp, "condition '%s': '%s[' is no memory read: write"
                   " u8[...], u16[...], u32[...] or u64[...], or s8 to s64"
                   " for signed numbers", ps->text, word);
        return -1;
    }
    a = mcp_cond_binary(ps, 1);
    if (a < 0) {
        return -1;
    }
    if (!mcp_cond_take(ps, close)) {
        error_setg(ps->errp, "condition '%s': a '%s' is missing", ps->text,
                   close);
        return -1;
    }
    r = mcp_cond_node(ps, op, a, -1, 0);
    if (r >= 0) {
        ps->c->node[r].size = size;
        ps->c->node[r].sgn = size < 0;
    }
    return r;
}

/* -x, !x, ~x, or a primary */
static int mcp_cond_unary(McpCondParser *ps)
{
    static const struct {
        const char *op;
        McpCondOp code;
    } ops[] = {
        { "-", COND_NEG }, { "!", COND_NOT }, { "~", COND_INV },
    };
    size_t i;
    int r = -2;

    if (++ps->depth > COND_DEPTH_MAX) {
        error_setg(ps->errp, "condition '%s' nests deeper than %d",
                   ps->text, COND_DEPTH_MAX);
        return -1;
    }
    for (i = 0; i < ARRAY_SIZE(ops) && r == -2; i++) {
        if (mcp_cond_take(ps, ops[i].op)) {
            r = mcp_cond_unary(ps);
            if (r >= 0) {
                r = mcp_cond_node(ps, ops[i].code, r, -1, 0);
            }
        }
    }
    if (r == -2) {
        r = mcp_cond_primary(ps);
    }
    ps->depth--;
    return r;
}

/* Operators that bind at least as tight as @min_prec, as in C */
static int mcp_cond_binary(McpCondParser *ps, int min_prec)
{
    static const struct {
        const char *op;
        McpCondOp code;
        int prec;
    } ops[] = {
        { "||", COND_LOR, 1 }, { "&&", COND_LAND, 2 }, { "|", COND_OR, 3 },
        { "^", COND_XOR, 4 }, { "&", COND_AND, 5 }, { "==", COND_EQ, 6 },
        { "!=", COND_NE, 6 }, { "<", COND_LT, 7 }, { "<=", COND_LE, 7 },
        { ">", COND_GT, 7 }, { ">=", COND_GE, 7 }, { "<<", COND_SHL, 8 },
        { ">>", COND_SHR, 8 }, { "+", COND_ADD, 9 }, { "-", COND_SUB, 9 },
        { "*", COND_MUL, 10 }, { "/", COND_DIV, 10 }, { "%", COND_MOD, 10 },
    };
    int left = mcp_cond_unary(ps);

    while (left >= 0) {
        const char *t = mcp_cond_peek(ps);
        size_t i;
        int right;

        for (i = 0; t && i < ARRAY_SIZE(ops) && strcmp(ops[i].op, t); i++) {
            /* find it */
        }
        if (!t || i == ARRAY_SIZE(ops) || ops[i].prec < min_prec) {
            break;
        }
        ps->p += strlen(t);
        right = mcp_cond_binary(ps, ops[i].prec + 1);
        if (right < 0) {
            return -1;
        }
        left = mcp_cond_node(ps, ops[i].code, left, right, 0);
    }
    return left;
}

/* @text as a condition for CPUs like @cpu, or NULL */
static McpCond *mcp_cond_parse(CPUState *cpu, const char *text, Error **errp)
{
    g_autoptr(McpCond) c = g_new0(McpCond, 1);
    McpCondParser ps = {
        .c = c, .text = text, .p = text, .errp = errp,
    };
    CPUState *each;

    if (strlen(text) > COND_TEXT_MAX) {
        error_setg(errp, "a condition has at most %d characters",
                   COND_TEXT_MAX);
        return NULL;
    }
    if (!cpu) {
        error_setg(errp, "this VM has no CPU");
        return NULL;
    }
    CPU_FOREACH(each) {
        c->n_met = MAX(c->n_met, each->cpu_index + 1);
    }
    c->met = g_new0(uint64_t, c->n_met);
    c->cpu = cpu;
    c->root = mcp_cond_binary(&ps, 1);
    if (c->root < 0) {
        return NULL;
    }
    mcp_cond_peek(&ps);
    if (*ps.p) {
        error_setg(errp, "condition '%s': '%s' does not fit there%s", text,
                   ps.p, *ps.p == '=' ? "; compare with ==" : "");
        return NULL;
    }
    c->base.test = mcp_cond_test;
    return g_steal_pointer(&c);
}

/*
 * How often the condition let a hit go by, for a summary: "the condition
 * was false 4 times", or NULL when it never did
 */
static char *mcp_cond_str(McpCond *c)
{
    int64_t f = c ? qatomic_read(&c->n_false) : 0;
    int64_t e = c ? qatomic_read(&c->n_failed) : 0;
    GString *s;

    if (!f && !e) {
        return NULL;
    }
    s = g_string_new("the condition ");
    if (f) {
        g_string_append_printf(s, "was false %" PRId64 " time%s", f,
                               f == 1 ? "" : "s");
    }
    if (e) {
        g_string_append_printf(s, "%scould not be worked out %" PRId64
                               " time%s, first because ", f ? "; it " : "",
                               e, e == 1 ? "" : "s");
        switch (c->fail) {
        case COND_NO_MEMORY:
            g_string_append_printf(s, "memory at 0x%" PRIx64 " could not be"
                                   " read", c->where);
            break;
        case COND_NO_REGISTER: {
            g_autoptr(GArray) names = gdb_get_register_list(c->cpu);
            g_autofree char *name = mcp_reg_name(names, c->where);

            g_string_append_printf(s, "register %s is not a number of up to"
                                   " 64 bits", name);
            break;
        }
        case COND_ZERO_DIVIDE:
            g_string_append(s, "it divided by zero");
            break;
        default:
            g_assert_not_reached();
        }
    }
    return g_string_free(s, false);
}

/*
 * " (@count; the condition was false 4 times)", leaving out what there
 * is not
 */
static void mcp_cond_append(GString *s, const char *count, McpCond *c)
{
    g_autofree char *cond = mcp_cond_str(c);

    if (count || cond) {
        g_string_append_printf(s, " (%s%s%s)", count ? count : "",
                               count && cond ? "; " : "", cond ? cond : "");
    }
}

/* The hits that did not meet the condition */
static int64_t mcp_cond_skipped(McpCond *c)
{
    return c ? qatomic_read(&c->n_false) + qatomic_read(&c->n_failed) : 0;
}

#define WATCH_LEN               4
#define WATCH_LEN_MAX           4096
#define WATCH_WORDS_SHOWN       16
#define WATCH_WORDS_EACH        4       /* shown of each of several */

/*
 * One area of memory to watch into @b.  Registers are read now, from
 * the CPU the tool shows.
 */
static bool mcp_parse_watch(McpWatch *w, McpBreak *b, Error **errp)
{
    static const int flags[] = {
        [MCP_WATCH_ACCESS_WRITE] = BP_MEM_WRITE,
        [MCP_WATCH_ACCESS_READ] = BP_MEM_READ,
        [MCP_WATCH_ACCESS_ACCESS] = BP_MEM_ACCESS,
    };
    CPUState *cpu = b->only ? b->only : mcp_debug_cpu(false, 0, errp);
    McpWatchArea a;
    McpMem m = { 0 };
    const char *bad = NULL;
    int i;

    if (!cpu) {
        return false;
    }
    if (!mcp_parse_mem_address(cpu, w->address, &m, errp)) {
        return false;
    }
    if (m.has_reg && runstate_is_running()) {
        error_setg(errp, "watch address '%s' reads registers, so it needs"
                   " the VM stopped (debug-step or vm-power pause first)",
                   w->address);
        return false;
    }
    if (!mcp_mem_address(cpu, &m, &a.addr, &bad)) {
        error_setg(errp, "watch address '%s': CPU %d has no integer"
                   " register %s", w->address, cpu->cpu_index, bad);
        return false;
    }
    a.len = w->has_length ? w->length : WATCH_LEN;
    if (a.len < 1 || a.len > WATCH_LEN_MAX) {
        error_setg(errp, "watch length must be 1 to %d", WATCH_LEN_MAX);
        return false;
    }
    if (a.addr + a.len - 1 < a.addr) {
        error_setg(errp, "watched memory runs past the end of the address"
                   " space");
        return false;
    }
    a.flags = flags[w->has_access ? w->access : MCP_WATCH_ACCESS_WRITE];
    a.physical = w->has_physical && w->physical;
    a.as = NULL;
    a.space[0] = 0;
    if (w->space) {
        AddressSpace *as = address_space_find_by_name(w->space);

        if (!as) {
            g_autofree char *names = address_space_names();

            error_setg(errp, "there is no address space '%s'; they are: %s",
                       w->space, names);
            return false;
        }
        if (a.physical) {
            error_setg(errp, "give physical or space, not both");
            return false;
        }
        /* the CPU's own memory: physical */
        if (as->root == cpu->as->root) {
            a.physical = true;
        } else {
            a.as = as;
            g_strlcpy(a.space, as->name, sizeof(a.space));
        }
    }
    /* devices' accesses, unless asked not to */
    a.devices = !w->has_devices || w->devices;
    a.has_ram = false;
    a.has_regs = false;
    a.anything = false;

    /* the same area twice is one watchpoint */
    for (i = 0; i < b->n_watches; i++) {
        McpWatchArea *o = &b->watches[i];

        if (o->addr == a.addr && o->len == a.len && o->flags == a.flags &&
            o->physical == a.physical && o->as == a.as &&
            o->devices == a.devices) {
            return true;
        }
    }
    if (b->n_watches == RUN_WATCHES_MAX) {
        error_setg(errp, "watch at most %d areas", RUN_WATCHES_MAX);
        return false;
    }
    b->watches[b->n_watches++] = a;
    b->wcpu = cpu;
    return true;
}

/*
 * The memory to watch, one area or several, into @b; into @shown, if
 * given, the memory to show when the VM stops, one McpMem per area
 */
static bool mcp_parse_watches(McpWatches *w, McpBreak *b, McpMem *shown,
                              Error **errp)
{
    McpWatchList one = { 0 };
    McpWatchList *l;
    int i, most;

    if (w->type == QTYPE_QDICT) {
        one.value = &w->u.one;
        l = &one;
    } else {
        l = w->u.several;
        if (!l) {
            error_setg(errp, "give at least one area to watch");
            return false;
        }
    }
    for (; l; l = l->next) {
        if (!mcp_parse_watch(l->value, b, errp)) {
            return false;
        }
    }
    if (!shown) {
        return true;
    }
    most = b->n_watches > 1 ? WATCH_WORDS_EACH : WATCH_WORDS_SHOWN;
    for (i = 0; i < b->n_watches; i++) {
        McpWatchArea *a = &b->watches[i];

        shown[i].n_terms = 1;
        shown[i].term[0].num = a->addr & ~3ULL;
        shown[i].words = MIN(DIV_ROUND_UP(a->len + (a->addr & 3), 4), most);
        shown[i].physical = a->physical;
        shown[i].watched = true;
        /* not in the CPU's memory: the hit shows what was done there */
        shown[i].skip = a->as;
    }
    return true;
}

/*
 * Whether @len bytes at @addr are RAM or ROM, or not mapped: whether a
 * debug tool can read them without a device seeing it
 */
static bool mcp_mem_plain(CPUState *cpu, uint64_t addr, uint64_t len,
                          bool physical)
{
    while (len) {
        MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
        AddressSpace *as = cpu->as;
        uint64_t phys = addr, chunk = len;
        MemoryRegion *mr;
        hwaddr xlat, l;

        if (!physical) {
            TranslateForDebugResult t;
            uint64_t page;

            if (!cpu_translate_for_debug(cpu, addr, &t)) {
                return true;
            }
            page = t.lg_page_size < 64 ? 1ULL << t.lg_page_size : 0;
            as = cpu_get_address_space(cpu, cpu_asidx_from_attrs(cpu,
                                                                 t.attrs));
            attrs = t.attrs;
            phys = t.physaddr;
            chunk = page ? MIN(len, page - (addr & (page - 1))) : len;
        }
        l = chunk;
        WITH_RCU_READ_LOCK_GUARD() {
            mr = address_space_translate(as, phys, &xlat, &l, false, attrs);
            if (!memory_region_supports_direct_access(mr) ||
                memory_region_is_ram_device(mr)) {
                return false;
            }
        }
        chunk = l ? MIN(l, chunk) : chunk;
        addr += chunk;
        len -= chunk;
    }
    return true;
}

/*
 * The C string at @addr, quoted, up to STRING_MAX bytes; NULL when not
 * a byte of it can be read.  A device's registers are not read.
 */
static char *mcp_read_string(CPUState *cpu, uint64_t addr, bool physical)
{
    uint64_t page = qemu_target_page_size();
    uint8_t buf[STRING_MAX];
    GString *s;
    int len = 0, i;
    bool ended = false;

    /* a page at a time: the next one may not be mapped */
    while (len < STRING_MAX && !ended) {
        uint64_t a = addr + len;
        int chunk = MIN(STRING_MAX - len, page - (a & (page - 1)));
        bool ok;

        if (!mcp_mem_plain(cpu, a, chunk, physical)) {
            break;
        }
        if (physical) {
            ok = address_space_read(cpu->as, a, MEMTXATTRS_UNSPECIFIED,
                                    buf + len, chunk) == MEMTX_OK;
        } else {
            ok = cpu_memory_rw_debug(cpu, a, buf + len, chunk, false) == 0;
        }
        if (!ok) {
            break;
        }
        ended = memchr(buf + len, 0, chunk);
        len += chunk;
    }
    if (!len) {
        return NULL;
    }
    s = g_string_new("\"");
    for (i = 0; i < len && buf[i]; i++) {
        switch (buf[i]) {
        case '\n':
            g_string_append(s, "\\n");
            break;
        case '\r':
            g_string_append(s, "\\r");
            break;
        case '\t':
            g_string_append(s, "\\t");
            break;
        case '"':
        case '\\':
            g_string_append_printf(s, "\\%c", buf[i]);
            break;
        default:
            if (buf[i] >= 0x20 && buf[i] < 0x7f) {
                g_string_append_c(s, buf[i]);
            } else {
                g_string_append_printf(s, "\\x%02x", buf[i]);
            }
        }
    }
    g_string_append_c(s, '"');
    if (!ended && len < STRING_MAX) {
        g_string_append(s, " (no zero byte; the rest can't be read)");
    } else if (!ended) {
        g_string_append_printf(s, " (no zero byte in the first %d bytes)",
                               STRING_MAX);
    }
    return g_string_free(s, false);
}

static bool mcp_debug_mapped(CPUState *cpu, uint64_t addr)
{
    TranslateForDebugResult t;

    return cpu_translate_for_debug(cpu, addr, &t);
}

/* The instruction at @pc, or why it can't be read */
static char *mcp_debug_insn(CPUState *cpu, uint64_t pc)
{
    if (!mcp_debug_mapped(cpu, pc)) {
        return g_strdup_printf("0x%" PRIx64 ":  (not mapped for CPU %d now,"
                               " so it can't be shown; the CPU may take a"
                               " TLB miss or page fault there)\n", pc,
                               cpu->cpu_index);
    }
    return mcp_cpu_disas(cpu, pc, 1, false);
}

/* The next instruction of @cpu, its registers and the memory asked for */
static void mcp_debug_show(GString *s, CPUState *cpu, McpMem *mem, int n)
{
    g_autofree char *insn = mcp_debug_insn(cpu, cpu->cc->get_pc(cpu));
    g_autofree char *regs = mcp_cpu_registers(cpu);
    int i;

    if (!insn || !regs) {
        g_string_append(s, "\n(no registers: this QEMU has no HMP)");
        return;
    }
    /* one empty line before it */
    g_string_append(s, s->str[s->len - 1] == '\n' ? "\n" : "\n\n");
    g_string_append_printf(s, "Next instruction:\n%s\nRegisters:\n%s\n",
                           insn, g_strchomp(regs));
    for (i = 0; i < n; i++) {
        const char *what = mem[i].string ? "String" : "Memory";
        const char *bad = NULL;
        g_autofree char *dump = NULL;
        g_autofree char *where = NULL;
        uint64_t addr;

        if (mem[i].skip) {
            continue;
        }
        if (!mcp_mem_address(cpu, &mem[i], &addr, &bad)) {
            g_string_append_printf(s, "\n%s at %s: CPU %d has no integer"
                                   " register %s\n", what, mem[i].text,
                                   cpu->cpu_index, bad);
            continue;
        }
        g_autofree char *at = mem[i].physical ?
            g_strdup_printf("physical 0x%" PRIx64, addr) : mcp_addr_str(addr);

        where = g_strdup_printf("%s%s%s", mem[i].has_reg ? mem[i].text : "",
                                mem[i].has_reg ? " = " : "", at);
        if (!mem[i].physical && !mcp_debug_mapped(cpu, addr)) {
            g_string_append_printf(s, "\n%s at %s: not mapped for CPU %d now"
                                   " (step on until the guest uses it).%s"
                                   " If it is at the same physical address"
                                   " (kernels often are), ask for it with"
                                   " physical: true.\n", what, where,
                                   cpu->cpu_index, mem[i].has_reg ?
                                   " If a register holds only the offset in"
                                   " a space or segment, add its base." : "");
            continue;
        }
        if (mem[i].string) {
            dump = mcp_read_string(cpu, addr, mem[i].physical);
            g_string_append_printf(s, "\nString at %s: %s\n", where,
                                   dump ? dump : "(it can't be read: no RAM"
                                   " or ROM there)");
            continue;
        }
        if (mem[i].watched &&
            !mcp_mem_plain(cpu, addr, mem[i].words * 4, mem[i].physical)) {
            g_string_append_printf(s, "\nMemory at %s: a device's registers,"
                                   " not read, as a read may change the"
                                   " device\n", where);
            continue;
        }
        dump = mcp_cpu_memory(cpu, addr, mem[i].words, mem[i].physical);
        g_string_append_printf(s, "\nMemory at %s:\n%s", where,
                               dump ? dump : "");
    }
}

/*
 * By CPU index: the instruction that took an interruption (a TLB miss,
 * a page fault) when a debug tool last stepped it.  The CPU usually
 * comes back to run it again, and that is not a new arrival there.
 */
typedef struct McpRetryMark {
    bool set;
    uint64_t pc;
    uint32_t taken;             /* exceptions_taken after it */
} McpRetryMark;

static McpRetryMark *mcp_retry_marks;
static int mcp_n_retry_marks;

/* More interruptions than this since, and it did not come back soon */
#define RETRY_SOON      1000

static McpRetryMark *mcp_retry_mark(CPUState *cpu)
{
    int n = cpu->cpu_index + 1;

    if (n > mcp_n_retry_marks) {
        mcp_retry_marks = g_renew(McpRetryMark, mcp_retry_marks, n);
        memset(mcp_retry_marks + mcp_n_retry_marks, 0,
               (n - mcp_n_retry_marks) * sizeof(McpRetryMark));
        mcp_n_retry_marks = n;
    }
    return &mcp_retry_marks[cpu->cpu_index];
}

/*
 * Take one instruction on @cpu.  *@interrupted tells whether the CPU
 * took an interruption (a fault, a trap) instead of finishing it.
 */
static McpStepResult coroutine_fn mcp_debug_step_one(QemuInputCtx *ctx,
                                                     CPUState *cpu,
                                                     bool *interrupted)
{
    McpRetryMark *m = mcp_retry_mark(cpu);
    uint64_t pc = cpu->cc->get_pc(cpu);
    uint32_t taken = qatomic_read(&cpu->exceptions_taken);
    McpStepResult step = mcp_debug_step(ctx, cpu);

    *interrupted = step == STEP_DONE &&
                   qatomic_read(&cpu->exceptions_taken) != taken;
    if (*interrupted) {
        m->set = true;
        m->pc = pc;
        m->taken = qatomic_read(&cpu->exceptions_taken);
    } else if (step == STEP_DONE && m->pc == pc) {
        m->set = false;         /* it ran this time */
    }
    return step;
}

/*
 * Whether @cpu, at @pc, may be back to run the instruction there again
 * after it took an interruption.  That uses up its mark.
 */
static bool mcp_debug_retry(CPUState *cpu, uint64_t pc)
{
    McpRetryMark *m = mcp_retry_mark(cpu);
    uint32_t since = qatomic_read(&cpu->exceptions_taken) - m->taken;

    if (!m->set || m->pc != pc) {
        return false;
    }
    m->set = false;
    return since <= RETRY_SOON;
}

static void mcp_debug_set_cpu(McpDebugStop *res, CPUState *cpu)
{
    uint64_t pc = cpu->cc->get_pc(cpu);

    res->has_cpu = true;
    res->cpu = cpu->cpu_index;
    res->pc = g_strdup_printf("0x%" PRIx64, pc);
    res->symbol = mcp_sym_name(pc);
    mcp_debug_last_cpu = cpu->cpu_index;
}

typedef enum {
    RUN_REACHED,
    RUN_TIMEOUT,
    RUN_STOPPED,        /* the VM stopped for another reason */
    RUN_CANCELLED,
} McpRunResult;

/* What a CPU or a device did to watched memory, for each access */
static const char *const mcp_watch_verb[] = {
    [BP_MEM_WRITE] = "wrote to",
    [BP_MEM_READ] = "read",
    [BP_MEM_ACCESS] = "touched",
};

/*
 * The watched areas: "wrote to 0x2000", "writes to 0x2000 (4 bytes)",
 * each with the word from @verb for its access
 */
static void mcp_debug_append_watches(GString *s, McpBreak *b,
                                     const char *const verb[],
                                     bool with_len, const char *conj)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    int i;

    for (i = 0; i < b->n_watches; i++) {
        McpWatchArea *a = &b->watches[i];
        g_autofree char *len = with_len ?
            g_strdup_printf(" (%" PRIu64 " byte%s)", a->len,
                            a->len == 1 ? "" : "s") : NULL;
        g_autofree char *at = mcp_area_str(a, a->addr);

        g_ptr_array_add(items, g_strdup_printf("%s %s%s", verb[a->flags], at,
                                               len ? len : ""));
    }
    mcp_append_list(s, items, conj);
}

/*
 * The memory watched for devices' accesses, as the run began: "wrote to
 * 0x2000", "read I/O 0x60", or more, each with the word from @verb for
 * what is seen there, joined with @conj.  False when there is none.
 */
static bool mcp_debug_append_dev(GString *s, McpBreak *b,
                                 const char *const verb[], const char *conj)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    int i;

    for (i = 0; i < b->n_watches; i++) {
        McpWatchArea *a = &b->watches[i];
        /* devices' reads of RAM are not seen */
        int flags = a->has_regs ? a->flags : a->has_ram ? BP_MEM_WRITE : 0;
        g_autofree char *at = mcp_area_str(a, a->addr);

        if (a->devices && flags) {
            g_ptr_array_add(items, g_strdup_printf("%s %s", verb[flags], at));
        }
    }
    if (!items->len) {
        g_ptr_array_free(items, true);
        return false;
    }
    mcp_append_list(s, items, conj);
    return true;
}

/*
 * What an access to registers did: ": a byte at 0x5 in serial
 * (isa-serial)", the offset in the region; ", where nothing is"
 */
static void mcp_dev_append_reg(GString *s, const McpDevHit *h)
{
    if (h->nothing) {
        g_string_append(s, ", where nothing is");
        return;
    }
    if (h->len == 1) {
        g_string_append(s, ": a byte");
    } else {
        g_string_append_printf(s, ": %" PRIu64 " bytes", h->len);
    }
    g_string_append_printf(s, " at 0x%" PRIx64 " in %s", h->offset,
                           h->region);
    if (!h->ok) {
        g_string_append(s, ", which did not take it");
    }
}

/* "wrote 0x41 to I/O 0x3f8", "read 0x1c from physical 0xfe000005" */
static void mcp_dev_append_value(GString *s, McpBreak *b, const McpDevHit *h)
{
    g_autofree char *at = mcp_area_str(&b->watches[h->area], h->addr);

    g_string_append_printf(s, "%s 0x%" PRIx64 " %s %s",
                           h->write ? "wrote" : "read", h->value,
                           h->write ? "to" : "from", at);
}

/*
 * What a device did: "a device wrote to 0x2000 (physical 0x2000) from
 * the main loop, 4 bytes in all"; "a device read 0x41 from I/O 0x3f8
 * from the main loop: a byte at 0x0 in serial (isa-serial)"
 */
static void mcp_dev_append(GString *s, McpBreak *b, const McpDevHit *h)
{
    static const char *const from[] = {
        [DEV_FROM_MAIN] = "from the main loop",
        [DEV_FROM_IOTHREAD] = "from an I/O thread",
        [DEV_FROM_OTHER] = "from another thread",
    };
    McpWatchArea *a = &b->watches[h->area];

    g_string_append(s, "a device ");
    if (h->reg) {
        mcp_dev_append_value(s, b, h);
    } else if (a->physical) {
        g_string_append_printf(s, "wrote to physical 0x%" PRIx64, h->addr);
    } else {
        g_autofree char *at = mcp_addr_str(h->addr);

        g_string_append_printf(s, "wrote to %s (physical 0x%" PRIx64 ")", at,
                               h->phys);
    }
    if (h->from == DEV_FROM_CPU) {
        g_string_append_printf(s, " during CPU %d's I/O access", h->cpu);
    } else {
        g_string_append_printf(s, " %s", from[h->from]);
    }
    if (h->reg) {
        mcp_dev_append_reg(s, h);
    } else {
        g_string_append_printf(s, ", %" PRIu64 " byte%s in all", h->len,
                               h->len == 1 ? "" : "s");
    }
}

static McpDeviceWrite *mcp_dev_hit(const McpDevHit *h)
{
    static const char *const from[] = {
        [DEV_FROM_CPU] = "cpu",
        [DEV_FROM_MAIN] = "main-loop",
        [DEV_FROM_IOTHREAD] = "io-thread",
        [DEV_FROM_OTHER] = "other-thread",
    };
    McpDeviceWrite *d = g_new0(McpDeviceWrite, 1);

    if (h->has_phys) {
        d->physical = g_strdup_printf("0x%" PRIx64, h->phys);
    }
    d->length = h->len;
    d->from = g_strdup(from[h->from]);
    d->has_cpu = h->from == DEV_FROM_CPU;
    d->cpu = h->cpu;
    return d;
}

/* What the hit @seen of @b did to the watched memory, for a result */
static McpWatchHit *mcp_watch_hit(McpBreak *b, const McpWatchSeen *seen)
{
    McpWatchArea *a = &b->watches[seen->area];
    McpWatchHit *w = g_new0(McpWatchHit, 1);
    const McpDevHit *h = &seen->dev;

    w->address = g_strdup_printf("0x%" PRIx64, seen->addr);
    w->has_physical = a->physical;
    w->physical = a->physical;
    if (a->as) {
        w->space = g_strdup(a->space);
    }
    w->access = g_strdup(seen->write ? "write" : "read");
    if (seen->device) {
        w->device = mcp_dev_hit(h);
    } else if (seen->by_insn) {
        w->pc = g_strdup_printf("0x%" PRIx64, seen->pc);
    } else {
        w->has_block_end = true;
        w->block_end = true;
    }
    if (seen->has_dev && h->reg) {
        w->value = g_strdup_printf("0x%" PRIx64, h->value);
        w->has_size = true;
        w->size = h->len;
        if (!h->nothing) {
            w->region = g_strdup(h->region);
            w->offset = g_strdup_printf("0x%" PRIx64, h->offset);
        }
    }
    return w;
}

/*
 * The exceptions of @b: "exception 14" (@one and the number), "exceptions
 * 13 or 14" (@many and the numbers, joined with @conj), or with none
 * given @any, as in "an exception", or "an exception other than 2 or 3"
 */
static void mcp_debug_append_excp(GString *s, McpBreak *b, const char *one,
                                  const char *many, const char *any,
                                  const char *conj)
{
    CPUExcpWatch *w = &b->excp;
    bool skip = w->n_skip;
    int n = skip ? w->n_skip : w->n_only;
    int32_t *num = skip ? w->skip : w->only;
    GPtrArray *items;
    int i;

    if (!n) {
        g_string_append(s, any);
        return;
    }
    items = g_ptr_array_new_with_free_func(g_free);
    for (i = 0; i < n; i++) {
        g_ptr_array_add(items, g_strdup_printf("%" PRId32, num[i]));
    }
    if (skip) {
        g_string_append_printf(s, "%s other than ", any);
        mcp_append_list(s, items, "or");
    } else {
        g_string_append_printf(s, "%s ", n == 1 ? one : many);
        mcp_append_list(s, items, conj);
    }
}

/*
 * "reached 0x1000", "wrote to 0x2000", "took exception 14", or more of
 * them, for the summary
 */
static void mcp_debug_what(GString *s, McpBreak *b)
{
    const char *sep = "";

    if (b->n_addrs) {
        g_string_append(s, "reached ");
        mcp_debug_append_addrs(s, b, "or");
        sep = " or ";
    }
    if (b->n_watches) {
        g_string_append(s, sep);
        mcp_debug_append_watches(s, b, mcp_watch_verb, false, "or");
        sep = " or ";
    }
    if (b->has_excp) {
        g_string_append(s, sep);
        mcp_debug_append_excp(s, b, "took exception", "took exceptions",
                              "took an exception", "or");
    }
}

/*
 * What none of the CPUs and devices did, or what one did not do before:
 * "no CPU wrote to 0x2000, and no device wrote to 0x2000" with @cpu "no
 * CPU", @join ", and ", @dev "no device"; "a CPU reached 0x1000, or a
 * device wrote to 0x2000" with "a CPU", ", or ", "a device"
 */
static void mcp_debug_what_all(GString *s, McpBreak *b, const char *cpu,
                               const char *join, const char *dev)
{
    bool cpus = b->n_addrs || b->has_excp || b->n_watches;
    g_autoptr(GString) devs = g_string_new(NULL);

    if (cpus) {
        g_string_append_printf(s, "%s ", cpu);
        mcp_debug_what(s, b);
    }
    if (mcp_debug_append_dev(devs, b, mcp_watch_verb, "or")) {
        g_string_append_printf(s, "%s%s %s", cpus ? join : "", dev,
                               devs->str);
    }
}

/* A debug-run or a debug-trace: what it waits for, and what it saw */
typedef struct McpRun McpRun;
struct McpRun {
    McpBreak b;
    QemuInputCtx *ctx;
    int64_t limit;              /* the hits to wait for */
    int64_t end;                /* QEMU_CLOCK_REALTIME, ms */
    int history;                /* blocks to note, 0 for none */
    /*
     * debug-trace: each hit, with the VM stopped there.  @seen is the
     * watched access, @excp the exception, or neither for an arrival.
     */
    void (*log)(McpRun *r, CPUState *cpu, const McpWatchSeen *seen,
                const McpExcpSeen *excp, bool retry);
    int64_t n_hits;
    int64_t n_retries;
    CPUState *at;               /* the last hit */
    bool by_watch;
    bool by_excp;
    bool retry;
    McpWatchSeen seen;
    McpExcpSeen excp;
    bool late;                  /* hits as the time ran out */
    uint64_t dev_more;          /* accesses no hit took, after the last */
    McpRunResult result;
};

static void mcp_run_hit(McpRun *r, CPUState *cpu, const McpWatchSeen *seen,
                        const McpExcpSeen *excp, bool retry)
{
    r->n_hits++;
    r->n_retries += retry;
    r->at = cpu;
    r->by_watch = seen;
    r->by_excp = excp;
    r->retry = retry;
    if (seen) {
        r->seen = *seen;
    }
    if (excp) {
        r->excp = *excp;
    }
    if (r->log) {
        r->log(r, cpu, seen, excp, retry);
    }
}

/* The CPUs stopped at the address have got there */
static void mcp_run_arrivals(McpRun *r)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (mcp_debug_is_hit(&r->b, cpu)) {
            mcp_cond_forget(r->b.cond, cpu);
            mcp_run_hit(r, cpu, NULL, NULL,
                        mcp_debug_retry(cpu, cpu->cc->get_pc(cpu)));
        }
    }
}

/*
 * The accesses to watched memory that no CPU watchpoint caught, noted
 * since the run last looked; the VM is stopped.  Each is a hit when the
 * condition holds for the CPU it shows: the one that made it, or in
 * whose I/O access it came, else the one the tool shows.  How many
 * accesses it looked at.
 */
static int mcp_run_dev(McpRun *r)
{
    g_autofree McpDevHit *w = g_new(McpDevHit, DEV_PENDING_MAX);
    McpBreak *b = &r->b;
    uint64_t lost;
    int n = mcp_dev_take(w, &lost), i;

    for (i = 0; i < n; i++) {
        CPUState *cpu = w[i].from == DEV_FROM_CPU ? qemu_get_cpu(w[i].cpu)
                                                  : NULL;
        McpWatchSeen seen = { 0 };

        cpu = cpu ? cpu : b->only ? b->only : b->wcpu;
        if (r->n_hits >= r->limit) {
            r->dev_more++;
            continue;
        }
        if (b->cond) {
            bool holds = mcp_cond_test(cpu, &b->cond->base);

            /* not an arrival, if the CPU stands at an address */
            mcp_cond_forget(b->cond, cpu);
            if (!holds) {
                continue;
            }
        }
        seen.cpu = cpu;
        seen.area = w[i].area;
        seen.addr = w[i].addr;
        seen.pc = cpu->cc->get_pc(cpu);
        seen.write = w[i].write;
        /* a CPU's own access to registers, outside its watchpoints */
        seen.device = !w[i].reg || w[i].from != DEV_FROM_CPU;
        seen.has_dev = true;
        seen.dev = w[i];
        mcp_run_hit(r, cpu, &seen, NULL, false);
    }
    r->dev_more += lost;
    return n + !!lost;
}

#define HISTORY_MAX     1024

/*
 * Note the last @n blocks of code that the CPUs of @b run.  The VM is
 * stopped.  Their blocks no longer chain, so the guest runs slower.
 */
static void mcp_history_on(McpBreak *b, int n)
{
    uint32_t size = pow2ceil(n);
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        CPUTBHistory *h;

        if (b->only && cpu != b->only) {
            continue;
        }
        h = g_malloc0(sizeof(*h) + size * sizeof(h->e[0]));
        h->size = size;
        g_free(cpu->tb_history);
        cpu->tb_history = h;
        tcg_update_cflags(cpu);
    }
}

/* Stop noting them; the vCPUs pause for it if the VM runs */
static void mcp_history_off(void *opaque)
{
    bool running = runstate_is_running();
    CPUState *cpu;

    if (running) {
        pause_all_vcpus();
    }
    CPU_FOREACH(cpu) {
        CPUTBHistory *h = cpu->tb_history;

        cpu->tb_history = NULL;
        tcg_update_cflags(cpu);
        g_free(h);
    }
    if (running) {
        resume_all_vcpus();
    }
}

/* The last @n blocks @cpu ran, oldest first: "0x1000-0x1013 x5" */
static strList *mcp_history_lines(CPUState *cpu, int n)
{
    CPUTBHistory *h = cpu->tb_history;
    strList *list = NULL, **tail = &list;
    uint64_t i;

    if (!h) {
        return NULL;
    }
    for (i = MIN(h->count, (uint64_t)n); i > 0; i--) {
        CPUTBHistoryEntry *e = &h->e[(h->next - i) & (h->size - 1)];
        g_autofree char *end = e->size ?
            g_strdup_printf("-0x%" PRIx64, (uint64_t)e->pc + e->size - 1) :
            NULL;
        g_autofree char *rep = e->repeat > 1 ?
            g_strdup_printf(" x%" PRIu32, e->repeat) : NULL;
        g_autofree char *name = mcp_sym_name(e->pc);

        QAPI_LIST_APPEND(tail, g_strdup_printf("0x%" PRIx64 "%s%s%s%s%s",
                                               (uint64_t)e->pc,
                                               end ? end : "",
                                               rep ? rep : "",
                                               name ? " <" : "",
                                               name ? name : "",
                                               name ? ">" : ""));
    }
    return list;
}

/* What @cpu stopped for besides an arrival: a watched access, an exception */
static void mcp_run_access(McpRun *r, CPUState *cpu)
{
    McpWatchSeen seen;
    McpExcpSeen excp;

    if (mcp_watch_take(&r->b, cpu, &seen)) {
        mcp_run_hit(r, cpu, &seen, NULL, false);
    }
    if (mcp_excp_take(&r->b, cpu, &excp)) {
        mcp_run_hit(r, cpu, NULL, &excp, false);
    }
}

/*
 * Let the guest run until @r has seen its hits, the time runs out, or
 * the VM stops for another reason.  The breakpoint and the watchpoint
 * are taken away at the end, and the VM is left as it is.
 */
static void coroutine_fn mcp_run(McpRun *r)
{
    McpBreak *b = &r->b;
    QemuInputCtx *ctx = r->ctx;
    RunState paused = RUN_STATE_PAUSED;
    McpStepResult step;
    CPUState *cpu;
    int64_t before;
    bool inserted = false, interrupted;
    int looked;

    mcp_dev_init();
    r->result = RUN_TIMEOUT;
    /* a CPU there when the running VM stops has got there */
    if (runstate_is_running()) {
        mcp_debug_outside(mcp_debug_stop, &paused);
        if (!runstate_is_running()) {
            mcp_run_arrivals(r);
        }
    }
    if (r->history && !runstate_is_running()) {
        mcp_history_on(b, r->history);
    }
    while (r->n_hits < r->limit) {
        if (runstate_is_running()) {
            r->result = RUN_STOPPED;    /* someone else started it */
            break;
        }
        if (!inserted) {
            mcp_debug_insert(b);
            inserted = true;
        }
        /* a CPU stopped at the address goes on to the next arrival */
        step = STEP_DONE;
        CPU_FOREACH(cpu) {
            if (mcp_debug_is_at(b, cpu)) {
                step = mcp_debug_step_one(ctx, cpu, &interrupted);
                /* that instruction may touch watched memory, or trap */
                mcp_run_access(r, cpu);
                if (step != STEP_DONE && step != STEP_HALTED) {
                    break;
                }
            }
        }
        /* devices may have written to it, even with the VM stopped */
        mcp_run_dev(r);
        if (r->n_hits >= r->limit) {
            break;
        }
        if (step == STEP_STOPPED || step == STEP_NOT_STARTED) {
            r->result = RUN_STOPPED;
            break;
        }
        if (ctx->cancelled) {
            r->result = RUN_CANCELLED;
            break;
        }
        if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) >= r->end) {
            break;
        }

        mcp_debug_outside(mcp_debug_start, NULL);
        /*
         * A device that wrote while the VM was stopped asked for no stop.
         * A write since then did, and the VM has not stopped for it yet
         * while it runs: this request joins that one.
         */
        if (runstate_is_running() && mcp_dev_pending()) {
            qemu_system_debug_request();
        }
        if (!mcp_debug_wait_stop(ctx, r->end, NULL)) {
            r->result = ctx->cancelled ? RUN_CANCELLED : RUN_TIMEOUT;
            mcp_debug_outside(mcp_debug_remove_running, b);
            inserted = false;
            if (b->hits || b->whits || b->ehits || b->dhits) {
                /* got there as the time ran out; its stop comes next */
                mcp_debug_wait_stop(ctx, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                                    + STEP_WAIT_MS, NULL);
                before = r->n_hits;
                mcp_run_arrivals(r);
                if (b->whits) {
                    mcp_run_hit(r, b->seen.cpu, &b->seen, NULL, false);
                }
                if (b->ehits) {
                    mcp_run_hit(r, b->eseen.cpu, NULL, &b->eseen, false);
                }
                mcp_run_dev(r);
                /* devices' writes that the condition left out don't count */
                if (r->n_hits != before) {
                    r->late = true;
                    r->result = r->n_hits >= r->limit ? RUN_REACHED
                                                      : RUN_STOPPED;
                }
            }
            break;
        }
        before = r->n_hits;
        looked = 0;
        if (runstate_check(RUN_STATE_DEBUG)) {
            mcp_run_arrivals(r);
            CPU_FOREACH(cpu) {
                mcp_run_access(r, cpu);
            }
            looked = mcp_run_dev(r);
        }
        /* devices' writes that the condition left out: go on */
        if (r->n_hits == before && !looked) {
            r->at = NULL;
            r->result = RUN_STOPPED;
            break;
        }
    }
    if (r->n_hits >= r->limit) {
        r->result = RUN_REACHED;
    }
    if (inserted) {
        mcp_debug_outside(mcp_debug_remove_paused, b);
    }
    /* devices' writes noted after the last hit, which none took */
    r->dev_more += mcp_dev_drop();
}

/* The addresses to stop at, one or several, into @b */
static bool mcp_parse_addrs(McpAddresses *address, McpBreak *b, Error **errp)
{
    strList one = { 0 };
    strList *l;

    if (address->type == QTYPE_QSTRING) {
        one.value = address->u.one;
        l = &one;
    } else {
        l = address->u.several;
        if (!l) {
            error_setg(errp, "give at least one address");
            return false;
        }
    }
    for (; l; l = l->next) {
        uint64_t a;
        int i;

        if (!mcp_parse_place(l->value, "address", true, &a, errp)) {
            return false;
        }
        /* the same address twice is one breakpoint */
        for (i = 0; i < b->n_addrs; i++) {
            if (b->addrs[i] == a) {
                break;
            }
        }
        if (i < b->n_addrs) {
            continue;
        }
        if (b->n_addrs == RUN_ADDRS_MAX) {
            error_setg(errp, "give at most %d addresses", RUN_ADDRS_MAX);
            return false;
        }
        b->addrs[b->n_addrs++] = a;
    }
    return true;
}

/* Exception numbers from @l into @num, which has *@n */
static bool mcp_parse_excp_numbers(intList *l, int32_t *num, int *n,
                                   Error **errp)
{
    for (; l; l = l->next) {
        int i;

        /* from EXCP_INTERRUPT on, they are QEMU's, not the CPU's */
        if (l->value < 0 || l->value >= EXCP_INTERRUPT) {
            error_setg(errp, "exception numbers are 0 to %d",
                       EXCP_INTERRUPT - 1);
            return false;
        }
        for (i = 0; i < *n; i++) {
            if (num[i] == l->value) {
                break;
            }
        }
        if (i < *n) {
            continue;
        }
        if (*n == CPU_EXCP_WATCH_MAX) {
            error_setg(errp, "give at most %d exception numbers",
                       CPU_EXCP_WATCH_MAX);
            return false;
        }
        num[(*n)++] = l->value;
    }
    return true;
}

/* The exceptions to stop at, into @b */
static bool mcp_parse_excp(McpExceptions *e, McpBreak *b, Error **errp)
{
    CPUExcpWatch *w = &b->excp;

    if (e->numbers && e->skip) {
        error_setg(errp, "give the exceptions to stop at or the ones to"
                   " skip, not both");
        return false;
    }
    if (!mcp_parse_excp_numbers(e->numbers, w->only, &w->n_only, errp) ||
        !mcp_parse_excp_numbers(e->skip, w->skip, &w->n_skip, errp)) {
        return false;
    }
    b->has_excp = true;
    return true;
}

/*
 * Where to stop or what to log: addresses, watched memory, exceptions,
 * a CPU
 */
static bool mcp_run_parse(McpRun *r, McpAddresses *address,
                          const char *address_mask, McpExceptions *exception,
                          bool has_cpu, int64_t cpu_index, Error **errp)
{
    if (address && !mcp_parse_addrs(address, &r->b, errp)) {
        return false;
    }
    if (exception && !mcp_parse_excp(exception, &r->b, errp)) {
        return false;
    }
    r->b.mask = UINT64_MAX;
    if (address_mask) {
        if (!address) {
            error_setg(errp, "address-mask needs an address");
            return false;
        }
        if (!mcp_parse_address(address_mask, "address-mask", &r->b.mask,
                               errp)) {
            return false;
        }
        if (!r->b.mask) {
            error_setg(errp, "address-mask must keep at least one bit");
            return false;
        }
        r->b.has_mask = true;
    }
    if (has_cpu) {
        r->b.only = mcp_debug_cpu(true, cpu_index, errp);
        if (!r->b.only) {
            return false;
        }
    }
    return true;
}

/*
 * What the last hit of @r was: "CPU 0 reached 0x1000", "CPU 0 wrote to
 * 0x2000", "CPU 0 wrote 0x41 to I/O 0x3f8: a byte at 0x0 in serial
 * (isa-serial)", "CPU 0 took exception 14 at 0x1000", "a device wrote
 * to 0x2000 (physical 0x2000) from the main loop, 4 bytes in all"
 */
static void mcp_run_append_hit(GString *s, McpRun *r)
{
    McpWatchSeen *seen = &r->seen;
    g_autofree char *at = r->by_watch ?
        mcp_area_str(&r->b.watches[seen->area], seen->addr) :
        mcp_addr_str(r->by_excp ? r->excp.from :
                     mcp_debug_reached(&r->b, r->at));

    if (r->by_watch && seen->device) {
        mcp_dev_append(s, &r->b, &seen->dev);
        return;
    }
    g_string_append_printf(s, "CPU %d ", r->at->cpu_index);
    if (r->by_watch && seen->has_dev && seen->dev.reg) {
        mcp_dev_append_value(s, &r->b, &seen->dev);
        mcp_dev_append_reg(s, &seen->dev);
    } else if (r->by_watch) {
        g_string_append_printf(s, "%s %s", seen->write ? "wrote to" : "read",
                               at);
    } else if (r->by_excp) {
        g_string_append_printf(s, "took exception %" PRId32 " at %s",
                               r->excp.number, at);
    } else {
        g_string_append_printf(s, "reached %s", at);
    }
}

/*
 * " (3 more accesses to the watched memory, not counted)": devices' or
 * CPUs' that no CPU watchpoint caught, past the hits asked for, or more
 * than the run could note at once
 */
static void mcp_run_append_more(GString *s, McpRun *r)
{
    if (r->dev_more) {
        g_string_append_printf(s, " (%" PRIu64 " more access%s to the"
                               " watched memory, not counted)", r->dev_more,
                               r->dev_more == 1 ? "" : "es");
    }
}

/*
 * A paragraph on watched memory that devices' accesses were not seen at,
 * and one on watched memory where no device was
 */
static void mcp_dev_append_skipped(GString *s, McpBreak *b)
{
    if (b->dev_skip) {
        g_autofree char *at = mcp_area_str(&b->watches[b->dev_skip_area],
                                           b->dev_skip_at);

        g_string_append(s, s->len && s->str[s->len - 1] == '\n' ? "\n"
                                                                 : "\n\n");
        g_string_append_printf(s, "Devices' accesses were not watched at %s"
                               " and maybe further on: %s.", at, b->dev_skip);
    }
    if (b->dev_none) {
        McpWatchArea *a = &b->watches[b->dev_none_area];
        g_autofree char *at = mcp_area_str(a, a->addr);

        g_string_append(s, s->len && s->str[s->len - 1] == '\n' ? "\n"
                                                                 : "\n\n");
        g_string_append_printf(s, "No device was at %s as the run began.",
                               at);
        /* some machines have their I/O ports in another space */
        if (a->as) {
            g_autofree char *names = address_space_names();

            g_string_append_printf(s, " The address spaces are: %s; hmp's"
                                   " \"info mtree\" shows what each holds."
                                   " Where the CPU sees the device in its"
                                   " memory, a watch with physical: true"
                                   " there works too.", names);
        }
    }
}

/* "arrival", "access", "exception", or "hit" when it may be any of them */
static const char *mcp_run_hit_word(McpBreak *b)
{
    if (!!b->n_addrs + !!b->n_watches + b->has_excp > 1) {
        return "hit";
    }
    return b->n_watches ? "access" : b->has_excp ? "exception" : "arrival";
}

McpDebugStop *coroutine_fn qmp_debug_run(McpAddresses *address,
                                         const char *address_mask,
                                         McpWatches *watch,
                                         McpExceptions *exception,
                                         const char *condition,
                                         bool has_hits, int64_t hits,
                                         bool has_cpu, int64_t cpu_index,
                                         bool has_memory,
                                         McpDebugMemoryList *memory,
                                         bool has_history, int64_t history,
                                         bool has_timeout_ms,
                                         int64_t timeout_ms, Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(McpCond) cond = NULL;
    g_autofree char *count = NULL;
    g_autofree McpMem *mem = NULL;
    McpRun r = { .ctx = mcp_current_input_ctx() };
    McpBreak *b = &r.b;
    McpWatchSeen *seen = &r.seen;
    McpDebugStop *res;
    CPUState *at, *cpu;
    strList *hist = NULL, *l;
    int n_mem = 0, n;
    const char *hit_word;
    bool name_cpu;

    if (!r.ctx) {
        qemu_input_ctx_init(&local, NULL);
        r.ctx = &local;
    }
    if (!address && !watch && !exception) {
        error_setg(errp, "give an address to stop at, memory to watch,"
                   " exceptions, or more of them");
        return NULL;
    }
    hits = has_hits ? hits : 1;
    if (hits < 1 || hits > RUN_HITS_MAX) {
        error_setg(errp, "hits must be 1 to %d", RUN_HITS_MAX);
        return NULL;
    }
    timeout_ms = has_timeout_ms ? timeout_ms : RUN_DEFAULT_MS;
    timeout_ms = MIN(MAX(timeout_ms, 0), mcp_wait_max_ms());
    if (has_history && (history < 0 || history > HISTORY_MAX)) {
        error_setg(errp, "history must be 0 to %d", HISTORY_MAX);
        return NULL;
    }
    r.history = has_history ? history : 0;
    if (!mcp_run_parse(&r, address, address_mask, exception, has_cpu,
                       cpu_index, errp)) {
        return NULL;
    }
    if (condition) {
        cond = mcp_cond_parse(b->only ? b->only : first_cpu, condition, errp);
        if (!cond) {
            return NULL;
        }
        b->cond = cond;
    }
    mem = mcp_parse_memory(b->only ? b->only : first_cpu, memory, &n_mem,
                           errp);
    if (!mem) {
        return NULL;
    }
    if (watch && !mcp_parse_watches(watch, b, &mem[n_mem], errp)) {
        return NULL;
    }
    if (!mcp_debug_begin(r.ctx, errp)) {
        return NULL;
    }
    r.end = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + timeout_ms;
    r.limit = hits;
    hit_word = mcp_run_hit_word(b);

    mcp_run(&r);
    at = r.at;
    cpu = at ? at : b->only ? b->only : first_cpu;
    if (r.history) {
        /* how the CPU shown got there, read while it is stopped */
        if (!runstate_is_running()) {
            hist = mcp_history_lines(cpu, r.history);
        }
        mcp_debug_outside(mcp_history_off, NULL);
    }
    /* the time ran out or the call was cancelled: the guest goes on */
    if ((r.result == RUN_TIMEOUT || r.result == RUN_CANCELLED) &&
        !runstate_is_running()) {
        mcp_debug_outside(mcp_debug_start, NULL);
    }
    mcp_debug_end();
    if (r.result == RUN_CANCELLED) {
        qapi_free_strList(hist);
        error_setg(errp, "cancelled");
        return NULL;
    }

    res = g_new0(McpDebugStop, 1);
    res->reached = r.result == RUN_REACHED;
    res->has_hits = true;
    res->hits = r.n_hits;
    res->has_skipped = cond != NULL;
    res->skipped = mcp_cond_skipped(cond);
    if (at) {
        mcp_debug_set_cpu(res, at);
    }
    res->running = runstate_is_running();
    if (at && r.retry) {
        res->has_retry = true;
        res->retry = true;
    }
    if (at && r.by_watch) {
        res->watch = mcp_watch_hit(b, seen);
    }
    if (at && r.by_excp) {
        res->exception = g_new0(McpExceptionHit, 1);
        res->exception->number = r.excp.number;
        res->exception->pc = g_strdup_printf("0x%" PRIx64, r.excp.from);
        res->exception->handler = g_strdup_printf("0x%" PRIx64, r.excp.to);
    }
    /* a device's access, from no CPU: say whose registers follow */
    name_cpu = !at || (r.by_watch && seen->device &&
                       seen->dev.from != DEV_FROM_CPU);
    switch (r.result) {
    case RUN_REACHED:
        mcp_run_append_hit(s, &r);
        if (hits > 1) {
            count = g_strdup_printf("%s %" PRId64, hit_word, r.n_hits);
        }
        mcp_cond_append(s, count, cond);
        mcp_run_append_more(s, &r);
        if (r.by_watch && seen->device) {
            g_string_append(s, "; the VM is stopped right after it");
            if (!name_cpu) {
                g_string_append_printf(s, ", though CPU %d may have run on"
                                       " to the end of its block of code",
                                       at->cpu_index);
            }
            break;
        }
        if (r.by_watch && !seen->by_insn) {
            g_string_append(s, "; the VM is stopped where the CPU's block of"
                            " code ended: right after the instruction that"
                            " did it, or a few instructions on");
            break;
        }
        if (r.by_watch) {
            g_autofree char *insn = mcp_debug_insn(at, seen->pc);

            g_string_append_printf(s, "; the VM is stopped after the"
                                   " instruction that did it:\n%s",
                                   insn ? insn : "");
            break;
        }
        if (r.by_excp) {
            g_autofree char *insn = mcp_debug_insn(at, r.excp.from);
            g_autofree char *handler = mcp_addr_str(r.excp.to);

            g_string_append_printf(s, "; the VM is stopped at the first"
                                   " instruction of the guest's handler"
                                   " for it, %s. The instruction that took"
                                   " it:\n%s", handler, insn ? insn : "");
            break;
        }
        g_string_append(s, "; the VM is stopped");
        if (r.retry) {
            g_string_append(s, ". This may be the CPU coming back to run"
                            " the instruction again: when it went on from"
                            " here, the instruction took an interruption"
                            " first (such as a TLB miss). Run debug-run"
                            " again to go on");
        }
        if (hits > 1 && r.n_retries) {
            g_string_append_printf(s, " (%" PRId64 " of the arrivals came"
                                   " right after an interruption there)",
                                   r.n_retries);
        }
        break;
    case RUN_TIMEOUT:
        mcp_debug_what_all(s, b, "no CPU", ", and ", "no device");
        g_string_append_printf(s, " in %.1f s", timeout_ms / 1000.0);
        if (hits > 1) {
            count = g_strdup_printf("%" PRId64 " of %" PRId64 " %ss",
                                    r.n_hits, hits, hit_word);
        }
        mcp_cond_append(s, count, cond);
        g_string_append(s, "; the VM keeps running");
        break;
    case RUN_STOPPED:
        if (at) {
            mcp_run_append_hit(s, &r);
            count = g_strdup_printf("%s %" PRId64 " of %" PRId64, hit_word,
                                    r.n_hits, hits);
            g_string_append(s, " as the time ran out");
            mcp_cond_append(s, count, cond);
            mcp_run_append_more(s, &r);
            g_string_append(s, "; the VM is stopped");
        } else {
            if (res->running) {
                g_string_assign(s, "something else started the VM");
            } else {
                g_string_printf(s, "the VM stopped (%s)",
                                RunState_str(runstate_get()));
            }
            g_string_append(s, " before ");
            mcp_debug_what_all(s, b, "a CPU", ", or ", "a device");
            mcp_cond_append(s, NULL, cond);
        }
        break;
    default:
        g_assert_not_reached();
    }
    if (!res->running) {
        if (name_cpu) {
            g_string_append_printf(s, "; CPU %d:", cpu->cpu_index);
        }
        if (hist) {
            for (l = hist, n = 0; l; l = l->next) {
                n++;
            }
            g_string_append_printf(s, "\n\nHow it got here, the last %d"
                                   " block%s of code CPU %d ran, oldest"
                                   " first:", n, n == 1 ? "" : "s",
                                   cpu->cpu_index);
            for (l = hist; l; l = l->next) {
                g_string_append_printf(s, "\n  %s", l->value);
            }
            res->has_history = true;
            res->history = g_steal_pointer(&hist);
        }
        /* and the watched memory, after the memory asked for */
        mcp_debug_show(s, cpu, mem, n_mem + b->n_watches);
    }
    mcp_dev_append_skipped(s, b);
    qapi_free_strList(hist);
    res->summary = g_strdup(s->str);
    return res;
}

#define TRACE_COUNT             1000
#define TRACE_COUNT_MAX         100000
#define TRACE_LIST              20
#define TRACE_LIST_MAX          500
#define TRACE_INSNS_MAX         100
#define TRACE_EXCPS_MAX         64
#define TRACE_REGS_MAX          16
#define TRACE_DEFAULT_MS        10000

/* @words 32-bit words at @addr, in hex, in the order hmp's x shows */
static char *mcp_read_words(CPUState *cpu, uint64_t addr, int words,
                            bool physical)
{
    g_autofree uint8_t *buf = g_malloc(words * 4);
    GString *s;
    bool ok;
    int i;

    if (physical) {
        ok = address_space_read(cpu->as, addr, MEMTXATTRS_UNSPECIFIED, buf,
                                words * 4) == MEMTX_OK;
    } else {
        ok = cpu_memory_rw_debug(cpu, addr, buf, words * 4, false) == 0;
    }
    if (!ok) {
        return NULL;
    }
    s = g_string_new(NULL);
    for (i = 0; i < words; i++) {
        uint32_t v = target_big_endian() ? ldl_be_p(buf + 4 * i)
                                         : ldl_le_p(buf + 4 * i);

        g_string_append_printf(s, "%s0x%08" PRIx32, i ? " " : "", v);
    }
    return g_string_free(s, false);
}

/* Memory for a trace line: [sp+4]=0x00001000, or [r26]="text" */
static char *mcp_trace_memory(CPUState *cpu, McpMem *m)
{
    g_autofree char *v = NULL;
    const char *bad = NULL;
    const char *phys = m->physical ? ", physical" : "";
    uint64_t addr;

    if (!mcp_mem_address(cpu, m, &addr, &bad)) {
        return g_strdup_printf("[%s%s]=? (CPU %d has no integer register"
                               " %s)", m->text, phys, cpu->cpu_index, bad);
    }
    if (!m->physical && !mcp_debug_mapped(cpu, addr)) {
        return g_strdup_printf("[%s]=not mapped", m->text);
    }
    /* at each hit, while the guest runs: a read may change a device */
    if (!mcp_mem_plain(cpu, addr, m->string ? 1 : m->words * 4,
                       m->physical)) {
        return g_strdup_printf("[%s%s]=? (a device's registers, not read)",
                               m->text, phys);
    }
    v = m->string ? mcp_read_string(cpu, addr, m->physical)
                  : mcp_read_words(cpu, addr, m->words, m->physical);
    return g_strdup_printf("[%s%s]=%s", m->text, phys,
                           v ? v : "? (no RAM or ROM there)");
}

/* An instruction that touched the watched memory, and how often */
typedef struct McpTraceCount {
    uint64_t pc;
    int64_t count;
    char *insn;                 /* the first time it could be read */
    bool mapped;
} McpTraceCount;

static void mcp_trace_count_free(gpointer p)
{
    McpTraceCount *c = p;

    g_free(c->insn);
    g_free(c);
}

static gint mcp_trace_count_cmp(gconstpointer a, gconstpointer b)
{
    const McpTraceCount *x = *(McpTraceCount *const *)a;
    const McpTraceCount *y = *(McpTraceCount *const *)b;

    if (x->count != y->count) {
        return x->count > y->count ? -1 : 1;
    }
    return x->pc < y->pc ? -1 : x->pc > y->pc;
}

/* An exception CPUs took, and how often */
typedef struct McpTraceExcp {
    int32_t number;
    int64_t count;
    uint64_t handler;           /* where the first one went */
} McpTraceExcp;

static gint mcp_trace_excp_cmp(gconstpointer a, gconstpointer b)
{
    const McpTraceExcp *x = *(McpTraceExcp *const *)a;
    const McpTraceExcp *y = *(McpTraceExcp *const *)b;

    if (x->count != y->count) {
        return x->count > y->count ? -1 : 1;
    }
    return x->number < y->number ? -1 : x->number > y->number;
}

/* A device's register that the watched accesses went to, and how often */
typedef struct McpTraceReg {
    char *key;
    int area;
    uint64_t addr;              /* the first watched byte, as watched */
    char region[DEV_REGION_MAX];
    uint64_t offset;            /* in the region */
    bool nothing;
    bool write;
    int64_t count;
    uint64_t value;             /* the last one */
} McpTraceReg;

static void mcp_trace_reg_free(gpointer p)
{
    McpTraceReg *g = p;

    g_free(g->key);
    g_free(g);
}

static gint mcp_trace_reg_cmp(gconstpointer a, gconstpointer b)
{
    const McpTraceReg *x = *(McpTraceReg *const *)a;
    const McpTraceReg *y = *(McpTraceReg *const *)b;

    if (x->count != y->count) {
        return x->count > y->count ? -1 : 1;
    }
    return strcmp(x->key, y->key);
}

typedef struct McpTracer {
    McpRun r;
    McpMem *mem;
    int n_mem;
    strList *regs;
    int64_t list;               /* hits to list in full */
    GString *text;              /* the hits listed */
    McpTraceEntryList *entries;
    McpTraceEntryList **tail;
    int64_t n_listed;
    GHashTable *insns;          /* McpTraceCount by pc */
    int64_t n_other;            /* by instructions past TRACE_INSNS_MAX */
    /* CPUs' accesses that no watchpoint caught, by where they stopped */
    GHashTable *blocks;         /* McpTraceCount by pc */
    int64_t n_other_blocks;
    GHashTable *dregs;          /* McpTraceReg by key */
    int64_t n_other_dregs;
    GHashTable *excps;          /* McpTraceExcp by number */
    int64_t n_other_excps;      /* past TRACE_EXCPS_MAX numbers */
    int64_t n_dev;              /* devices' accesses */
} McpTracer;

/* Count exception @excp by its number */
static void mcp_trace_count_excp(McpTracer *t, const McpExcpSeen *excp)
{
    McpTraceExcp *x = g_hash_table_lookup(t->excps, &excp->number);

    if (!x && g_hash_table_size(t->excps) < TRACE_EXCPS_MAX) {
        x = g_new0(McpTraceExcp, 1);
        x->number = excp->number;
        x->handler = excp->to;
        g_hash_table_insert(t->excps, &x->number, x);
    }
    if (x) {
        x->count++;
    } else {
        t->n_other_excps++;
    }
}

/* Count an access to a device's register by the register */
static void mcp_trace_count_reg(McpTracer *t, const McpDevHit *h)
{
    g_autofree char *key = g_strdup_printf("%s %016" PRIx64 " %d",
                                           h->nothing ? "" : h->region,
                                           h->nothing ? h->addr : h->offset,
                                           h->write);
    McpTraceReg *g = g_hash_table_lookup(t->dregs, key);

    if (!g && g_hash_table_size(t->dregs) < TRACE_INSNS_MAX) {
        g = g_new0(McpTraceReg, 1);
        g->key = g_steal_pointer(&key);
        g->area = h->area;
        g->addr = h->addr;
        memcpy(g->region, h->region, sizeof(g->region));
        g->offset = h->offset;
        g->nothing = h->nothing;
        g->write = h->write;
        g_hash_table_insert(t->dregs, g->key, g);
    }
    if (g) {
        g->count++;
        g->value = h->value;
    } else {
        t->n_other_dregs++;
    }
}

/* Count a hit in @table by @pc: whether it is the first there */
static bool mcp_trace_count_pc(McpTracer *t, GHashTable *table,
                               int64_t *other, CPUState *cpu, uint64_t pc,
                               bool show)
{
    McpTraceCount *c = g_hash_table_lookup(table, &pc);
    bool first = false;

    if (!c && g_hash_table_size(table) < TRACE_INSNS_MAX) {
        c = g_new0(McpTraceCount, 1);
        c->pc = pc;
        g_hash_table_insert(table, &c->pc, c);
        first = true;
    }
    /*
     * A CPU often gets to code before it is mapped, then comes back
     * after the TLB miss: show the instruction once it can be read
     */
    if (c && show && !c->mapped) {
        bool mapped = mcp_debug_mapped(cpu, pc);

        if (mapped || !c->insn) {
            g_free(c->insn);
            c->insn = mcp_debug_insn(cpu, pc);
            if (c->insn) {
                g_strchomp(c->insn);
            }
            c->mapped = mapped;
        }
    }
    if (c) {
        c->count++;
    } else {
        (*other)++;
    }
    return first;
}

static void mcp_trace_log1(McpRun *r, CPUState *cpu, const McpWatchSeen *seen,
                           const McpExcpSeen *excp, bool retry)
{
    McpTracer *t = container_of(r, McpTracer, r);
    McpBreak *b = &r->b;
    McpWatchArea *a = seen ? &b->watches[seen->area] : NULL;
    bool device = seen && seen->device;
    /* a CPU's access to registers that no watchpoint caught */
    bool block = seen && !device && !seen->by_insn;
    const McpDevHit *reg = seen && seen->has_dev && seen->dev.reg ?
                           &seen->dev : NULL;
    uint64_t pc = seen ? seen->pc : excp ? excp->from :
                  mcp_debug_reached(b, cpu);
    g_autofree char *at = NULL;
    McpTraceEntry *e;
    strList **vtail, *l;
    bool first = false;
    int i;

    if (excp) {
        mcp_trace_count_excp(t, excp);
    }
    if (reg) {
        mcp_trace_count_reg(t, reg);
    }
    if (device) {
        /* no instruction: counted apart, the first one listed */
        first = !t->n_dev++;
    } else if (block) {
        first = mcp_trace_count_pc(t, t->blocks, &t->n_other_blocks, cpu, pc,
                                   false);
    } else if (seen || excp || mcp_debug_by_addr(b)) {
        /* which instructions touch the watched memory, trap, or are got to */
        first = mcp_trace_count_pc(t, t->insns, &t->n_other, cpu, pc, true);
    }
    if (t->n_listed >= t->list && !first) {
        return;
    }
    t->n_listed++;

    e = g_new0(McpTraceEntry, 1);
    e->hit = r->n_hits;
    e->cpu = cpu->cpu_index;
    e->pc = g_strdup_printf("0x%" PRIx64, pc);
    at = mcp_addr_str(pc);
    if (seen) {
        e->watch = mcp_watch_hit(b, seen);
        /* the value accessed; else the watched word now, in RAM or ROM */
        if (reg) {
            e->value = g_strdup_printf("0x%" PRIx64, reg->value);
        } else if (!a->as &&
                   mcp_mem_plain(cpu, seen->addr & ~3ULL, 4, a->physical)) {
            e->value = mcp_read_words(cpu, seen->addr & ~3ULL, 1,
                                      a->physical);
        }
    }
    if (device) {
        g_string_append_printf(t->text, "#%" PRId64 " ", e->hit);
        mcp_dev_append(t->text, b, &seen->dev);
        if (e->value && !reg) {
            g_string_append_printf(t->text, " (now %s)", e->value);
        }
        g_string_append_printf(t->text, "; CPU %d at %s", cpu->cpu_index,
                               at);
    } else {
        g_string_append_printf(t->text, "#%" PRId64 " CPU %d at %s",
                               e->hit, cpu->cpu_index, at);
    }
    if (seen && !device) {
        g_autofree char *addr = mcp_area_str(a, seen->addr);

        g_string_append_c(t->text, ' ');
        if (reg) {
            mcp_dev_append_value(t->text, b, reg);
            mcp_dev_append_reg(t->text, reg);
        } else {
            g_string_append_printf(t->text, "%s %s",
                                   seen->write ? "wrote to" : "read", addr);
        }
        if (e->value && !reg) {
            g_string_append_printf(t->text, " (now %s)", e->value);
        }
        if (block) {
            g_string_append(t->text, " (in the block of code that ended"
                            " here)");
        }
    }
    if (excp) {
        g_autofree char *handler = mcp_addr_str(excp->to);

        e->exception = g_new0(McpExceptionHit, 1);
        e->exception->number = excp->number;
        e->exception->pc = g_strdup(e->pc);
        e->exception->handler = g_strdup_printf("0x%" PRIx64, excp->to);
        g_string_append_printf(t->text, " took exception %" PRId32 ", went"
                               " to %s", excp->number, handler);
    }
    if (retry) {
        e->has_retry = true;
        e->retry = true;
        g_string_append(t->text, " (again, after an interruption)");
    }
    vtail = &e->values;
    for (l = t->regs; l; l = l->next) {
        uint64_t v;

        QAPI_LIST_APPEND(vtail, mcp_reg_value(cpu, l->value, &v)
                         ? g_strdup_printf("%s=0x%" PRIx64, l->value, v)
                         : g_strdup_printf("%s=?", l->value));
    }
    for (i = 0; i < t->n_mem; i++) {
        QAPI_LIST_APPEND(vtail, mcp_trace_memory(cpu, &t->mem[i]));
    }
    e->has_values = e->values;
    for (l = e->values; l; l = l->next) {
        g_string_append_printf(t->text, "%s %s", l == e->values ? ":" : "",
                               l->value);
    }
    g_string_append_c(t->text, '\n');
    QAPI_LIST_APPEND(t->tail, e);
}

/* Log a hit; what it reads of the guest's memory is not a hit itself */
static void mcp_trace_log(McpRun *r, CPUState *cpu, const McpWatchSeen *seen,
                          const McpExcpSeen *excp, bool retry)
{
    mcp_dev_self++;
    mcp_trace_log1(r, cpu, seen, excp, retry);
    mcp_dev_self--;
}

/* "arrivals at 0x1000 and writes to 0x2000 (4 bytes)", "exceptions" */
static void mcp_trace_what(GString *s, McpBreak *b)
{
    static const char *const what[] = {
        [BP_MEM_WRITE] = "writes to",
        [BP_MEM_READ] = "reads of",
        [BP_MEM_ACCESS] = "accesses to",
    };
    const char *sep = "";

    if (b->n_addrs) {
        g_string_append(s, "arrivals at ");
        mcp_debug_append_addrs(s, b, "and");
        sep = " and ";
    }
    if (b->n_watches) {
        g_string_append(s, sep);
        mcp_debug_append_watches(s, b, what, true, "and");
        sep = " and ";
    }
    if (b->has_excp) {
        g_string_append(s, sep);
        mcp_debug_append_excp(s, b, "exception", "exceptions", "exceptions",
                              "and");
        sep = " and ";
    }
    if (b->n_watches) {
        g_autoptr(GString) devs = g_string_new(NULL);

        if (mcp_debug_append_dev(devs, b, what, "and")) {
            g_string_append_printf(s, "%sdevices' %s", *sep ? ", and " : "",
                                   devs->str);
        }
    }
}

McpTrace *coroutine_fn qmp_debug_trace(McpAddresses *address,
                                       const char *address_mask,
                                       McpWatches *watch,
                                       McpExceptions *exception,
                                       const char *condition,
                                       bool has_count, int64_t count,
                                       bool has_list, int64_t list,
                                       bool has_cpu, int64_t cpu_index,
                                       bool has_registers,
                                       strList *registers,
                                       bool has_memory,
                                       McpDebugMemoryList *memory,
                                       bool has_duration_ms,
                                       int64_t duration_ms, Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GString) text = g_string_new(NULL);
    g_autoptr(GHashTable) insns = g_hash_table_new_full(g_int64_hash,
                                                        g_int64_equal, NULL,
                                                        mcp_trace_count_free);
    g_autoptr(GHashTable) blocks = g_hash_table_new_full(g_int64_hash,
                                                         g_int64_equal, NULL,
                                                         mcp_trace_count_free);
    g_autoptr(GHashTable) dregs = g_hash_table_new_full(g_str_hash,
                                                        g_str_equal, NULL,
                                                        mcp_trace_reg_free);
    g_autoptr(GHashTable) excps = g_hash_table_new_full(g_int_hash,
                                                        g_int_equal, NULL,
                                                        g_free);
    g_autoptr(GPtrArray) sorted = NULL;
    g_autoptr(McpCond) cond = NULL;
    g_autofree char *cond_str = NULL;
    g_autofree McpMem *mem = NULL;
    McpTracer t = {
        .r.ctx = mcp_current_input_ctx(),
        .r.log = mcp_trace_log,
        .text = text,
        .insns = insns,
        .blocks = blocks,
        .dregs = dregs,
        .excps = excps,
    };
    McpBreak *b = &t.r.b;
    McpTraceInstructionList **itail;
    McpTraceRegisterList **rtail;
    McpExceptionCountList **xtail;
    McpDebugMemoryList *ml;
    McpTrace *res;
    CPUState *cpu;
    strList *rl;
    int64_t start, ms;
    unsigned int i;
    int n;

    if (!t.r.ctx) {
        qemu_input_ctx_init(&local, NULL);
        t.r.ctx = &local;
    }
    if (!address && !watch && !exception) {
        error_setg(errp, "give an address to trace, memory to watch,"
                   " exceptions, or more of them");
        return NULL;
    }
    count = has_count ? count : TRACE_COUNT;
    if (count < 1 || count > TRACE_COUNT_MAX) {
        error_setg(errp, "count must be 1 to %d", TRACE_COUNT_MAX);
        return NULL;
    }
    list = has_list ? list : TRACE_LIST;
    if (list < 0 || list > TRACE_LIST_MAX) {
        error_setg(errp, "list must be 0 to %d", TRACE_LIST_MAX);
        return NULL;
    }
    duration_ms = has_duration_ms ? duration_ms : TRACE_DEFAULT_MS;
    duration_ms = MIN(MAX(duration_ms, 0), mcp_wait_max_ms());
    if (!mcp_run_parse(&t.r, address, address_mask, exception, has_cpu,
                       cpu_index, errp)) {
        return NULL;
    }
    cpu = b->only ? b->only : first_cpu;
    if (!cpu) {
        error_setg(errp, "this VM has no CPU");
        return NULL;
    }
    if (condition) {
        cond = mcp_cond_parse(cpu, condition, errp);
        if (!cond) {
            return NULL;
        }
        b->cond = cond;
    }
    for (rl = registers, n = 0; rl; rl = rl->next) {
        if (++n > TRACE_REGS_MAX) {
            error_setg(errp, "ask for at most %d registers", TRACE_REGS_MAX);
            return NULL;
        }
        if (g_ascii_strcasecmp(rl->value, "pc") &&
            mcp_reg_number(cpu, rl->value) < 0) {
            g_autofree char *names = mcp_reg_names(cpu);

            error_setg(errp, "'%s' is not a register; the registers are %s",
                       rl->value, names);
            return NULL;
        }
    }
    mem = mcp_parse_memory(cpu, memory, &t.n_mem, errp);
    if (!mem) {
        return NULL;
    }
    /* one word each, unless asked */
    for (ml = memory, n = 0; ml; ml = ml->next, n++) {
        if (!ml->value->has_words) {
            mem[n].words = 1;
        }
    }
    /* each line shows the watched word */
    if (watch && !mcp_parse_watches(watch, b, NULL, errp)) {
        return NULL;
    }
    t.mem = mem;
    t.regs = registers;
    t.list = list;
    t.tail = &t.entries;
    t.r.limit = count;
    if (!mcp_debug_begin(t.r.ctx, errp)) {
        return NULL;
    }
    start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    t.r.end = start + duration_ms;

    mcp_run(&t.r);
    ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start;
    /* the guest goes on, unless something else stopped it */
    if (!runstate_is_running() &&
        (t.r.result != RUN_STOPPED || t.r.late)) {
        mcp_debug_outside(mcp_debug_start, NULL);
    }
    mcp_debug_end();
    if (t.r.result == RUN_CANCELLED) {
        qapi_free_McpTraceEntryList(t.entries);
        error_setg(errp, "cancelled");
        return NULL;
    }

    res = g_new0(McpTrace, 1);
    res->hits = t.r.n_hits;
    res->has_skipped = cond != NULL;
    res->skipped = mcp_cond_skipped(cond);
    res->ms = ms;
    res->running = runstate_is_running();
    res->entries = t.entries;
    for (n = 0; n < b->n_watches; n++) {
        res->has_device_writes |= b->watches[n].devices;
    }
    res->device_writes = t.n_dev;
    g_string_assign(s, "Traced ");
    mcp_trace_what(s, b);
    g_string_append_printf(s, " for %.1f s: ", ms / 1000.0);
    if (t.r.n_hits) {
        g_string_append_printf(s, "%" PRId64 " hit%s", t.r.n_hits,
                               t.r.n_hits == 1 ? "" : "s");
    } else {
        g_string_append(s, "no hits");
    }
    if (t.n_dev) {
        g_string_append_printf(s, " (%" PRId64 " by devices)", t.n_dev);
    }
    if (t.r.result == RUN_REACHED) {
        g_string_append(s, ", as many as asked for");
    }
    mcp_run_append_more(s, &t.r);
    if (t.r.n_retries) {
        g_string_append_printf(s, " (%" PRId64 " right after an interruption"
                               " there)", t.r.n_retries);
    }
    cond_str = mcp_cond_str(cond);
    if (cond_str) {
        g_string_append_printf(s, "; %s", cond_str);
    }
    if (res->running) {
        g_string_append(s, "; the guest runs on.");
    } else {
        g_string_append_printf(s, "; then the VM stopped (%s), and it stays"
                               " stopped.", RunState_str(runstate_get()));
    }

    if (b->has_excp) {
        GHashTableIter it;
        gpointer v;

        sorted = g_ptr_array_new();
        g_hash_table_iter_init(&it, excps);
        while (g_hash_table_iter_next(&it, NULL, &v)) {
            g_ptr_array_add(sorted, v);
        }
        g_ptr_array_sort(sorted, mcp_trace_excp_cmp);
        res->has_exceptions = true;
        xtail = &res->exceptions;
        if (sorted->len) {
            g_string_append(s, "\n\nBy exception:\n");
        }
        for (i = 0; i < sorted->len; i++) {
            McpTraceExcp *x = g_ptr_array_index(sorted, i);
            McpExceptionCount *xc = g_new0(McpExceptionCount, 1);
            g_autofree char *handler = mcp_addr_str(x->handler);

            xc->number = x->number;
            xc->count = x->count;
            xc->handler = g_strdup_printf("0x%" PRIx64, x->handler);
            QAPI_LIST_APPEND(xtail, xc);
            g_string_append_printf(s, "%8" PRId64 " x exception %" PRId32
                                   ", handler %s\n", x->count, x->number,
                                   handler);
        }
        if (t.n_other_excps) {
            g_string_append_printf(s, "%8" PRId64 " x other exceptions"
                                   " (only %d are told apart)\n",
                                   t.n_other_excps, TRACE_EXCPS_MAX);
        }
        g_clear_pointer(&sorted, g_ptr_array_unref);
    }
    if (b->n_watches || b->has_excp || mcp_debug_by_addr(b)) {
        GHashTableIter it;
        gpointer v;

        sorted = g_ptr_array_new();
        g_hash_table_iter_init(&it, insns);
        while (g_hash_table_iter_next(&it, NULL, &v)) {
            g_ptr_array_add(sorted, v);
        }
        g_ptr_array_sort(sorted, mcp_trace_count_cmp);
        res->has_instructions = true;
        itail = &res->instructions;
        if (sorted->len || t.n_dev || g_hash_table_size(blocks)) {
            g_string_append(s, s->str[s->len - 1] == '\n' ? "\n" : "\n\n");
            g_string_append(s, "By instruction:\n");
        }
        for (i = 0; i < sorted->len; i++) {
            McpTraceCount *c = g_ptr_array_index(sorted, i);
            McpTraceInstruction *in = g_new0(McpTraceInstruction, 1);

            in->pc = g_strdup_printf("0x%" PRIx64, c->pc);
            in->count = c->count;
            in->instruction = g_strdup(c->insn);
            QAPI_LIST_APPEND(itail, in);
            g_string_append_printf(s, "%8" PRId64 " x %s\n", c->count,
                                   c->insn ? c->insn : in->pc);
        }
        if (t.n_other) {
            g_string_append_printf(s, "%8" PRId64 " x other instructions"
                                   " (only %d are told apart)\n", t.n_other,
                                   TRACE_INSNS_MAX);
        }
        /* CPUs' accesses that no watchpoint caught, by where they stopped */
        g_clear_pointer(&sorted, g_ptr_array_unref);
        sorted = g_ptr_array_new();
        g_hash_table_iter_init(&it, blocks);
        while (g_hash_table_iter_next(&it, NULL, &v)) {
            g_ptr_array_add(sorted, v);
        }
        g_ptr_array_sort(sorted, mcp_trace_count_cmp);
        for (i = 0; i < sorted->len; i++) {
            McpTraceCount *c = g_ptr_array_index(sorted, i);
            McpTraceInstruction *in = g_new0(McpTraceInstruction, 1);
            g_autofree char *at = mcp_addr_str(c->pc);

            in->pc = g_strdup_printf("0x%" PRIx64, c->pc);
            in->count = c->count;
            in->has_block_end = true;
            in->block_end = true;
            QAPI_LIST_APPEND(itail, in);
            g_string_append_printf(s, "%8" PRId64 " x in the block of code"
                                   " that ended before %s\n", c->count, at);
        }
        if (t.n_other_blocks) {
            g_string_append_printf(s, "%8" PRId64 " x in other blocks of"
                                   " code (only %d are told apart)\n",
                                   t.n_other_blocks, TRACE_INSNS_MAX);
        }
        if (t.n_dev) {
            g_string_append_printf(s, "%8" PRId64 " x no instruction: a"
                                   " device did it\n", t.n_dev);
        }
        g_clear_pointer(&sorted, g_ptr_array_unref);
    }
    if (g_hash_table_size(dregs) || t.n_other_dregs) {
        GHashTableIter it;
        gpointer v;

        sorted = g_ptr_array_new();
        g_hash_table_iter_init(&it, dregs);
        while (g_hash_table_iter_next(&it, NULL, &v)) {
            g_ptr_array_add(sorted, v);
        }
        g_ptr_array_sort(sorted, mcp_trace_reg_cmp);
        rtail = &res->device_registers;
        g_string_append(s, s->str[s->len - 1] == '\n' ? "\n" : "\n\n");
        g_string_append(s, "By device register:\n");
        for (i = 0; i < sorted->len; i++) {
            McpTraceReg *g = g_ptr_array_index(sorted, i);
            McpTraceRegister *tr = g_new0(McpTraceRegister, 1);
            g_autofree char *at = mcp_area_str(&b->watches[g->area], g->addr);

            tr->address = g_strdup_printf("0x%" PRIx64, g->addr);
            if (!g->nothing) {
                tr->region = g_strdup(g->region);
                tr->offset = g_strdup_printf("0x%" PRIx64, g->offset);
            }
            tr->access = g_strdup(g->write ? "write" : "read");
            tr->count = g->count;
            tr->value = g_strdup_printf("0x%" PRIx64, g->value);
            QAPI_LIST_APPEND(rtail, tr);
            if (g->nothing) {
                g_string_append_printf(s, "%8" PRId64 " x %s %s, where"
                                       " nothing is (last 0x%" PRIx64 ")\n",
                                       g->count, g->write ? "write to" :
                                       "read of", at, g->value);
            } else {
                g_string_append_printf(s, "%8" PRId64 " x %s 0x%" PRIx64
                                       " in %s, %s (last 0x%" PRIx64 ")\n",
                                       g->count, g->write ? "write to" :
                                       "read of", g->offset, g->region, at,
                                       g->value);
            }
        }
        if (t.n_other_dregs) {
            g_string_append_printf(s, "%8" PRId64 " x other registers (only"
                                   " %d are told apart)\n", t.n_other_dregs,
                                   TRACE_INSNS_MAX);
        }
        g_clear_pointer(&sorted, g_ptr_array_unref);
    }
    if (t.n_listed) {
        g_string_append(s, s->str[s->len - 1] == '\n' ? "\n" : "\n\n");
        if (t.n_listed == t.r.n_hits) {
            g_string_append(s, "Hits:\n");
        } else if (b->n_watches || b->has_excp || mcp_debug_by_addr(b)) {
            /* "the first access by each instruction" */
            bool insn = b->n_watches || b->has_excp;
            const char *what = insn && mcp_debug_by_addr(b) ? "hit at" :
                               b->n_watches && b->has_excp ? "hit by" :
                               b->n_watches ? "access by" :
                               b->has_excp ? "exception taken by" :
                               "arrival at";
            const char *where = insn ? "instruction" : "address";

            if (list) {
                g_string_append_printf(s, "Hits (the first %" PRId64 ", then"
                                       " the first %s each new %s):\n",
                                       list, what, where);
            } else {
                g_string_append_printf(s, "Hits (the first %s each %s):\n",
                                       what, where);
            }
        } else {
            g_string_append_printf(s, "Hits (the first %" PRId64 "):\n",
                                   list);
        }
        g_string_append(s, text->str);
    }
    mcp_dev_append_skipped(s, b);
    res->summary = g_strdup(g_strchomp(s->str));
    return res;
}

McpDebugStop *coroutine_fn qmp_debug_step(bool has_count, int64_t count,
                                          bool has_cpu, int64_t cpu_index,
                                          bool has_memory,
                                          McpDebugMemoryList *memory,
                                          Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_current_input_ctx();
    g_autoptr(GString) s = g_string_new(NULL);
    g_autofree McpMem *mem = NULL;
    g_autofree char **done_insn = NULL;
    RunState paused = RUN_STATE_PAUSED;
    McpStepResult step = STEP_DONE;
    McpDebugStop *res;
    const char *why = NULL;
    CPUState *cpu;
    int64_t done, first, i;
    int n_mem = 0;
    bool interrupted;

    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }
    count = has_count ? count : 1;
    if (count < 1 || count > STEP_MAX) {
        error_setg(errp, "count must be 1 to %d", STEP_MAX);
        return NULL;
    }
    cpu = mcp_debug_cpu(has_cpu, cpu_index, errp);
    if (!cpu) {
        return NULL;
    }
    mem = mcp_parse_memory(cpu, memory, &n_mem, errp);
    if (!mem || !mcp_debug_begin(ctx, errp)) {
        return NULL;
    }
    if (runstate_is_running()) {
        mcp_debug_outside(mcp_debug_stop, &paused);
    }

    done_insn = g_new0(char *, STEP_SHOWN);
    for (done = 0; done < count && !ctx->cancelled; done++) {
        char **slot = &done_insn[done % STEP_SHOWN];
        char *insn;

        if (runstate_is_running()) {
            why = "something else started the VM";
            break;
        }
        if (cpu->halted) {
            why = "it is halted, waiting for an interrupt, and a step keeps "
                  "interrupts away";
            break;
        }
        insn = mcp_debug_insn(cpu, cpu->cc->get_pc(cpu));
        step = mcp_debug_step_one(ctx, cpu, &interrupted);
        if (step == STEP_NOT_STARTED || step == STEP_CANCELLED) {
            g_free(insn);
            why = step == STEP_CANCELLED ? "cancelled" :
                  "the VM did not start";
            break;
        }
        if (interrupted && insn) {
            g_autofree char *to = mcp_addr_str(cpu->cc->get_pc(cpu));
            char *old = insn;

            insn = g_strdup_printf("%s  -> it took an interruption: the CPU"
                                   " went to %s\n", old, to);
            g_free(old);
        }
        g_free(*slot);
        *slot = insn;
        if (step == STEP_HALTED) {
            why = "it halted, waiting for an interrupt";
            done++;
            break;
        }
        if (step == STEP_STOPPED) {
            why = "the VM stopped";
            done++;
            break;
        }
    }
    mcp_debug_end();
    if (ctx->cancelled) {
        for (i = 0; i < STEP_SHOWN; i++) {
            g_free(done_insn[i]);
        }
        error_setg(errp, "cancelled");
        return NULL;
    }

    res = g_new0(McpDebugStop, 1);
    res->reached = done == count;
    res->has_steps = true;
    res->steps = done;
    res->running = runstate_is_running();
    mcp_debug_set_cpu(res, cpu);
    g_string_printf(s, "CPU %d did %" PRId64, cpu->cpu_index, done);
    if (done < count) {
        g_string_append_printf(s, " of %" PRId64, count);
    }
    g_string_append_printf(s, " step%s", count == 1 ? "" : "s");
    if (why) {
        g_string_append_printf(s, ": %s", why);
        if (step == STEP_STOPPED) {
            g_string_append_printf(s, " (%s)", RunState_str(runstate_get()));
        }
    }
    g_string_append(s, res->running ? "; the VM is running" :
                    "; the VM is stopped");

    first = MAX(done - STEP_SHOWN, 0);
    if (done && done_insn[first % STEP_SHOWN]) {
        g_string_append_printf(s, "\n\n%s:\n", done > STEP_SHOWN ?
                               "The last instructions done" :
                               done == 1 ? "Instruction done" :
                               "Instructions done");
        for (i = first; i < done; i++) {
            g_string_append(s, done_insn[i % STEP_SHOWN]);
        }
    }
    for (i = 0; i < STEP_SHOWN; i++) {
        g_free(done_insn[i]);
    }
    if (!res->running) {
        mcp_debug_show(s, cpu, mem, n_mem);
    }
    res->summary = g_strdup(s->str);
    return res;
}

#define PC_NO_PHYS      UINT64_MAX

typedef struct McpPcCount {
    uint64_t pc;
    uint64_t pa;                /* where it was then, or PC_NO_PHYS */
    int64_t count;
} McpPcCount;

static guint mcp_pc_hash(gconstpointer key)
{
    const McpPcCount *c = key;

    return g_int64_hash(&c->pc) * 31 + g_int64_hash(&c->pa);
}

static gboolean mcp_pc_equal(gconstpointer a, gconstpointer b)
{
    const McpPcCount *x = a, *y = b;

    return x->pc == y->pc && x->pa == y->pa;
}

/*
 * The samples, shared by the tool and its queued work: the last of
 * them to finish frees it.
 */
typedef struct McpProfileData {
    int refs;
    GHashTable *pcs;            /* set of McpPcCount, by pc and pa */
    int64_t ran, halted;
    bool *pending;              /* by CPU index: a sample is queued */
} McpProfileData;

static void mcp_profile_unref(McpProfileData *p)
{
    if (!--p->refs) {
        g_hash_table_destroy(p->pcs);
        g_free(p->pending);
        g_free(p);
    }
}

/* In the vCPU thread with the BQL, between two blocks of code */
static void mcp_profile_sample(CPUState *cpu, run_on_cpu_data data)
{
    McpProfileData *p = data.host_ptr;

    p->pending[cpu->cpu_index] = false;
    if (!runstate_is_running()) {
        /* paused since the sample was asked for */
    } else if (cpu->halted) {
        p->halted++;
    } else {
        McpPcCount key = { .pc = cpu->cc->get_pc(cpu), .pa = PC_NO_PHYS };
        TranslateForDebugResult t;
        McpPcCount *c;

        /* now, while the CPU runs there: the mapping may be gone later */
        if (cpu_translate_for_debug(cpu, key.pc, &t)) {
            key.pa = t.physaddr;
        }
        c = g_hash_table_lookup(p->pcs, &key);
        if (!c) {
            c = g_memdup2(&key, sizeof(key));
            g_hash_table_add(p->pcs, c);
        }
        c->count++;
        p->ran++;
    }
    mcp_profile_unref(p);
}

static gint mcp_pc_count_cmp(gconstpointer a, gconstpointer b)
{
    const McpPcCount *x = *(McpPcCount * const *)a;
    const McpPcCount *y = *(McpPcCount * const *)b;

    if (x->count != y->count) {
        return x->count < y->count ? 1 : -1;
    }
    if (x->pc != y->pc) {
        return x->pc < y->pc ? -1 : 1;
    }
    return x->pa < y->pa ? -1 : x->pa > y->pa;
}

McpProfile *coroutine_fn qmp_debug_profile(bool has_interval_ms,
                                           int64_t interval_ms,
                                           bool has_cpu, int64_t cpu_index,
                                           bool has_top, int64_t top,
                                           Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_current_input_ctx();
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GPtrArray) all = NULL;
    McpProfileData *p;
    McpProfile *res;
    McpProfileEntryList **tail;
    GHashTableIter iter;
    gpointer value;
    CPUState *only = NULL, *cpu;
    int64_t start, now, end, ms, ticks = 0, paused = 0;
    int ncpus = 0;
    guint i;

    if (!tcg_enabled()) {
        error_setg(errp, "debug-profile needs TCG (-accel tcg)");
        return NULL;
    }
    if (has_cpu) {
        only = mcp_debug_cpu(true, cpu_index, errp);
        if (!only) {
            return NULL;
        }
    }
    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }
    interval_ms = has_interval_ms ? interval_ms : PROFILE_DEFAULT_MS;
    interval_ms = MAX(MIN(interval_ms, mcp_wait_max_ms()), PROFILE_MIN_MS);
    top = has_top ? top : PROFILE_TOP;
    top = MIN(MAX(top, 1), PROFILE_TOP_MAX);
    CPU_FOREACH(cpu) {
        ncpus = MAX(ncpus, cpu->cpu_index + 1);
    }

    p = g_new0(McpProfileData, 1);
    p->refs = 1;
    p->pcs = g_hash_table_new_full(mcp_pc_hash, mcp_pc_equal, g_free, NULL);
    p->pending = g_new0(bool, MAX(ncpus, 1));
    start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    end = start + interval_ms;
    for (;;) {
        ticks++;
        if (!runstate_is_running()) {
            paused++;
        } else {
            CPU_FOREACH(cpu) {
                if ((only && cpu != only) || cpu->cpu_index >= ncpus ||
                    p->pending[cpu->cpu_index]) {
                    continue;
                }
                p->pending[cpu->cpu_index] = true;
                p->refs++;
                async_run_on_cpu(cpu, mcp_profile_sample,
                                 RUN_ON_CPU_HOST_PTR(p));
            }
        }
        now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (now >= end || ctx->cancelled) {
            break;
        }
        /* uneven steps, so that a guest timer can't keep pace with them */
        qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                  MIN(g_random_int_range(2, 9), end - now)
                                  * SCALE_MS);
    }
    if (ctx->cancelled) {
        mcp_profile_unref(p);
        error_setg(errp, "cancelled");
        return NULL;
    }
    ms = MAX(now - start, 1);

    all = g_ptr_array_new();
    g_hash_table_iter_init(&iter, p->pcs);
    while (g_hash_table_iter_next(&iter, &value, NULL)) {
        g_ptr_array_add(all, value);
    }
    g_ptr_array_sort(all, mcp_pc_count_cmp);

    res = g_new0(McpProfile, 1);
    res->interval_ms = ms;
    res->samples = p->ran + p->halted;
    res->halted_percent = res->samples ? p->halted * 100 / res->samples : 0;
    res->paused_percent = paused * 100 / ticks;
    tail = &res->top;
    cpu = only ? only : first_cpu;
    for (i = 0; i < all->len && i < top; i++) {
        McpPcCount *c = g_ptr_array_index(all, i);
        McpProfileEntry *e = g_new0(McpProfileEntry, 1);
        bool phys = c->pa != PC_NO_PHYS;
        g_autofree char *insn = cpu ? mcp_cpu_disas(cpu, phys ? c->pa : c->pc,
                                                    1, phys) : NULL;

        e->pc = g_strdup_printf("0x%" PRIx64, c->pc);
        e->symbol = mcp_sym_name(c->pc);
        if (phys) {
            e->physical = g_strdup_printf("0x%" PRIx64, c->pa);
        }
        e->count = c->count;
        e->percent = (c->count * 100 + p->ran / 2) / p->ran;
        if (insn) {
            /* without the address in front, which is in @pc */
            char *text = strstr(insn, ":  ");
            char *in, *out;

            text = g_strstrip(text ? text + 3 : insn);
            for (in = out = text; *in; in++) {
                char ch = *in == '\t' ? ' ' : *in;

                if (ch != ' ' || (out > text && out[-1] != ' ')) {
                    *out++ = ch;
                }
            }
            *out = '\0';
            e->instruction = g_strdup(text);
        }
        QAPI_LIST_APPEND(tail, e);
    }

    g_string_printf(s, "%.1f s, %" PRId64 " samples; halted %" PRId64 "%%",
                    ms / 1000.0, res->samples, res->halted_percent);
    if (res->paused_percent) {
        g_string_append_printf(s, ", paused %" PRId64 "%%",
                               res->paused_percent);
    }
    if (res->top) {
        McpProfileEntry *e = res->top->value;

        g_string_append_printf(s, "; most at %s%s%s%s (%.1f%%)", e->pc,
                               e->symbol ? " <" : "",
                               e->symbol ? e->symbol : "",
                               e->symbol ? ">" : "",
                               e->count * 100.0 / p->ran);
    } else {
        g_string_append(s, "; no CPU ran");
    }
    res->summary = g_strdup(s->str);
    mcp_profile_unref(p);
    return res;
}

/*
 * debug-coverage.  While it records, each CPU notes every block of
 * translated code it enters, in a table of its own (tb-cover.h); that
 * keeps its blocks from chaining.  The tables are swapped with the
 * vCPUs paused.  A block is known by where its code is in RAM, so one
 * address in two processes is two blocks.
 */

#define COVER_QUIET_MS          2000
#define COVER_QUIET_MAX_MS      60000
#define COVER_LIMIT_MS          (10 * 60 * 1000)
#define COVER_LIST              20
#define COVER_LIST_MAX          500
#define COVER_GAP               64      /* most bytes between blocks of a
                                           range */

typedef struct McpCoverBlock {
    uint64_t key;               /* as in CPUTBCoverEntry; first, for the
                                   hash table */
    uint64_t pc;
    uint32_t size;
    uint64_t runs;
} McpCoverBlock;

static struct {
    bool on;                    /* the CPUs note the blocks they run */
    bool busy;                  /* a start is in its quiet part */
    bool timed_out;             /* the time limit stopped the recording */
    int64_t since;              /* when the recording began, realtime ms */
    int64_t ms;                 /* how long it recorded, once stopped */
    uint64_t lost;              /* runs the tables had no room for */
    GHashTable *quiet;          /* McpCoverBlock by key: the quiet part */
    GHashTable *rec;            /* the same: the recording, once stopped */
    GHashTable *list;           /* the same: what the last stop kept */
    QEMUTimer *limit;
} mcp_cover;

static GHashTable *mcp_cover_blocks(void)
{
    return g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);
}

/*
 * Take the CPUs' tables, and give them new ones if @again.  The vCPUs
 * pause for it if the VM runs.
 */
static GPtrArray *mcp_cover_swap(bool again)
{
    GPtrArray *tables = g_ptr_array_new();
    bool running = runstate_is_running();
    CPUState *cpu;

    if (running) {
        pause_all_vcpus();
    }
    CPU_FOREACH(cpu) {
        if (cpu->tb_cover) {
            g_ptr_array_add(tables, cpu->tb_cover);
        }
        cpu->tb_cover = again ? cpu_tb_cover_new() : NULL;
        tcg_update_cflags(cpu);
    }
    if (running) {
        resume_all_vcpus();
    }
    return tables;
}

/* The blocks in @tables, which it frees, by key */
static GHashTable *mcp_cover_merge(GPtrArray *tables)
{
    GHashTable *t = mcp_cover_blocks();
    guint i;

    for (i = 0; i < tables->len; i++) {
        CPUTBCover *c = g_ptr_array_index(tables, i);
        uint32_t j;

        for (j = 0; j <= c->mask; j++) {
            CPUTBCoverEntry *e = &c->e[j];
            McpCoverBlock *b;

            if (!e->runs) {
                continue;
            }
            b = g_hash_table_lookup(t, &e->key);
            if (!b) {
                b = g_new0(McpCoverBlock, 1);
                b->key = e->key;
                b->pc = e->pc;
                g_hash_table_insert(t, &b->key, b);
            }
            b->size = MAX(b->size, e->size);
            b->runs += e->runs;
        }
        mcp_cover.lost += c->lost;
        cpu_tb_cover_free(c);
    }
    g_ptr_array_unref(tables);
    return t;
}

static void mcp_cover_end_recording(void)
{
    mcp_cover.rec = mcp_cover_merge(mcp_cover_swap(false));
    mcp_cover.on = false;
    mcp_cover.ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - mcp_cover.since;
    timer_del(mcp_cover.limit);
}

/* A recording nobody stops ends by itself, so the guest runs fast again */
static void mcp_cover_limit(void *opaque)
{
    if (mcp_cover.on && !mcp_cover.busy) {
        mcp_cover_end_recording();
        mcp_cover.timed_out = true;
    }
}

typedef struct McpRamPhys {
    uint64_t ram;
    hwaddr pa;
    bool found;
} McpRamPhys;

static bool mcp_ram_phys_range(Int128 start, Int128 len,
                               const MemoryRegion *mr, hwaddr off,
                               void *opaque)
{
    McpRamPhys *r = opaque;
    ram_addr_t base = memory_region_get_ram_addr(mr);

    if (base == RAM_ADDR_INVALID) {
        return false;
    }
    base += off;
    if (r->ram >= base && r->ram - base < int128_get64(len)) {
        r->pa = int128_get64(start) + (r->ram - base);
        r->found = true;
        return true;
    }
    return false;
}

/* Where code at @ram in RAM or ROM shows in physical memory, the lowest */
static bool mcp_ram_phys(uint64_t ram, hwaddr *pa)
{
    McpRamPhys r = { .ram = ram };

    if (ram & TB_COVER_NOT_RAM) {
        return false;
    }
    WITH_RCU_READ_LOCK_GUARD() {
        flatview_for_each_range(address_space_to_flatview(&address_space_memory),
                                mcp_ram_phys_range, &r);
    }
    *pa = r.pa;
    return r.found;
}

static gint mcp_cover_by_key(gconstpointer a, gconstpointer b)
{
    const McpCoverBlock *x = *(McpCoverBlock * const *)a;
    const McpCoverBlock *y = *(McpCoverBlock * const *)b;

    return x->key < y->key ? -1 : x->key > y->key;
}

typedef struct McpCoverSpan {
    uint64_t key;               /* where it is, to sort the same pc */
    uint64_t pc, end;           /* the first and the last byte */
    int64_t blocks;
    uint64_t runs;
} McpCoverSpan;

static gint mcp_cover_span_cmp(gconstpointer a, gconstpointer b)
{
    const McpCoverSpan *x = a, *y = b;

    if (x->pc != y->pc) {
        return x->pc < y->pc ? -1 : 1;
    }
    return x->key < y->key ? -1 : x->key > y->key;
}

/*
 * The blocks of @t as ranges of code close together: in RAM, and at
 * addresses that stay as far apart, so the same code mapped twice or
 * two processes don't mix.  By address.
 */
static GArray *mcp_cover_spans(GHashTable *t)
{
    g_autoptr(GPtrArray) all = g_ptr_array_new();
    GArray *spans = g_array_new(false, false, sizeof(McpCoverSpan));
    McpCoverSpan *sp = NULL;
    const McpCoverBlock *prev = NULL;
    GHashTableIter iter;
    gpointer value;
    guint i;

    g_hash_table_iter_init(&iter, t);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        g_ptr_array_add(all, value);
    }
    g_ptr_array_sort(all, mcp_cover_by_key);
    for (i = 0; i < all->len; i++) {
        const McpCoverBlock *b = g_ptr_array_index(all, i);
        uint64_t last = b->pc + MAX(b->size, 1) - 1;

        if (!prev || b->key - b->pc != prev->key - prev->pc ||
            b->key > sp->key + (sp->end - sp->pc) + 1 + COVER_GAP) {
            McpCoverSpan n = { .key = b->key, .pc = b->pc, .end = last };

            g_array_append_val(spans, n);
            sp = &g_array_index(spans, McpCoverSpan, spans->len - 1);
        }
        sp->end = MAX(sp->end, last);
        sp->blocks++;
        sp->runs += b->runs;
        prev = b;
    }
    g_array_sort(spans, mcp_cover_span_cmp);
    return spans;
}

/* "250 ms", "2.5 s" */
static void mcp_cover_append_time(GString *s, int64_t ms)
{
    if (ms < 1000) {
        g_string_append_printf(s, "%" PRId64 " ms", ms);
    } else {
        g_string_append_printf(s, "%.1f s", ms / 1000.0);
    }
}

static McpCoverage *mcp_cover_start(bool has_quiet_ms, int64_t quiet_ms,
                                    Error **errp)
{
    g_auto(QemuInputCtx) local = { 0 };
    QemuInputCtx *ctx = mcp_current_input_ctx();
    g_autoptr(GString) s = g_string_new(NULL);
    McpCoverage *res;
    bool was_on = mcp_cover.on;
    int64_t start, now;

    quiet_ms = has_quiet_ms ? quiet_ms : COVER_QUIET_MS;
    if (quiet_ms < 0 || quiet_ms > COVER_QUIET_MAX_MS) {
        error_setg(errp, "quiet-ms must be 0 to %d", COVER_QUIET_MAX_MS);
        return NULL;
    }
    if (quiet_ms && !runstate_is_running()) {
        error_setg(errp, "the VM is paused, so nothing would run in the"
                   " quiet part: resume it (vm-power), or give quiet-ms 0");
        return NULL;
    }
    if (!ctx) {
        qemu_input_ctx_init(&local, NULL);
        ctx = &local;
    }
    if (!mcp_cover.limit) {
        mcp_cover.limit = timer_new_ms(QEMU_CLOCK_REALTIME, mcp_cover_limit,
                                       NULL);
    }

    /* what an earlier start recorded goes; the list stays */
    if (mcp_cover.on) {
        g_hash_table_unref(mcp_cover_merge(mcp_cover_swap(false)));
        mcp_cover.on = false;
        timer_del(mcp_cover.limit);
    }
    g_clear_pointer(&mcp_cover.rec, g_hash_table_unref);
    g_clear_pointer(&mcp_cover.quiet, g_hash_table_unref);
    mcp_cover.timed_out = false;
    mcp_cover.lost = 0;

    res = g_new0(McpCoverage, 1);
    start = now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (quiet_ms) {
        GPtrArray *tables;

        mcp_cover.busy = true;
        g_ptr_array_unref(mcp_cover_swap(true));
        while (now < start + quiet_ms && !ctx->cancelled) {
            qemu_co_sleep_ns_wakeable(&ctx->sleep, QEMU_CLOCK_REALTIME,
                                      (start + quiet_ms - now) * SCALE_MS);
            now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        }
        /* the quiet part's tables, and new ones for the recording */
        tables = mcp_cover_swap(!ctx->cancelled);
        mcp_cover.busy = false;
        mcp_cover.quiet = mcp_cover_merge(tables);
        if (ctx->cancelled) {
            g_clear_pointer(&mcp_cover.quiet, g_hash_table_unref);
            qapi_free_McpCoverage(res);
            error_setg(errp, "cancelled");
            return NULL;
        }
        res->has_milliseconds = true;
        res->milliseconds = now - start;
        res->has_ran = true;
        res->ran = g_hash_table_size(mcp_cover.quiet);
        g_string_assign(s, "In the quiet part (");
        mcp_cover_append_time(s, res->milliseconds);
        g_string_append_printf(s, "), %" PRId64 " block%s of code ran; stop"
                               " leaves %s out. ", res->ran,
                               res->ran == 1 ? "" : "s",
                               res->ran == 1 ? "it" : "them");
    } else {
        g_ptr_array_unref(mcp_cover_swap(true));
        g_string_assign(s, "No quiet part. ");
    }
    mcp_cover.on = true;
    mcp_cover.since = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    timer_mod(mcp_cover.limit, mcp_cover.since + COVER_LIMIT_MS);
    res->recording = true;
    g_string_append(s, "Recording now: do the thing, then call"
                    " debug-coverage with action stop. The guest runs slower"
                    " until then.");
    if (was_on) {
        g_string_append(s, " (The recording that was under way is thrown"
                        " away.)");
    }
    res->summary = g_strdup(s->str);
    return res;
}

static McpCoverage *mcp_cover_stop(bool refine, bool ran, int64_t max,
                                   Error **errp)
{
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GHashTable) rec = NULL;
    g_autoptr(GHashTable) quiet = NULL;
    g_autoptr(GHashTable) old = NULL;
    g_autoptr(GArray) spans = NULL;
    GHashTable *list;
    GHashTableIter iter;
    gpointer value;
    McpCoverage *res;
    McpCoverageRangeList **tail;
    int64_t nquiet = 0, listed = 0;
    guint before = 0;
    guint i;

    if (mcp_cover.busy) {
        error_setg(errp, "debug-coverage start is still in its quiet part");
        return NULL;
    }
    if (!mcp_cover.on && !mcp_cover.rec) {
        error_setg(errp, "nothing is recording: call debug-coverage with"
                   " action start first");
        return NULL;
    }
    if (refine && !mcp_cover.list) {
        error_setg(errp, "refine looks at the blocks the stop before kept:"
                   " stop once without refine first");
        return NULL;
    }
    if (mcp_cover.on) {
        mcp_cover_end_recording();
    }
    rec = g_steal_pointer(&mcp_cover.rec);
    quiet = g_steal_pointer(&mcp_cover.quiet);

    res = g_new0(McpCoverage, 1);
    res->has_milliseconds = true;
    res->milliseconds = mcp_cover.ms;
    res->has_ran = true;
    res->ran = g_hash_table_size(rec);

    /* what ran, less what ran in the quiet part */
    list = mcp_cover_blocks();
    g_hash_table_iter_init(&iter, rec);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        McpCoverBlock *b = value;

        if (quiet && g_hash_table_contains(quiet, &b->key)) {
            nquiet++;
        } else if (!refine || (ran && g_hash_table_contains(mcp_cover.list,
                                                            &b->key))) {
            g_hash_table_iter_steal(&iter);
            g_hash_table_insert(list, &b->key, b);
        }
    }
    if (refine) {
        old = g_steal_pointer(&mcp_cover.list);
        before = g_hash_table_size(old);
    }
    if (refine && !ran) {
        /* the blocks on the list that did not run at all */
        g_hash_table_iter_init(&iter, old);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            McpCoverBlock *b = value;

            if (!g_hash_table_contains(rec, &b->key) &&
                !(quiet && g_hash_table_contains(quiet, &b->key))) {
                g_hash_table_iter_steal(&iter);
                g_hash_table_insert(list, &b->key, b);
            }
        }
    }
    g_clear_pointer(&mcp_cover.list, g_hash_table_unref);
    mcp_cover.list = list;
    res->has_quiet = !!quiet;
    res->quiet = nquiet;
    res->has_blocks = true;
    res->blocks = g_hash_table_size(list);

    spans = mcp_cover_spans(list);
    res->has_ranges = true;
    tail = &res->ranges;
    for (i = 0; i < spans->len && i < max; i++) {
        McpCoverSpan *sp = &g_array_index(spans, McpCoverSpan, i);
        McpCoverageRange *r = g_new0(McpCoverageRange, 1);
        hwaddr pa;

        r->address = g_strdup_printf("0x%" PRIx64, sp->pc);
        r->end = g_strdup_printf("0x%" PRIx64, sp->end);
        if (mcp_ram_phys(sp->key, &pa)) {
            r->physical = g_strdup_printf("0x%" HWADDR_PRIx, pa);
        }
        r->symbol = mcp_sym_name(sp->pc);
        r->blocks = sp->blocks;
        r->runs = sp->runs;
        QAPI_LIST_APPEND(tail, r);
        listed++;
    }

    g_string_assign(s, "Recorded ");
    mcp_cover_append_time(s, res->milliseconds);
    g_string_append_printf(s, ": %" PRId64 " block%s of code ran", res->ran,
                           res->ran == 1 ? "" : "s");
    if (quiet) {
        g_string_append_printf(s, "; %" PRId64 " of them ran in the quiet"
                               " part too and %s left out", nquiet,
                               nquiet == 1 ? "is" : "are");
    }
    g_string_append(s, ".\n");
    if (refine) {
        g_string_append_printf(s, "Of the %u block%s on the list, ",
                               before, before == 1 ? "" : "s");
        if (!res->blocks) {
            g_string_append_printf(s, "%s. The list is empty: to begin"
                                   " again, start, and stop without"
                                   " refine.", ran ? "none ran again"
                                                   : "every one ran");
        } else {
            g_string_append_printf(s, "%" PRId64 " %s: %s on the list",
                                   res->blocks,
                                   ran ? "ran again" : "did not run",
                                   res->blocks == 1 ? "it stays" :
                                   "they stay");
        }
    } else if (!res->blocks) {
        g_string_append(s, "The list is empty.");
    } else {
        g_string_append_printf(s, "On the list: %" PRId64 " block%s",
                               res->blocks, res->blocks == 1 ? "" : "s");
    }
    if (res->blocks) {
        g_string_append_printf(s, ", in %u range%s by address", spans->len,
                               spans->len == 1 ? "" : "s");
        if (listed < spans->len) {
            g_string_append_printf(s, "; the first %" PRId64, listed);
        }
        g_string_append(s, listed ? ":\n" : ".");
    }
    for (tail = &res->ranges; *tail; tail = &(*tail)->next) {
        McpCoverageRange *r = (*tail)->value;

        g_string_append_printf(s, "%s..%s%s%s%s", r->address, r->end,
                               r->symbol ? " <" : "",
                               r->symbol ? r->symbol : "",
                               r->symbol ? ">" : "");
        if (r->physical) {
            g_string_append_printf(s, " (physical %s)", r->physical);
        }
        g_string_append_printf(s, ": %" PRId64 " block%s, ran %" PRId64
                               " time%s\n", r->blocks,
                               r->blocks == 1 ? "" : "s", r->runs,
                               r->runs == 1 ? "" : "s");
    }
    if (mcp_cover.timed_out) {
        g_string_append_printf(s, "\nRecording had stopped by itself after"
                               " %d minutes.", COVER_LIMIT_MS / 60000);
    }
    if (mcp_cover.lost) {
        g_string_append_printf(s, "\nThe CPUs ran more blocks than it could"
                               " note: %" PRIu64 " were not noted.",
                               mcp_cover.lost);
    }
    if (spans->len > 1) {
        g_string_append(s, "\nTo narrow the list down: start again, do the"
                        " same thing, and stop with refine: true; or do"
                        " something else (or nothing), and stop with"
                        " refine: true and ran: false.");
    }
    if (res->blocks) {
        g_string_append(s, "\nTo stop at a block, give its address to"
                        " debug-run.");
    }
    res->summary = g_strdup(g_strchomp(s->str));
    return res;
}

McpCoverage *coroutine_fn qmp_debug_coverage(McpCoverageAction action,
                                             bool has_quiet_ms,
                                             int64_t quiet_ms,
                                             bool has_refine, bool refine,
                                             bool has_ran, bool ran,
                                             bool has_max, int64_t max,
                                             Error **errp)
{
    if (!tcg_enabled()) {
        error_setg(errp, "debug-coverage needs TCG (-accel tcg)");
        return NULL;
    }
    refine = has_refine && refine;
    if (has_ran && !refine) {
        error_setg(errp, "ran goes with refine");
        return NULL;
    }
    if (action == MCP_COVERAGE_ACTION_START) {
        if (has_refine || has_max) {
            error_setg(errp, "refine, ran and max go with stop");
            return NULL;
        }
        return mcp_cover_start(has_quiet_ms, quiet_ms, errp);
    }
    if (has_quiet_ms) {
        error_setg(errp, "quiet-ms goes with start");
        return NULL;
    }
    max = has_max ? max : COVER_LIST;
    if (max < 0 || max > COVER_LIST_MAX) {
        error_setg(errp, "max must be 0 to %d", COVER_LIST_MAX);
        return NULL;
    }
    return mcp_cover_stop(refine, !has_ran || ran, max, errp);
}

/*
 * register-write.  GDB's stub sets a register by name.  The pc goes
 * through the CPU's own set_pc hook, as the GDB stub's "continue at"
 * does, and may change nothing else the debug tools can see: not the
 * CPU's mode (its MMU modes and the flags code is translated with),
 * and no register that does not move as far as the pc.
 */

#define PC_MODE_BITS            4       /* low bits set_pc may take a mode in */

/* What a write may change in a CPU */
typedef struct McpCpuSeen {
    uint64_t pc;
    int mmu_idx[2];                     /* for data, for code */
    uint32_t tb_flags;
    int n_regs;                         /* GDB's registers */
    GByteArray **regs;
} McpCpuSeen;

typedef enum McpRegChange {
    REG_SAME,
    REG_MOVED,                          /* by as much as asked */
    REG_OTHER,
} McpRegChange;

static void mcp_cpu_see(CPUState *cpu, McpCpuSeen *s)
{
    const TCGCPUOps *ops = cpu->cc->tcg_ops;
    int i;

    s->pc = cpu->cc->get_pc(cpu);
    s->mmu_idx[0] = ops->mmu_index(cpu, false);
    s->mmu_idx[1] = ops->mmu_index(cpu, true);
    s->tb_flags = ops->get_tb_cpu_state(cpu).flags;
    /* all of GDB's: the pc may be outside the core ones, as on SPARC */
    s->n_regs = cpu->gdb_num_regs;
    s->regs = g_new(GByteArray *, s->n_regs);
    for (i = 0; i < s->n_regs; i++) {
        s->regs[i] = g_byte_array_new();
        gdb_read_register(cpu, s->regs[i], i);
    }
}

static void mcp_cpu_seen_free(McpCpuSeen *s)
{
    int i;

    for (i = 0; i < s->n_regs; i++) {
        g_byte_array_unref(s->regs[i]);
    }
    g_free(s->regs);
}

static uint64_t mcp_reg_mask(unsigned int len)
{
    return len >= 8 ? UINT64_MAX : (1ULL << (len * 8)) - 1;
}

/* How register @n changed since @was, read into @now */
static McpRegChange mcp_reg_change(CPUState *cpu, const McpCpuSeen *was,
                                   int n, uint64_t delta, GByteArray *now)
{
    const GByteArray *old = was->regs[n];
    uint64_t a, b;

    g_byte_array_set_size(now, 0);
    gdb_read_register(cpu, now, n);
    if (now->len == old->len && !memcmp(now->data, old->data, old->len)) {
        return REG_SAME;
    }
    if (now->len == old->len && mcp_reg_decode(cpu, old, &a) &&
        mcp_reg_decode(cpu, now, &b) &&
        !((b - a - delta) & mcp_reg_mask(now->len))) {
        return REG_MOVED;
    }
    return REG_OTHER;
}

/* A register's value: a number, else GDB's bytes in hex */
static char *mcp_reg_show(CPUState *cpu, const GByteArray *b)
{
    uint64_t v;

    if (mcp_reg_decode(cpu, b, &v)) {
        return g_strdup_printf("0x%" PRIx64, v);
    }
    return mcp_hex(b->data, b->len);
}

/*
 * Append the registers but @skip that changed since @was: all of
 * them, or those that did not move by @delta: "npc (0x2000 to 0x123c)".
 * Returns how many.
 */
static int mcp_reg_append_changes(GString *s, CPUState *cpu,
                                  const McpCpuSeen *was, bool all,
                                  uint64_t delta, int skip)
{
    g_autoptr(GArray) names = gdb_get_register_list(cpu);
    g_autoptr(GByteArray) now = g_byte_array_new();
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    int i, n;

    for (i = 0; i < was->n_regs; i++) {
        McpRegChange c = mcp_reg_change(cpu, was, i, delta, now);
        g_autofree char *name = NULL;
        g_autofree char *a = NULL;
        g_autofree char *b = NULL;

        if (i == skip || c == REG_SAME || (c == REG_MOVED && !all)) {
            continue;
        }
        name = mcp_reg_name(names, i);
        a = mcp_reg_show(cpu, was->regs[i]);
        b = mcp_reg_show(cpu, now);
        g_ptr_array_add(items, g_strdup_printf("%s (%s to %s)", name, a, b));
    }
    n = items->len;
    mcp_append_list(s, items, "and");
    return n;
}

/*
 * Whether @cpu is at @want with nothing changed since @was but the
 * registers that moved as far as the pc.  The translation flags come
 * last: some CPUs check them against the rest as they make them.
 */
static bool mcp_pc_alone(CPUState *cpu, const McpCpuSeen *was, uint64_t want)
{
    const TCGCPUOps *ops = cpu->cc->tcg_ops;
    g_autoptr(GByteArray) now = g_byte_array_new();
    int i;

    if (cpu->cc->get_pc(cpu) != want ||
        ops->mmu_index(cpu, false) != was->mmu_idx[0] ||
        ops->mmu_index(cpu, true) != was->mmu_idx[1]) {
        return false;
    }
    for (i = 0; i < was->n_regs; i++) {
        if (mcp_reg_change(cpu, was, i, want - was->pc, now) == REG_OTHER) {
            return false;
        }
    }
    return ops->get_tb_cpu_state(cpu).flags == was->tb_flags;
}

/*
 * Put @cpu at @want through set_pc, with @mode in the low bits: some
 * CPUs take a mode with the pc there (Thumb, MIPS16, PALcode, privilege
 * levels).  When the pc the tools show adds a base, such as the code
 * segment's on x86, set_pc takes the offset.
 */
static void mcp_pc_set(CPUState *cpu, uint64_t want, int mode)
{
    vaddr v = want | mode;
    vaddr got;

    cpu_set_pc(cpu, v);
    got = cpu->cc->get_pc(cpu);
    if (got != want) {
        cpu_set_pc(cpu, v - (got - want));
    }
}

/* Put @cpu back as @was saw it: the pc and the mode, then the registers */
static bool mcp_pc_restore(CPUState *cpu, const McpCpuSeen *was)
{
    const TCGCPUOps *ops = cpu->cc->tcg_ops;
    g_autoptr(GByteArray) now = g_byte_array_new();
    int mode, i;

    for (mode = 0; mode < PC_MODE_BITS; mode++) {
        mcp_pc_set(cpu, was->pc, mode);
        if (cpu->cc->get_pc(cpu) == was->pc &&
            ops->mmu_index(cpu, false) == was->mmu_idx[0] &&
            ops->mmu_index(cpu, true) == was->mmu_idx[1]) {
            break;
        }
    }
    for (i = 0; i < was->n_regs; i++) {
        if (was->regs[i]->len &&
            mcp_reg_change(cpu, was, i, 0, now) != REG_SAME) {
            gdb_write_register(cpu, was->regs[i]->data, i);
        }
    }
    return mcp_pc_alone(cpu, was, was->pc);
}

/*
 * @s as the value of a register of @len bytes: hex with 0x, decimal, or
 * a negative decimal
 */
static bool mcp_parse_reg_value(const char *s, unsigned int len,
                                uint64_t *val, Error **errp)
{
    uint64_t mask = mcp_reg_mask(len);
    int64_t neg;

    if (s[0] == '-') {
        if (qemu_strtoi64(s, NULL, 10, &neg) < 0) {
            error_setg(errp, "value '%s' is not a number: write hex with 0x,"
                       " decimal, or a negative decimal", s);
            return false;
        }
        if (len < 8 && neg < -(int64_t)(mask >> 1) - 1) {
            error_setg(errp, "value %s does not fit in %u byte%s", s, len,
                       len == 1 ? "" : "s");
            return false;
        }
        *val = (uint64_t)neg & mask;
        return true;
    }
    if (!mcp_parse_address(s, "value", val, errp)) {
        return false;
    }
    if (*val & ~mask) {
        error_setg(errp, "value %s does not fit in %u byte%s", s, len,
                   len == 1 ? "" : "s");
        return false;
    }
    return true;
}

static McpRegisterWriteResult *mcp_pc_write(CPUState *cpu, const char *value,
                                            const McpCpuSeen *was,
                                            Error **errp)
{
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GString) why = g_string_new(NULL);
    McpRegisterWriteResult *res;
    const char *hint = "";
    uint64_t want, pc;
    int mode;

    if (!mcp_parse_reg_value(value, 8, &want, errp)) {
        return NULL;
    }
    for (mode = 0; mode < PC_MODE_BITS; mode++) {
        mcp_pc_set(cpu, want, mode);
        if (mcp_pc_alone(cpu, was, want)) {
            break;
        }
        if (mode) {
            continue;
        }
        /* what the plain value changed, for the error */
        pc = cpu->cc->get_pc(cpu);
        if (pc != want) {
            g_string_printf(why, "put it at 0x%" PRIx64 " instead", pc);
            continue;
        }
        g_string_assign(why, "also change ");
        if (!mcp_reg_append_changes(why, cpu, was, false, want - was->pc,
                                    -1)) {
            g_string_append(why, "its mode");
        }
        hint = ". Set the registers by name instead, or, in a branch delay"
               " slot, debug-step once first";
    }
    if (mode == PC_MODE_BITS) {
        if (!mcp_pc_restore(cpu, was)) {
            error_setg(errp, "setting the pc of CPU %d to 0x%" PRIx64
                       " would %s, and CPU %d could not be put back exactly"
                       " as it was", cpu->cpu_index, want, why->str,
                       cpu->cpu_index);
            return NULL;
        }
        error_setg(errp, "CPU %d stays at 0x%" PRIx64 ": setting its pc to"
                   " 0x%" PRIx64 " would %s%s", cpu->cpu_index, was->pc,
                   want, why->str, hint);
        return NULL;
    }

    res = g_new0(McpRegisterWriteResult, 1);
    res->before = g_strdup_printf("0x%" PRIx64, was->pc);
    res->after = g_strdup_printf("0x%" PRIx64, want);
    g_string_printf(s, "set the pc of CPU %d to 0x%" PRIx64 " (it was 0x%"
                    PRIx64 ")", cpu->cpu_index, want, was->pc);
    if (want != was->pc) {
        g_string_append(s, ", in ");
        mcp_reg_append_changes(s, cpu, was, true, 0, -1);
    }
    res->summary = g_strdup(s->str);
    return res;
}

static McpRegisterWriteResult *mcp_reg_write(CPUState *cpu, int reg,
                                             const char *value,
                                             const McpCpuSeen *was,
                                             Error **errp)
{
    g_autoptr(GArray) names = gdb_get_register_list(cpu);
    g_autoptr(GByteArray) old = g_byte_array_new();
    g_autoptr(GByteArray) now = g_byte_array_new();
    g_autoptr(GString) s = g_string_new(NULL);
    g_autoptr(GString) also = g_string_new(NULL);
    g_autofree char *name = mcp_reg_name(names, reg);
    bool be = cpu_internal_is_big_endian(cpu);
    McpRegisterWriteResult *res;
    uint64_t want, before, after, pc;
    uint8_t buf[8];
    unsigned int i;

    gdb_read_register(cpu, old, reg);
    if (!mcp_reg_decode(cpu, old, &before)) {
        error_setg(errp, "%s has %u bytes; register-write sets registers of"
                   " 1 to 8 bytes", name, old->len);
        return NULL;
    }
    if (!mcp_parse_reg_value(value, old->len, &want, errp)) {
        return NULL;
    }
    /* in the byte order mcp_reg_decode() reads */
    for (i = 0; i < old->len; i++) {
        buf[be ? old->len - 1 - i : i] = want >> (8 * i);
    }
    if (!gdb_write_register(cpu, buf, reg)) {
        error_setg(errp, "CPU %d does not let GDB's stub write %s",
                   cpu->cpu_index, name);
        return NULL;
    }
    gdb_read_register(cpu, now, reg);
    if (!mcp_reg_decode(cpu, now, &after)) {
        after = want;
    }

    res = g_new0(McpRegisterWriteResult, 1);
    res->before = g_strdup_printf("0x%" PRIx64, before);
    res->after = g_strdup_printf("0x%" PRIx64, after);
    g_string_printf(s, "set %s of CPU %d to 0x%" PRIx64 " (it was 0x%" PRIx64
                    ")", name, cpu->cpu_index, want, before);
    if (after != want) {
        g_string_append_printf(s, ", but it reads back 0x%" PRIx64, after);
    }
    pc = cpu->cc->get_pc(cpu);
    if (pc != was->pc) {
        g_string_append_printf(s, "; the pc moved with it, to 0x%" PRIx64
                               " (it was 0x%" PRIx64 ")", pc, was->pc);
    }
    if (mcp_reg_append_changes(also, cpu, was, true, 0, reg)) {
        g_string_append_printf(s, "; it also changed %s", also->str);
    }
    res->summary = g_strdup(s->str);
    return res;
}

McpRegisterWriteResult *qmp_register_write(const char *q_register,
                                           const char *value, bool has_cpu,
                                           int64_t cpu_index, Error **errp)
{
    McpRegisterWriteResult *res;
    McpCpuSeen was;
    CPUState *cpu;
    int reg = -1;

    if (!tcg_enabled()) {
        error_setg(errp, "register-write needs TCG (-accel tcg)");
        return NULL;
    }
    if (replay_mode != REPLAY_MODE_NONE) {
        error_setg(errp, "registers are not written while QEMU records or"
                   " replays");
        return NULL;
    }
    if (mcp_debug.busy) {
        error_setg(errp, "a debug tool is running the VM; wait until it"
                   " ends");
        return NULL;
    }
    if (runstate_is_running()) {
        error_setg(errp, "the VM is running: stop it first with debug-run,"
                   " debug-step or vm-power pause");
        return NULL;
    }
    cpu = mcp_debug_cpu(has_cpu, cpu_index, errp);
    if (!cpu) {
        return NULL;
    }
    if (g_ascii_strcasecmp(q_register, "pc")) {
        reg = mcp_reg_number(cpu, q_register);
        if (reg < 0) {
            g_autofree char *names = mcp_reg_names(cpu);

            error_setg(errp, "'%s' is not a register; the registers are %s",
                       q_register, names);
            return NULL;
        }
    }
    mcp_cpu_see(cpu, &was);
    if (reg < 0) {
        res = mcp_pc_write(cpu, value, &was, errp);
    } else {
        res = mcp_reg_write(cpu, reg, value, &was, errp);
    }
    mcp_cpu_seen_free(&was);
    return res;
}

#endif /* CONFIG_TCG */
