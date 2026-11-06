/*
 * Disc and floppy images made from host files, and files read back from
 * a floppy, so files can go into and out of a guest through its CD-ROM
 * or floppy drive, with nothing to set up in the guest.
 *
 * - disc: ISO 9660 level 1, whose 8.3 names every guest reads, with
 *   Rock Ridge for the real names and modes in Unix guests that read it
 * - floppy: FAT12, 1.44 MB, 8.3 names, which DOS, Windows, OS/2 and the
 *   DOS tools of Unix guests read and write
 * - copy-out: the files on a FAT12 or FAT16 medium into a host folder,
 *   with the long names that Windows and OS/2 give them
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "system/mcp-image.h"

#define ISO_SECTOR          2048
#define ISO_LEVELS          8       /* folder levels, the top one included */
#define ISO_NM_MAX          160     /* longest Rock Ridge name in a record */
#define ISO_PAD             150     /* sectors at the end, for read-ahead */
#define FAT_SECTOR          512
#define FAT_TOTAL           2880    /* sectors of a 1.44 MB floppy */
#define FAT_SPF             9       /* sectors per FAT */
#define FAT_ROOT            224     /* entries of the top folder */
#define FAT_DATA            (1 + 2 * FAT_SPF + FAT_ROOT * 32 / FAT_SECTOR)
#define FAT_CLUSTERS        (FAT_TOTAL - FAT_DATA)
#define FAT_MAX_DEPTH       32
#define FAT_DIR_MAX         65536   /* entries a FAT folder can have */
#define IMG_MAX_ENTRIES     100000
#define IMG_RENAMED_MAX     40      /* renames listed for the caller */
#define IMG_CHUNK           (1024 * 1024)

typedef struct ImgNode ImgNode;

struct ImgNode {
    char *host;                 /* the host path; NULL for the top folder */
    char *name;                 /* the host name */
    char id[13];                /* the 8.3 name, "NAME.EXT" or "NAME" */
    bool dir;
    bool exec;
    uint64_t size;
    time_t mtime;
    ImgNode *parent;
    GPtrArray *kids;            /* of a folder: by id once named */
    uint32_t start;             /* disc: first sector; floppy: cluster */
    uint32_t len;               /* disc: bytes of a folder; floppy: clusters */
    int dirno;                  /* disc: number of a folder, from 1 */
};

void mcp_image_info_init(McpImageInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->renamed = g_string_new(NULL);
    info->skipped = g_string_new(NULL);
}

void mcp_image_info_free(McpImageInfo *info)
{
    g_string_free(info->renamed, true);
    g_string_free(info->skipped, true);
}

static void img_skip(McpImageInfo *info, const char *what, const char *why)
{
    if (info->skipped->len < 400) {
        g_string_append_printf(info->skipped, "%s%s (%s)",
                               info->skipped->len ? ", " : "", what, why);
    }
}

/* Windows has no pwrite() */
static bool img_pwrite(int fd, const void *buf, size_t len, uint64_t off,
                       Error **errp)
{
    if (lseek(fd, off, SEEK_SET) < 0 || qemu_write_full(fd, buf, len) != len) {
        error_setg_errno(errp, errno ? errno : ENOSPC,
                         "cannot write the image");
        return false;
    }
    return true;
}

/* Copy @size bytes of the host file @host into the image at @off */
static bool img_copy_in(int fd, uint64_t off, const char *host,
                        uint64_t size, Error **errp)
{
    g_autofree uint8_t *buf = g_malloc(IMG_CHUNK);
    uint64_t done = 0;
    int in = qemu_open(host, O_RDONLY | O_BINARY, errp);

    if (in < 0) {
        return false;
    }
    while (done < size) {
        ssize_t n = read(in, buf, MIN(IMG_CHUNK, size - done));

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            error_setg(errp, "'%s' got shorter while it was copied", host);
            qemu_close(in);
            return false;
        }
        if (!img_pwrite(fd, buf, n, off + done, errp)) {
            qemu_close(in);
            return false;
        }
        done += n;
    }
    qemu_close(in);
    return true;
}

/* The tree of files to put on the image */

static void img_node_free(gpointer opaque)
{
    ImgNode *n = opaque;

    if (n->kids) {
        g_ptr_array_free(n->kids, true);
    }
    g_free(n->host);
    g_free(n->name);
    g_free(n);
}

static ImgNode *img_node_new(ImgNode *parent, const char *host, bool dir)
{
    ImgNode *n = g_new0(ImgNode, 1);

    n->host = g_strdup(host);
    n->name = host ? g_path_get_basename(host) : g_strdup("");
    n->dir = dir;
    n->parent = parent;
    if (dir) {
        n->kids = g_ptr_array_new_with_free_func(img_node_free);
    }
    if (parent) {
        g_ptr_array_add(parent->kids, n);
    }
    return n;
}

/* Add the host file or folder @host to @dir, which is at @level */
static bool img_add(ImgNode *dir, const char *host, int level,
                    McpImageInfo *info, int *entries, Error **errp)
{
    struct stat st;
    ImgNode *n;

    if (stat(host, &st) < 0) {
        error_setg_errno(errp, errno, "cannot read '%s'", host);
        return false;
    }
    if (++*entries > IMG_MAX_ENTRIES) {
        error_setg(errp, "more than %d files and folders", IMG_MAX_ENTRIES);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        g_autoptr(GError) gerr = NULL;
        const char *e;
        GDir *d;

        if (level >= ISO_LEVELS) {
            error_setg(errp, "'%s' is more than %d folders deep", host,
                       ISO_LEVELS - 1);
            return false;
        }
        d = g_dir_open(host, 0, &gerr);
        if (!d) {
            error_setg(errp, "cannot read folder '%s': %s", host,
                       gerr->message);
            return false;
        }
        n = img_node_new(dir, host, true);
        n->mtime = st.st_mtime;
        info->folders++;
        while ((e = g_dir_read_name(d))) {
            g_autofree char *p = g_build_filename(host, e, NULL);

            if (!img_add(n, p, level + 1, info, entries, errp)) {
                g_dir_close(d);
                return false;
            }
        }
        g_dir_close(d);
        return true;
    }
    if (!S_ISREG(st.st_mode)) {
        img_skip(info, host, "not a file or folder");
        return true;
    }
    if (st.st_size > UINT32_MAX) {
        error_setg(errp, "'%s' is 4 GB or more, too big for one file on a"
                   " disc", host);
        return false;
    }
    n = img_node_new(dir, host, false);
    n->size = st.st_size;
    n->exec = st.st_mode & S_IXUSR;
    n->mtime = st.st_mtime;
    info->files++;
    info->bytes += st.st_size;
    return true;
}

/* 8.3 names */

static bool img_char_ok(char c, bool fat)
{
    return g_ascii_isupper(c) || g_ascii_isdigit(c) || c == '_' ||
           (fat && c && strchr("!#$%&'()-@^`{}~", c));
}

/* @len bytes of @in in upper case, others as '_', at most @max */
static void img_part(char *out, const char *in, size_t len, size_t max,
                     bool fat)
{
    size_t i, n = 0;

    for (i = 0; i < len && n < max; i++) {
        char c = g_ascii_toupper(in[i]);

        if (((uint8_t)c & 0xc0) == 0x80) {
            continue;           /* the rest of a UTF-8 character */
        }
        out[n++] = img_char_ok(c, fat) ? c : '_';
    }
    out[n] = 0;
}

/* Give @n an 8.3 name that nothing in @used has */
static void img_name(ImgNode *n, GHashTable *used, bool fat)
{
    const char *name = n->name, *dot;
    char base[9], ext[4], b[9];
    int i;

    while (*name == '.') {
        name++;                 /* ".profile" is "PROFILE" */
    }
    dot = n->dir ? NULL : strrchr(name, '.');
    img_part(base, name, dot ? dot - name : strlen(name), 8, fat);
    img_part(ext, dot ? dot + 1 : "", dot ? strlen(dot + 1) : 0, 3, fat);
    if (!base[0]) {
        strcpy(base, "_");
    }
    for (i = 0;; i++) {
        if (i) {
            char sfx[12];
            int l = snprintf(sfx, sizeof(sfx), "_%d", i);

            snprintf(b, sizeof(b), "%.*s%s", (int)MIN(strlen(base), 8 - l),
                     base, sfx);
        } else {
            g_strlcpy(b, base, sizeof(b));
        }
        if (ext[0]) {
            snprintf(n->id, sizeof(n->id), "%s.%s", b, ext);
        } else {
            snprintf(n->id, sizeof(n->id), "%s", b);
        }
        if (!g_hash_table_contains(used, n->id)) {
            break;
        }
    }
    g_hash_table_add(used, g_strdup(n->id));
}

static int img_cmp_name(gconstpointer a, gconstpointer b)
{
    return strcmp((*(ImgNode **)a)->name, (*(ImgNode **)b)->name);
}

static int img_cmp_id(gconstpointer a, gconstpointer b)
{
    return strcmp((*(ImgNode **)a)->id, (*(ImgNode **)b)->id);
}

/* Name everything below @d, in @path on the image, and sort it so */
static void img_name_tree(ImgNode *d, const char *path, bool fat,
                          McpImageInfo *info)
{
    g_autoptr(GHashTable) used = g_hash_table_new_full(g_str_hash,
                                                       g_str_equal,
                                                       g_free, NULL);
    guint i;

    g_ptr_array_sort(d->kids, img_cmp_name);
    for (i = 0; i < d->kids->len; i++) {
        ImgNode *k = g_ptr_array_index(d->kids, i);
        g_autofree char *p = NULL;

        img_name(k, used, fat);
        p = *path ? g_strdup_printf("%s/%s", path, k->id) : g_strdup(k->id);
        if (g_ascii_strcasecmp(k->id, k->name) &&
            info->n_renamed++ < IMG_RENAMED_MAX) {
            g_string_append_printf(info->renamed, "%s%s (%s)",
                                   info->renamed->len ? ", " : "", p,
                                   k->name);
        }
        if (k->dir) {
            img_name_tree(k, p, fat, info);
        }
    }
    g_ptr_array_sort(d->kids, img_cmp_id);
}

/* Disc: ISO 9660 with Rock Ridge */

typedef struct IsoCtx {
    ImgNode *root;
    GPtrArray *dirs;            /* in path table order */
    uint32_t pt_lba, pt_len, pt_sectors;
    uint32_t ce_lba;            /* the sector with Rock Ridge's ER entry */
    uint32_t files_lba;
    uint32_t total;
} IsoCtx;

static const char rr_er_id[] = "RRIP_1991A";
static const char rr_er_des[] =
    "THE ROCK RIDGE INTERCHANGE PROTOCOL PROVIDES SUPPORT FOR POSIX FILE "
    "SYSTEM SEMANTICS";
static const char rr_er_src[] =
    "PLEASE CONTACT DISC PUBLISHER FOR SPECIFICATION SOURCE.  SEE PUBLISHER "
    "IDENTIFIER IN PRIMARY VOLUME DESCRIPTOR FOR CONTACT INFORMATION.";
#define RR_ER_LEN   (8 + sizeof(rr_er_id) - 1 + sizeof(rr_er_des) - 1 + \
                     sizeof(rr_er_src) - 1)

static void iso_put16(uint8_t *p, uint16_t v)
{
    stw_le_p(p, v);
    stw_be_p(p + 2, v);
}

static void iso_put32(uint8_t *p, uint32_t v)
{
    stl_le_p(p, v);
    stl_be_p(p + 4, v);
}

static void iso_date7(uint8_t *p, time_t t)
{
    struct tm tm;

    gmtime_r(&t, &tm);
    p[0] = MIN(MAX(tm.tm_year, 0), 255);    /* years since 1900 */
    p[1] = tm.tm_mon + 1;
    p[2] = tm.tm_mday;
    p[3] = tm.tm_hour;
    p[4] = tm.tm_min;
    p[5] = tm.tm_sec;
    p[6] = 0;                               /* GMT */
}

static void iso_date17(uint8_t *p, time_t t)
{
    char s[64];
    struct tm tm;

    gmtime_r(&t, &tm);
    snprintf(s, sizeof(s), "%04d%02d%02d%02d%02d%02d00", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    memcpy(p, s, 16);
    p[16] = 0;
}

/* The entry that record @i of folder @d is about: 0 is ".", 1 ".." */
static ImgNode *iso_entry(ImgNode *d, int i)
{
    if (i == 0) {
        return d;
    }
    if (i == 1) {
        return d->parent ? d->parent : d;
    }
    return g_ptr_array_index(d->kids, i - 2);
}

static size_t rr_px(uint8_t *p, const ImgNode *n)
{
    memcpy(p, "PX\x24\x01", 4);
    iso_put32(p + 4, n->dir ? 040755 : n->exec ? 0100755 : 0100644);
    iso_put32(p + 12, n->dir ? 2 : 1);
    iso_put32(p + 20, 0);
    iso_put32(p + 28, 0);
    return 36;
}

/* The System Use field of record @i of folder @d: Rock Ridge entries */
static size_t rr_su(IsoCtx *c, ImgNode *d, int i, uint8_t *su)
{
    ImgNode *n = iso_entry(d, i);
    bool top = i == 0 && d == c->root;
    size_t len = 0, nm = i >= 2 ? strlen(n->name) : 0;

    if (nm > ISO_NM_MAX) {
        nm = 0;                 /* it keeps its 8.3 name */
    }
    if (top) {
        memcpy(su, "SP\x07\x01\xbe\xef\x00", 7);
        len = 7;
    }
    memcpy(su + len, "RR\x05\x01", 4);
    su[len + 4] = 0x01 | (nm ? 0x08 : 0);       /* PX, NM */
    len += 5;
    if (nm) {
        su[len] = 'N';
        su[len + 1] = 'M';
        su[len + 2] = 5 + nm;
        su[len + 3] = 1;
        su[len + 4] = 0;
        memcpy(su + len + 5, n->name, nm);
        len += 5 + nm;
    }
    len += rr_px(su + len, n);
    if (top) {
        memcpy(su + len, "CE\x1c\x01", 4);
        iso_put32(su + len + 4, c->ce_lba);
        iso_put32(su + len + 12, 0);
        iso_put32(su + len + 20, RR_ER_LEN);
        len += 28;
    }
    if (len & 1) {
        su[len++] = 0;          /* records have an even length */
    }
    return len;
}

/* Write record @i of folder @d to @p, if not NULL; its size */
static int iso_record(IsoCtx *c, ImgNode *d, int i, uint8_t *p)
{
    ImgNode *n = iso_entry(d, i);
    uint8_t su[256];
    char id[16];
    size_t idlen, sulen, pad;
    int size;

    if (i < 2) {
        id[0] = i;
        idlen = 1;
    } else if (n->dir) {
        idlen = snprintf(id, sizeof(id), "%s", n->id);
    } else {
        idlen = snprintf(id, sizeof(id), "%s%s;1", n->id,
                         strchr(n->id, '.') ? "" : ".");
    }
    sulen = rr_su(c, d, i, su);
    pad = !(idlen & 1);
    size = 33 + idlen + pad + sulen;
    assert(size <= 255);
    if (p) {
        memset(p, 0, size);
        p[0] = size;
        iso_put32(p + 2, n->start);
        iso_put32(p + 10, n->dir ? n->len : n->size);
        iso_date7(p + 18, n->mtime);
        p[25] = n->dir ? 2 : 0;
        iso_put16(p + 28, 1);
        p[32] = idlen;
        memcpy(p + 33, id, idlen);
        memcpy(p + 33 + idlen + pad, su, sulen);
    }
    return size;
}

/* Lay out the records of folder @d at @p, if not NULL; its bytes */
static uint32_t iso_dir(IsoCtx *c, ImgNode *d, uint8_t *p)
{
    uint32_t off = 0;
    guint i;

    for (i = 0; i < d->kids->len + 2; i++) {
        int size = iso_record(c, d, i, NULL);

        if (off % ISO_SECTOR + size > ISO_SECTOR) {
            off = ROUND_UP(off, ISO_SECTOR);    /* no record spans sectors */
        }
        if (p) {
            iso_record(c, d, i, p + off);
        }
        off += size;
    }
    return ROUND_UP(off, ISO_SECTOR);
}

/* Write the path table to @p, if not NULL; its bytes */
static uint32_t iso_path_table(IsoCtx *c, uint8_t *p, bool msb)
{
    uint32_t off = 0;
    guint i;

    for (i = 0; i < c->dirs->len; i++) {
        ImgNode *d = g_ptr_array_index(c->dirs, i);
        size_t idlen = d == c->root ? 1 : strlen(d->id);
        int parent = d->parent ? d->parent->dirno : 1;

        if (p) {
            p[off] = idlen;
            p[off + 1] = 0;
            if (msb) {
                stl_be_p(p + off + 2, d->start);
                stw_be_p(p + off + 6, parent);
            } else {
                stl_le_p(p + off + 2, d->start);
                stw_le_p(p + off + 6, parent);
            }
            memcpy(p + off + 8, d == c->root ? "" : d->id, idlen);
        }
        off += 8 + idlen + (idlen & 1);
    }
    return off;
}

static void iso_pvd(IsoCtx *c, uint8_t *p)
{
    ImgNode *r = c->root;
    time_t now = time(NULL);

    p[0] = 1;
    memcpy(p + 1, "CD001", 5);
    p[6] = 1;
    memset(p + 8, ' ', 64);                 /* system and volume ids */
    memcpy(p + 40, "LEMU", 4);
    iso_put32(p + 80, c->total);
    iso_put16(p + 120, 1);                  /* volume set size */
    iso_put16(p + 124, 1);                  /* volume sequence number */
    iso_put16(p + 128, ISO_SECTOR);
    iso_put32(p + 132, c->pt_len);
    stl_le_p(p + 140, c->pt_lba);
    stl_be_p(p + 148, c->pt_lba + c->pt_sectors);
    /* the record of the top folder */
    p[156] = 34;
    iso_put32(p + 158, r->start);
    iso_put32(p + 166, r->len);
    iso_date7(p + 174, r->mtime);
    p[181] = 2;
    iso_put16(p + 184, 1);
    p[188] = 1;
    memset(p + 190, ' ', 813 - 190);        /* set, publisher, ... ids */
    memcpy(p + 446, "LEMU", 4);             /* data preparer */
    iso_date17(p + 813, now);               /* created */
    iso_date17(p + 830, now);               /* modified */
    memset(p + 847, '0', 16);               /* expires: never */
    memset(p + 864, '0', 16);               /* effective: at once */
    p[881] = 1;
}

static bool iso_write(int fd, ImgNode *root, Error **errp)
{
    IsoCtx c = { .root = root };
    g_autoptr(GPtrArray) dirs = g_ptr_array_new();
    g_autofree uint8_t *meta = NULL;
    uint8_t *er;
    uint32_t lba;
    guint i, j;

    /* folders by level, then parent, then name: the path table order */
    c.dirs = dirs;
    g_ptr_array_add(dirs, root);
    for (i = 0; i < dirs->len; i++) {
        ImgNode *d = g_ptr_array_index(dirs, i);

        d->dirno = i + 1;
        for (j = 0; j < d->kids->len; j++) {
            ImgNode *k = g_ptr_array_index(d->kids, j);

            if (k->dir) {
                g_ptr_array_add(dirs, k);
            }
        }
    }
    if (dirs->len > UINT16_MAX) {
        error_setg(errp, "more than %d folders", UINT16_MAX);
        return false;
    }

    /*
     * 16: volume descriptor, 17: end of them, path tables, folders.  The
     * ER sector goes right after the top folder: readers that stream the
     * disc, like libarchive, want it after the record that points to it.
     */
    c.pt_len = iso_path_table(&c, NULL, false);
    c.pt_sectors = DIV_ROUND_UP(c.pt_len, ISO_SECTOR);
    c.pt_lba = 18;
    lba = c.pt_lba + 2 * c.pt_sectors;
    for (i = 0; i < dirs->len; i++) {
        ImgNode *d = g_ptr_array_index(dirs, i);

        d->start = lba;
        d->len = iso_dir(&c, d, NULL);
        lba += d->len / ISO_SECTOR;
        if (d == root) {
            c.ce_lba = lba++;
        }
    }
    c.files_lba = lba;
    for (i = 0; i < dirs->len; i++) {
        ImgNode *d = g_ptr_array_index(dirs, i);

        for (j = 0; j < d->kids->len; j++) {
            ImgNode *k = g_ptr_array_index(d->kids, j);
            uint64_t end;

            if (k->dir) {
                continue;
            }
            /*
             * An empty file gets a sector of its own too: AIX numbers
             * files by their first sector, and hides a file at sector 0
             */
            k->start = lba;
            end = lba + MAX(DIV_ROUND_UP(k->size, ISO_SECTOR), 1);
            if (end > UINT32_MAX) {
                error_setg(errp, "the files do not fit on one disc");
                return false;
            }
            lba = end;
        }
    }
    c.total = lba + ISO_PAD;

    meta = g_malloc0((size_t)c.files_lba * ISO_SECTOR);
    iso_pvd(&c, meta + 16 * ISO_SECTOR);
    meta[17 * ISO_SECTOR] = 255;
    memcpy(meta + 17 * ISO_SECTOR + 1, "CD001", 5);
    meta[17 * ISO_SECTOR + 6] = 1;
    iso_path_table(&c, meta + c.pt_lba * ISO_SECTOR, false);
    iso_path_table(&c, meta + (c.pt_lba + c.pt_sectors) * ISO_SECTOR, true);
    QEMU_BUILD_BUG_ON(RR_ER_LEN > 255);
    er = meta + c.ce_lba * ISO_SECTOR;
    er[0] = 'E';
    er[1] = 'R';
    er[2] = RR_ER_LEN;
    er[3] = 1;
    er[4] = sizeof(rr_er_id) - 1;
    er[5] = sizeof(rr_er_des) - 1;
    er[6] = sizeof(rr_er_src) - 1;
    er[7] = 1;
    memcpy(er + 8, rr_er_id, er[4]);
    memcpy(er + 8 + er[4], rr_er_des, er[5]);
    memcpy(er + 8 + er[4] + er[5], rr_er_src, er[6]);
    for (i = 0; i < dirs->len; i++) {
        ImgNode *d = g_ptr_array_index(dirs, i);

        iso_dir(&c, d, meta + (size_t)d->start * ISO_SECTOR);
    }
    if (!img_pwrite(fd, meta, (size_t)c.files_lba * ISO_SECTOR, 0, errp)) {
        return false;
    }
    for (i = 0; i < dirs->len; i++) {
        ImgNode *d = g_ptr_array_index(dirs, i);

        for (j = 0; j < d->kids->len; j++) {
            ImgNode *k = g_ptr_array_index(d->kids, j);

            if (!k->dir && k->size &&
                !img_copy_in(fd, (uint64_t)k->start * ISO_SECTOR, k->host,
                             k->size, errp)) {
                return false;
            }
        }
    }
    if (ftruncate(fd, (off_t)c.total * ISO_SECTOR) < 0) {
        error_setg_errno(errp, errno, "cannot write the image");
        return false;
    }
    return true;
}

/* Floppy: FAT12, 1.44 MB */

static void fat12_set(uint8_t *fat, uint32_t n, uint32_t v)
{
    uint8_t *p = fat + n * 3 / 2;

    if (n & 1) {
        p[0] = (p[0] & 0x0f) | (v << 4);
        p[1] = v >> 4;
    } else {
        p[0] = v;
        p[1] = (p[1] & 0xf0) | ((v >> 8) & 0x0f);
    }
}

static void fat_time(time_t t, uint16_t *date, uint16_t *time)
{
    struct tm tm;

    localtime_r(&t, &tm);
    if (tm.tm_year < 80) {
        *date = (1 << 5) | 1;           /* 1980-01-01 */
        *time = 0;
        return;
    }
    *date = ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
    *time = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
}

static bool fat_all_lower(const char *s, size_t len)
{
    bool lower = false;
    size_t i;

    for (i = 0; i < len; i++) {
        if (g_ascii_isupper(s[i])) {
            return false;
        }
        lower |= g_ascii_islower(s[i]);
    }
    return lower;
}

/*
 * Windows NT shows the 8.3 name of @n in lower case, like the host
 * name, if that is all it differs in
 */
static uint8_t fat_nt_case(const ImgNode *n)
{
    const char *dot;
    size_t base;
    uint8_t flags = 0;

    if (g_ascii_strcasecmp(n->id, n->name)) {
        return 0;
    }
    dot = strchr(n->id, '.');
    base = dot ? (size_t)(dot - n->id) : strlen(n->id);
    if (fat_all_lower(n->name, base)) {
        flags |= 0x08;
    }
    if (dot && fat_all_lower(n->name + base + 1, strlen(dot + 1))) {
        flags |= 0x10;
    }
    return flags;
}

static void fat_dirent(uint8_t *e, const char *id, bool dir, uint32_t cluster,
                       uint32_t size, time_t mtime, uint8_t nt_case)
{
    const char *dot = strchr(id, '.');
    uint16_t date, time;

    memset(e, ' ', 11);
    if (dot == id) {
        memcpy(e, id, strlen(id));      /* "." and ".." */
    } else {
        memcpy(e, id, dot ? dot - id : strlen(id));
        if (dot) {
            memcpy(e + 8, dot + 1, strlen(dot + 1));
        }
    }
    e[11] = dir ? 0x10 : 0x20;
    memset(e + 12, 0, 20);
    e[12] = nt_case;
    fat_time(mtime, &date, &time);
    stw_le_p(e + 14, time);             /* created */
    stw_le_p(e + 16, date);
    stw_le_p(e + 18, date);             /* used */
    stw_le_p(e + 22, time);             /* written */
    stw_le_p(e + 24, date);
    stw_le_p(e + 26, cluster);
    stl_le_p(e + 28, dir ? 0 : size);
}

/* Clusters for everything below @d, from *@next on */
static void fat_alloc(ImgNode *d, uint32_t *next)
{
    guint i;

    for (i = 0; i < d->kids->len; i++) {
        ImgNode *k = g_ptr_array_index(d->kids, i);

        k->len = k->dir ? DIV_ROUND_UP((k->kids->len + 2) * 32, FAT_SECTOR)
                        : DIV_ROUND_UP(k->size, FAT_SECTOR);
        k->start = k->len ? *next : 0;
        *next += k->len;
        if (k->dir) {
            fat_alloc(k, next);
        }
    }
}

static void fat_chains(ImgNode *d, uint8_t *fat)
{
    guint i, j;

    for (i = 0; i < d->kids->len; i++) {
        ImgNode *k = g_ptr_array_index(d->kids, i);

        for (j = 0; j < k->len; j++) {
            fat12_set(fat, k->start + j,
                      j + 1 < k->len ? k->start + j + 1 : 0xfff);
        }
        if (k->dir) {
            fat_chains(k, fat);
        }
    }
}

static void fat_boot(uint8_t *b)
{
    /* int 18h: a BIOS that boots from it goes on to the next device */
    static const uint8_t code[] = { 0xcd, 0x18, 0xeb, 0xfe };

    memcpy(b, "\xeb\x3c\x90" "LEMU    ", 11);
    stw_le_p(b + 11, FAT_SECTOR);
    b[13] = 1;                          /* sectors per cluster */
    stw_le_p(b + 14, 1);                /* reserved sectors */
    b[16] = 2;                          /* FATs */
    stw_le_p(b + 17, FAT_ROOT);
    stw_le_p(b + 19, FAT_TOTAL);
    b[21] = 0xf0;                       /* a 1.44 MB floppy */
    stw_le_p(b + 22, FAT_SPF);
    stw_le_p(b + 24, 18);               /* sectors per track */
    stw_le_p(b + 26, 2);                /* heads */
    b[38] = 0x29;                       /* the fields below are there */
    stl_le_p(b + 39, (uint32_t)time(NULL));
    memcpy(b + 43, "LEMU       FAT12   ", 19);
    memcpy(b + 62, code, sizeof(code));
    b[510] = 0x55;
    b[511] = 0xaa;
}

static uint64_t fat_offset(uint32_t cluster)
{
    return (uint64_t)(FAT_DATA + cluster - 2) * FAT_SECTOR;
}

/* Write the folders and files below @d */
static bool fat_write_dir(int fd, ImgNode *d, Error **errp)
{
    guint i, j;

    for (i = 0; i < d->kids->len; i++) {
        ImgNode *k = g_ptr_array_index(d->kids, i);

        if (k->dir) {
            g_autofree uint8_t *buf = g_malloc0(k->len * FAT_SECTOR);
            ImgNode *up = k->parent;

            fat_dirent(buf, ".", true, k->start, 0, k->mtime, 0);
            fat_dirent(buf + 32, "..", true, up->parent ? up->start : 0, 0,
                       up->mtime, 0);
            for (j = 0; j < k->kids->len; j++) {
                ImgNode *e = g_ptr_array_index(k->kids, j);

                fat_dirent(buf + 64 + j * 32, e->id, e->dir, e->start,
                           e->size, e->mtime, fat_nt_case(e));
            }
            if (!img_pwrite(fd, buf, k->len * FAT_SECTOR,
                            fat_offset(k->start), errp) ||
                !fat_write_dir(fd, k, errp)) {
                return false;
            }
        } else if (k->size &&
                   !img_copy_in(fd, fat_offset(k->start), k->host, k->size,
                                errp)) {
            return false;
        }
    }
    return true;
}

static bool fat_write(int fd, ImgNode *root, Error **errp)
{
    g_autofree uint8_t *meta = g_malloc0(FAT_DATA * FAT_SECTOR);
    uint8_t *fat = meta + FAT_SECTOR;
    uint8_t *top = meta + (1 + 2 * FAT_SPF) * FAT_SECTOR;
    uint32_t next = 2;
    guint i;

    if (root->kids->len > FAT_ROOT) {
        error_setg(errp, "the top folder of a floppy holds at most %d files"
                   " and folders; put them in a folder", FAT_ROOT);
        return false;
    }
    fat_alloc(root, &next);
    if (next - 2 > FAT_CLUSTERS) {
        error_setg(errp, "the files need %u KB, more than the %u KB of a"
                   " 1.44 MB floppy; use a CD-ROM drive", (next - 2) / 2,
                   FAT_CLUSTERS / 2);
        return false;
    }
    fat_boot(meta);
    fat12_set(fat, 0, 0xff0);
    fat12_set(fat, 1, 0xfff);
    fat_chains(root, fat);
    memcpy(fat + FAT_SPF * FAT_SECTOR, fat, FAT_SPF * FAT_SECTOR);
    for (i = 0; i < root->kids->len; i++) {
        ImgNode *k = g_ptr_array_index(root->kids, i);

        fat_dirent(top + i * 32, k->id, k->dir, k->start, k->size, k->mtime,
                   fat_nt_case(k));
    }
    if (ftruncate(fd, FAT_TOTAL * FAT_SECTOR) < 0) {
        error_setg_errno(errp, errno, "cannot write the image");
        return false;
    }
    return img_pwrite(fd, meta, FAT_DATA * FAT_SECTOR, 0, errp) &&
           fat_write_dir(fd, root, errp);
}

bool mcp_image_make(int fd, bool disc, strList *files, McpImageInfo *info,
                    Error **errp)
{
    ImgNode *root = img_node_new(NULL, NULL, true);
    strList *l;
    int entries = 0;
    bool ok = false;

    root->mtime = time(NULL);
    for (l = files; l; l = l->next) {
        if (!img_add(root, l->value, 1, info, &entries, errp)) {
            goto out;
        }
    }
    img_name_tree(root, "", !disc, info);
    if (info->n_renamed > IMG_RENAMED_MAX) {
        g_string_append_printf(info->renamed, ", and %d more",
                               info->n_renamed - IMG_RENAMED_MAX);
    }
    ok = disc ? iso_write(fd, root, errp) : fat_write(fd, root, errp);
out:
    img_node_free(root);
    return ok;
}


/* Copy-out: the files of a FAT12 or FAT16 medium */

typedef struct FatReader {
    McpImageRead *read;
    void *opaque;
    McpImageInfo *info;
    uint32_t bps;                       /* bytes per sector */
    uint32_t cluster_size;
    uint32_t clusters;
    uint32_t data_sector;
    int bits;
    uint8_t *fat;
    GHashTable *seen;                   /* folders walked, by first cluster */
    /* bytes left to read: a damaged FAT can share clusters many times */
    uint64_t budget;
} FatReader;

static uint32_t fat_next(FatReader *r, uint32_t c)
{
    if (r->bits == 12) {
        uint16_t v = lduw_le_p(r->fat + c * 3 / 2);

        return c & 1 ? v >> 4 : v & 0xfff;
    }
    return lduw_le_p(r->fat + c * 2);
}

static bool fat_valid(FatReader *r, uint32_t c)
{
    return c >= 2 && c < r->clusters + 2;
}

/* Read @len bytes of cluster @c, which count against the budget */
static bool fat_read_cluster(FatReader *r, uint32_t c, void *buf,
                             uint32_t len, Error **errp)
{
    if (len > r->budget) {
        error_setg(errp, "the files on the floppy add up to more than it"
                   " holds, so its FAT is damaged; let the guest check it");
        return false;
    }
    r->budget -= len;
    return r->read(r->opaque, (uint64_t)r->data_sector * r->bps +
                   (uint64_t)(c - 2) * r->cluster_size, buf, len, errp);
}

/* The clusters of a folder from @c on, joined */
static uint8_t *fat_read_dir(FatReader *r, uint32_t c, size_t *len,
                             Error **errp)
{
    g_autoptr(GByteArray) a = g_byte_array_new();
    g_autofree uint8_t *buf = g_malloc(r->cluster_size);

    while (fat_valid(r, c)) {
        if (a->len >= FAT_DIR_MAX * 32) {
            error_setg(errp, "a folder on the floppy has more than %d"
                       " entries, so its FAT is damaged; let the guest"
                       " check it", FAT_DIR_MAX);
            return NULL;
        }
        if (!fat_read_cluster(r, c, buf, r->cluster_size, errp)) {
            return NULL;
        }
        g_byte_array_append(a, buf, r->cluster_size);
        c = fat_next(r, c);
    }
    *len = a->len;
    return g_byte_array_free(g_steal_pointer(&a), false);
}

static bool fat_copy_file(FatReader *r, uint32_t c, uint32_t size,
                          const char *path, Error **errp)
{
    g_autofree uint8_t *buf = g_malloc(r->cluster_size);
    uint32_t left = size;
    int fd = qemu_create(path, O_WRONLY | O_BINARY | O_TRUNC, 0644, errp);

    if (fd < 0) {
        return false;
    }
    while (left && fat_valid(r, c)) {
        uint32_t n = MIN(left, r->cluster_size);

        if (!fat_read_cluster(r, c, buf, n, errp)) {
            qemu_close(fd);
            return false;
        }
        if (qemu_write_full(fd, buf, n) != n) {
            error_setg_errno(errp, errno, "cannot write '%s'", path);
            qemu_close(fd);
            return false;
        }
        left -= n;
        c = fat_next(r, c);
    }
    qemu_close(fd);
    if (left) {
        img_skip(r->info, path, "it ends early on the floppy");
    }
    r->info->bytes += size - left;
    return true;
}

static uint8_t fat_lfn_sum(const uint8_t *e)
{
    uint8_t s = 0;
    int i;

    for (i = 0; i < 11; i++) {
        s = ((s & 1) << 7) + (s >> 1) + e[i];
    }
    return s;
}

/* A host name for an entry: no '/', no control characters */
static char *fat_clean(char *s)
{
    char *p;

    for (p = s; *p; p++) {
        if (*p == '/' || *p == '\\' || (uint8_t)*p < 0x20 || *p == 0x7f) {
            *p = '_';
        }
    }
    return s;
}

/* The 8.3 name of @e, in lower case where Windows NT says so */
static char *fat_short_name(const uint8_t *e)
{
    char base[9], ext[4];
    int i, nb = 8, ne = 3;

    for (i = 0; i < 8; i++) {
        base[i] = e[i] >= 0x80 ? '_' : e[12] & 0x08 ? g_ascii_tolower(e[i])
                                                    : e[i];
    }
    for (i = 0; i < 3; i++) {
        ext[i] = e[8 + i] >= 0x80 ? '_' : e[12] & 0x10
                                        ? g_ascii_tolower(e[8 + i])
                                        : e[8 + i];
    }
    if (e[0] == 0x05) {
        base[0] = '_';                  /* an 0xe5 that is not "deleted" */
    }
    while (nb && base[nb - 1] == ' ') {
        nb--;
    }
    while (ne && ext[ne - 1] == ' ') {
        ne--;
    }
    return fat_clean(ne ? g_strdup_printf("%.*s.%.*s", nb, base, ne, ext)
                        : g_strndup(base, nb));
}

/* Copy the @n entries at @ents, a folder of the floppy, into @dir */
static bool fat_walk(FatReader *r, const uint8_t *ents, size_t n,
                     const char *dir, int depth, Error **errp)
{
    gunichar2 lfn[20 * 13 + 1];
    uint32_t lfn_seen = 0, lfn_want = 0;
    int lfn_sum = -1;
    size_t i;

    if (depth > FAT_MAX_DEPTH) {
        error_setg(errp, "the folders on the floppy are more than %d deep",
                   FAT_MAX_DEPTH);
        return false;
    }
    for (i = 0; i < n; i++) {
        const uint8_t *e = ents + i * 32;
        uint8_t attr = e[11];
        g_autofree char *name = NULL;
        g_autofree char *path = NULL;
        uint32_t start = lduw_le_p(e + 26);

        if (e[0] == 0) {
            break;
        }
        if (e[0] == 0xe5) {
            lfn_sum = -1;
            continue;
        }
        if ((attr & 0x3f) == 0x0f) {
            /* a part of a long name: 13 UCS-2 characters */
            static const uint8_t at[13] = {
                1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
            };
            int seq = e[0] & 0x1f, k;

            if (e[0] & 0x40) {
                lfn_sum = e[13];
                lfn_want = seq >= 1 && seq <= 20 ? (1u << (seq + 1)) - 2 : 0;
                lfn_seen = 0;
                memset(lfn, 0, sizeof(lfn));
            }
            if (lfn_sum != e[13] || seq < 1 || seq > 20) {
                lfn_sum = -1;
                continue;
            }
            for (k = 0; k < 13; k++) {
                lfn[(seq - 1) * 13 + k] = lduw_le_p(e + at[k]);
            }
            lfn_seen |= 1u << seq;
            continue;
        }
        if (attr & 0x08) {
            lfn_sum = -1;               /* the volume label */
            continue;
        }
        if (lfn_sum == fat_lfn_sum(e) && lfn_want && lfn_seen == lfn_want) {
            int k;

            for (k = 0; k < 20 * 13 && lfn[k] && lfn[k] != 0xffff; k++) {
                /* up to the end of the name */
            }
            name = g_utf16_to_utf8(lfn, k, NULL, NULL, NULL);
            if (name && strlen(name) > 255) {
                g_clear_pointer(&name, g_free);     /* too long for the host */
            }
            if (name) {
                fat_clean(name);
            }
        }
        lfn_sum = -1;
        if (!name) {
            name = fat_short_name(e);
        }
        if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) {
            continue;
        }
        path = g_build_filename(dir, name, NULL);
        if (attr & 0x10) {
            g_autofree uint8_t *sub = NULL;
            size_t len;

            if (!g_hash_table_add(r->seen, GUINT_TO_POINTER(start))) {
                img_skip(r->info, path, "a folder seen before; the FAT is"
                         " damaged");
                continue;
            }
            if (g_mkdir_with_parents(path, 0755) < 0) {
                error_setg_errno(errp, errno, "cannot make folder '%s'", path);
                return false;
            }
            sub = fat_read_dir(r, start, &len, errp);
            if (!sub || !fat_walk(r, sub, len / 32, path, depth + 1, errp)) {
                return false;
            }
            r->info->folders++;
        } else {
            if (!fat_copy_file(r, start, ldl_le_p(e + 28), path, errp)) {
                return false;
            }
            r->info->files++;
        }
    }
    return true;
}

bool mcp_image_copy_out(McpImageRead *read, void *opaque, uint64_t size,
                        const char *dir, McpImageInfo *info, Error **errp)
{
    FatReader r = { .read = read, .opaque = opaque, .info = info };
    g_autoptr(GHashTable) seen = g_hash_table_new(NULL, NULL);
    g_autofree uint8_t *fat = NULL;
    g_autofree uint8_t *top = NULL;
    uint8_t b[512];
    uint32_t spc, reserved, nfats, entries, total, spf, top_sector;
    uint64_t fat_need;

    if (size < sizeof(b)) {
        error_setg(errp, "the medium is too small for a FAT file system");
        return false;
    }
    if (!read(opaque, 0, b, sizeof(b), errp)) {
        return false;
    }
    r.bps = lduw_le_p(b + 11);
    spc = b[13];
    reserved = lduw_le_p(b + 14);
    nfats = b[16];
    entries = lduw_le_p(b + 17);
    total = lduw_le_p(b + 19) ? lduw_le_p(b + 19) : ldl_le_p(b + 32);
    spf = lduw_le_p(b + 22);
    if ((r.bps != 512 && r.bps != 1024 && r.bps != 2048 && r.bps != 4096) ||
        !spc || (spc & (spc - 1)) || !reserved || !nfats || !spf ||
        !entries || !total) {
        error_setg(errp, "the medium has no FAT12 or FAT16 file system; has"
                   " the guest formatted it?");
        return false;
    }
    r.cluster_size = spc * r.bps;
    top_sector = reserved + nfats * spf;
    r.data_sector = top_sector + DIV_ROUND_UP(entries * 32, r.bps);
    if (r.data_sector >= total || (uint64_t)total * r.bps > size) {
        error_setg(errp, "the FAT file system is bigger than the medium");
        return false;
    }
    r.clusters = (total - r.data_sector) / spc;
    if (r.clusters >= 65525) {
        error_setg(errp, "the medium has FAT32, which lemu does not read");
        return false;
    }
    r.bits = r.clusters < 4085 ? 12 : 16;
    /* the FAT up to its last cluster, and the 2 bytes fat_next() reads */
    fat_need = ((uint64_t)(r.clusters + 2) * r.bits + 7) / 8 + 2;
    if ((uint64_t)spf * r.bps < fat_need) {
        error_setg(errp, "the FAT is too small for the medium");
        return false;
    }
    fat = g_malloc(fat_need);
    top = g_malloc(entries * 32);
    if (!read(opaque, (uint64_t)reserved * r.bps, fat, fat_need, errp) ||
        !read(opaque, (uint64_t)top_sector * r.bps, top, entries * 32,
              errp)) {
        return false;
    }
    r.fat = fat;
    r.seen = seen;
    r.budget = (uint64_t)r.clusters * r.cluster_size;
    if (g_mkdir_with_parents(dir, 0755) < 0) {
        error_setg_errno(errp, errno, "cannot make folder '%s'", dir);
        return false;
    }
    return fat_walk(&r, top, entries, dir, 0, errp);
}
