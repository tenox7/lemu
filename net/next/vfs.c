/*
 * Virtual File System
 * 
 * Created by Simon Schubiger
 * Rewritten in C by Andreas Grabher
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "nextnet.h"
#include "vfs.h"

#ifdef _WIN32
#define HOST_SEPARATOR "\\"
#else
#define HOST_SEPARATOR "/"
#endif


/* ----- Helpers */
size_t vfscpy(char* dst, const char* src, size_t size) {
    size_t srclen, retval;
    
    srclen = strlen(src);
    retval = srclen;
    
    if (size > 0) {
        if (srclen >= size) {
            srclen = size - 1;
        }
        memcpy(dst, src, srclen);
        dst[srclen] = '\0';
    }
    return retval;
}

size_t vfscat(char* dst, const char* src, size_t size) {
    size_t dstlen, srclen, retval;
    
    dstlen = strlen(dst);
    srclen = strlen(src);
    retval = dstlen + srclen;
    
    if (size > 0) {
        if (dstlen >= size) {
            dstlen = size - 1;
        }
        if (srclen >= size - dstlen) {
            srclen = size - dstlen - 1;
        }
        memcpy(dst + dstlen, src, srclen);
        dst[dstlen + srclen] = '\0';
    }
    return retval;
}

size_t vfs_join(char* path, const char* name, size_t size) {
    int needsep;
    size_t pathlen, namelen, retval;
    
    pathlen = strlen(path);
    namelen = strlen(name);
    needsep = pathlen && namelen && path[pathlen - 1] != '/';
    retval  = pathlen + namelen + needsep;

    if (size > pathlen + needsep + 1) {
        if (needsep) {
            path[pathlen] = '/';
            pathlen += needsep;
        }
        if (namelen >= size - pathlen) {
            namelen = size - pathlen - 1;
        }
        memcpy(path + pathlen, name, namelen);
        path[pathlen + namelen] = '\0';
    }
    return retval;
}

/* ----- VFS and host path */
void vfs_path_canonicalize(char* vfs_path) {
    char* vfsPath = vfs_path;
    char* slashslashptr;
    char* dotdotptr;
    char* slashdotptr;
    char* slashptr;
    
    /* step 1 : replace '//' sequences by a single '/' */
    slashslashptr = vfsPath;
    while(1) {
        slashslashptr = strstr(slashslashptr,"//");
        if(NULL == slashslashptr)
            break;
        /* remove extra '/' */
        memmove(slashslashptr,slashslashptr+1,strlen(slashslashptr+1)+1);
    }
    
    /* step 2 : replace '/./' sequences by a single '/' */
    slashdotptr = vfsPath;
    while(1) {
        slashdotptr = strstr(slashdotptr,"/./");
        if(NULL == slashdotptr)
            break;
        /* strip the extra '/.' */
        memmove(slashdotptr,slashdotptr+2,strlen(slashdotptr+2)+1);
    }
    
    /* step 3 : replace '/<name>/../' sequences by a single '/' */
    
    while(1) {
        dotdotptr = strstr(vfsPath,"/../");
        if(NULL == dotdotptr)
            break;
        if(dotdotptr == vfsPath) {
            /* special case : '/../' at the beginning of the path are replaced
             by a single '/' */
            memmove(vfsPath, vfsPath+3,strlen(vfsPath+3)+1);
            continue;
        }
        
        /* null-terminate the string before the '/../', so that strrchr will
         start looking right before it */
        *dotdotptr = '\0';
        slashptr = strrchr(vfsPath,'/');
        if(NULL == slashptr) {
            /* this happens if this function was called with a relative path.
             don't do that. */
            nn_log("VFS: no '/' before '/../' in '%s'\n", vfsPath);
            break;
        }
        memmove(slashptr,dotdotptr+3,strlen(dotdotptr+3)+1);
    }
    
    /* step 4 : remove a trailing '/..' */
    
    dotdotptr = strstr(vfsPath,"/..");
    if(dotdotptr == vfsPath)
    /* if the full path is simply '/..', replace it by '/' */
        vfsPath[1] = '\0';
    else if(NULL != dotdotptr && '\0' == dotdotptr[3]) {
        *dotdotptr = '\0';
        slashptr = strrchr(vfsPath,'/');
        if(NULL != slashptr) {
            /* make sure the last slash isn't the root */
            if (slashptr == vfsPath)
                vfsPath[1] = '\0';
            else
                *slashptr = '\0';
        }
    }
    
    /* step 5 : remove a trailing '/.' */
    slashdotptr = vfsPath;
    while(1) {
        slashdotptr = strstr(slashdotptr,"/.");
        if (NULL == slashdotptr)
            break;
        if (slashdotptr[2] == '\0') {
            if(slashdotptr == vfsPath)
            /* if the full path is simply '/.', replace it by '/' */
                vfsPath[1] = '\0';
            else
                *slashdotptr = '\0';
            break;
        } else {
            /* if '/.' is in the middle of the path as with invisible files,
             skip it and continue */
            slashdotptr += 2;
        }
    }
    
    /*
     * No '.' or '..' should be left; if one is, make_host_path() refuses
     * the path rather than QEMU stopping on an assert
     */
}

static void vfs_get_parent_path(const char* vfs_path, char* parent_path) {
    char* sep;
    strcpy(parent_path, vfs_path);
    sep = strrchr(parent_path, '/');
    if (sep == parent_path) sep[1] = '\0'; /* root directory */
    else if (sep)           sep[0] = '\0';
    else            parent_path[0] = '\0';
}

static int vfs_path_is_absolute(const char* path) {
    return strlen(path) > 0 && path[0] == '/';
}

static char* path_relative(char* path, const char* base_path) {
    if (strstr(path, base_path) == path) {
        return path+strlen(base_path);
    } else {
        return path;
    }
}

/* The shared folder on the host: every host path must be in it */
static char vfs_host_base[FILENAME_MAX];

/*
 * A name that stays in its folder on the host: not '.' or '..', and on
 * Windows no separator, drive or stream, and no device name
 */
static bool vfs_name_ok(const char* name) {
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) {
        return false;
    }
#ifdef _WIN32
    static const char* const devices[] = {
        "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    size_t stem = strcspn(name, ". ");
    size_t i;

    for (i = 0; name[i]; i++) {
        if ((unsigned char)name[i] < 32 || strchr("\\:*?\"<>|", name[i])) {
            return false;
        }
    }
    /* Windows drops dots and spaces at the end: "..." would be ".." */
    if (name[i - 1] == '.' || name[i - 1] == ' ') {
        return false;
    }
    for (i = 0; i < G_N_ELEMENTS(devices); i++) {
        if (strlen(devices[i]) == stem &&
            !g_ascii_strncasecmp(name, devices[i], stem)) {
            return false;
        }
    }
#endif
    return true;
}

static size_t make_host_path(const char* host_base, char* vfs_path, char* host_path) {
    char* p;
    size_t len;
        
    if (strchr(vfs_path, '/') == vfs_path) {
        vfs_path++;  /* skip leading '/' */
    }
    if ((p = strrchr(vfs_path, '/')) && p[1] == '\0') {
        p[0] = '\0'; /* remove trailing '/' */
    }
    
    vfscpy(host_path, host_base, FILENAME_MAX);
    
    if (vfs_path[0] != '\0') {
        vfscat(host_path, HOST_SEPARATOR, FILENAME_MAX);
    }
    
    while ((p = strchr(vfs_path, '/'))) {
        p[0] = '\0';
        if (!vfs_name_ok(vfs_path)) {
            goto bad;
        }
        vfscat(host_path, vfs_path, FILENAME_MAX);
        vfscat(host_path, HOST_SEPARATOR, FILENAME_MAX);
        vfs_path = p + 1;
    }
    if (vfs_path[0] && !vfs_name_ok(vfs_path)) {
        goto bad;
    }
    len = vfscat(host_path, vfs_path, FILENAME_MAX);
    if (len >= FILENAME_MAX) {
        /* cut short it would name another file */
        host_path[0] = '\0';
    }
    return len;

bad:
    nn_log("VFS: refused name '%s'\n", vfs_path);
    host_path[0] = '\0';
    return 0;
}

size_t vfs_to_host_path(struct vfs_t* vfs, struct path_t* path) {
    char vfs_path[MAXPATHLEN];
    
    if (!vfs_path_is_absolute(path->vfs)) {
        nn_log("path is not absolute\n");
        strcpy(path->host, "");
        return 0;
    }
    strcpy(vfs_path, "/");
    vfscat(vfs_path, path_relative(path->vfs, vfs->base_path.vfs), sizeof(vfs_path));
    vfs_path_canonicalize(vfs_path);
    
    return make_host_path(vfs->base_path.host, vfs_path, path->host);
}

static size_t make_vfs_path(const char* vfs_base, char* host_path, char* vfs_path, int relative) {
    char* p;
    
    if (relative) {
        vfs_path[0] = '\0';
    } else {
        size_t len = vfscpy(vfs_path, vfs_base, MAXPATHLEN);
        if (len > 0 && len < MAXPATHLEN && vfs_path[len - 1] != '/') {
            vfscat(vfs_path, "/", MAXPATHLEN);
        }
        if (strncmp(host_path, HOST_SEPARATOR, strlen(HOST_SEPARATOR)) == 0) {
            host_path += strlen(HOST_SEPARATOR); /* skip leading separator */
        }
    }
    
    while ((p = strstr(host_path, HOST_SEPARATOR))) {
        p[0] = '\0';
        vfscat(vfs_path, host_path, MAXPATHLEN);
        vfscat(vfs_path, "/", MAXPATHLEN);
        host_path = p + strlen(HOST_SEPARATOR);
    }
    return vfscat(vfs_path, host_path, MAXPATHLEN);
}

size_t vfs_to_vfs_path(struct vfs_t* vfs, struct path_t* path) {
    char* host_path = path_relative(path->host, vfs->base_path.host);
    
    return make_vfs_path(vfs->base_path.vfs, host_path, path->vfs, host_path == path->host);
}

/* Is it a link: a symbolic link, or on Windows any reparse point? */
static int host_is_link(const char* host_path, int* exists) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(host_path);

    *exists = attr != INVALID_FILE_ATTRIBUTES;
    return *exists && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat fstat;

    *exists = lstat(host_path, &fstat) == 0;
    return *exists && S_ISLNK(fstat.st_mode);
#endif
}

/*
 * The guest makes links with any target, so the host must never follow
 * one: a path is good when it is in the shared folder and none of its
 * folders is a link. With follow, the last part may not be a link
 * either, as the call would follow it. Links are for the guest to
 * follow, by READLINK.
 */
static int vfs_check(const struct path_t* path, int follow) {
    char buf[FILENAME_MAX];
    size_t base = strlen(vfs_host_base);
    size_t len = strlen(path->host);
    size_t i = base;

    if (!base || len >= sizeof(buf) || strncmp(path->host, vfs_host_base, base) ||
        (path->host[base] && strncmp(path->host + base, HOST_SEPARATOR,
                                     strlen(HOST_SEPARATOR)))) {
        return EACCES;
    }
    memcpy(buf, path->host, len + 1);
    while (i < len) {
        char* sep = strstr(buf + i + 1, HOST_SEPARATOR);
        size_t end = sep ? (size_t)(sep - buf) : len;
        char c = buf[end];
        int exists, link;

        buf[end] = '\0';
        link = host_is_link(buf, &exists);
        buf[end] = c;
        if (!exists) {
            return 0;   /* nothing after this exists either */
        }
        if (link && (end < len || follow)) {
            return ELOOP;
        }
        i = end;
    }
    return 0;
}

static int host_path_is_directory(const struct path_t* path) {
    struct stat fstat;
    if (vfs_check(path, 1) == 0 && stat(path->host, &fstat) == 0)
        return S_ISDIR(fstat.st_mode);
    return 0;
}


/*----- file io */
struct file_t {
    struct stat fstat;
    int restore_stat;
    FILE* file;
};

/* fopen(), but never through a link */
static FILE* host_fopen(const struct path_t* path, const char* mode) {
#ifdef _WIN32
    return fopen(path->host, mode);
#else
    int flags, fd;
    FILE* f;

    if (!strcmp(mode, "rb")) {
        flags = O_RDONLY;
    } else if (!strcmp(mode, "r+b")) {
        flags = O_RDWR;
    } else {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    }
    fd = open(path->host, flags | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (fd < 0) {
        return NULL;
    }
    f = fdopen(fd, mode);
    if (!f) {
        close(fd);
    }
    return f;
#endif
}

static struct file_t* file_open(const struct path_t* path, const char* mode) {
    struct file_t* file = (struct file_t*)g_malloc(sizeof(struct file_t));
    int err = vfs_check(path, 1);
    
    file->restore_stat = 0;
    if (err) {
        file->file = NULL;
        errno = err;
        return file;
    }
    file->file = host_fopen(path, mode);
    if (file->file == NULL && errno == EACCES) {
        file->restore_stat = 1;
        vfs_stat(path, &file->fstat);
        vfs_chmod(path, file->fstat.st_mode);
        file->file = host_fopen(path, mode);
    }
    return file;
}

static void file_close(const struct path_t* path, struct file_t* file) {
    if (file->restore_stat) {
        vfs_chmod(path, file->fstat.st_mode);
        struct timeval times[2];
#ifdef _WIN32
        times[0].tv_sec  = file->fstat.st_atime;
        times[0].tv_usec = 0;
        times[1].tv_sec  = file->fstat.st_mtime;
        times[1].tv_usec = 0;
#else
        times[0].tv_sec  = file->fstat.st_atimespec.tv_sec;
        times[0].tv_usec = (int32_t)(file->fstat.st_atimespec.tv_nsec / 1000);
        times[1].tv_sec  = file->fstat.st_mtimespec.tv_sec;
        times[1].tv_usec = (int32_t)(file->fstat.st_mtimespec.tv_nsec / 1000);
#endif
        vfs_utimes(path, times);
    }
    if (file->file) fclose(file->file);
    g_free(file);
}

static int file_is_open(struct file_t* file) {
    return file->file == NULL ? errno : 0;
}

static int file_seek(struct file_t* file, long offset) {
    return fseek(file->file, offset, SEEK_SET) < 0 ? errno : 0;
}

static size_t file_read(struct file_t* file, void* dst, size_t count) {
    return fread(dst, sizeof(uint8_t), count, file->file);
}

static size_t file_write(struct file_t* file, void* src, size_t count) {
    return fwrite(src, sizeof(uint8_t), count, file->file);
}

/*----- file attributes */
int valid32(uint32_t statval) { return statval != 0xFFFFFFFF; }
int valid16(uint32_t statval) { return (statval & 0x0000FFFF) != 0x0000FFFF; }

uint32_t vfs_file_id(uint64_t ino) {
    uint32_t result = (uint32_t)ino;
    return (result ^ (ino >> 32)) & 0x7FFFFFFF;
}

uint32_t vfs_get_parent_gid(struct vfs_t* vfs, const struct path_t* path) {
    struct path_t parent_path;
    
    if (vfs_path_is_absolute(path->vfs)) {
        vfs_get_parent_path(path->vfs, parent_path.vfs);
        vfs_to_host_path(vfs, &parent_path);
        if (host_path_is_directory(&parent_path)) {
            struct sattr_t sattr;
            vfs_get_sattr(vfs, &parent_path, &sattr);
            return sattr.gid;
        }
    }
    return vfs->gid;
}

int vfs_get_fstat(struct vfs_t* vfs, const struct path_t* path, struct stat* fstat) {
    struct sattr_t sattr;
    int result;
    
    result = vfs_stat(path, fstat);
    vfs_get_sattr(vfs, path, &sattr);
    
    if (valid16(sattr.mode)) {
        /* copy permissions from attributes */
        fstat->st_mode &= S_IFMT;
        fstat->st_mode |= sattr.mode & ~S_IFMT; 
        /* mode heuristics: if file is empty we map it to the various special formats (CHAR, BLOCK, FIFO, etc.) from stored attributes */
        if (S_ISREG(fstat->st_mode) && fstat->st_size == 0 && (sattr.mode & S_IFMT)) {
            fstat->st_mode &= ~S_IFMT;             /* clear format */
            fstat->st_mode |= sattr.mode & S_IFMT; /* copy format from attributes */
        }
    }
    fstat->st_uid  = valid16(sattr.uid)  ? sattr.uid  : fstat->st_uid;
    fstat->st_gid  = valid16(sattr.gid)  ? sattr.gid  : fstat->st_gid;
    fstat->st_rdev = valid16(sattr.rdev) ? sattr.rdev : fstat->st_rdev;
    
    return result;
}

void vfs_stat_to_sattr(const struct stat* stat, struct sattr_t* sattr) {
    sattr->mode       = (uint32_t)stat->st_mode;
    sattr->uid        = (uint32_t)stat->st_uid;
    sattr->gid        = (uint32_t)stat->st_gid;
    sattr->size       = (uint32_t)stat->st_size;
#ifdef _WIN32
    sattr->atime.sec  = (uint32_t)stat->st_atime;
    sattr->atime.usec = 0;
    sattr->mtime.sec  = (uint32_t)stat->st_mtime;
    sattr->mtime.usec = 0;
#else
    sattr->atime.sec  = (uint32_t)stat->st_atimespec.tv_sec;
    sattr->atime.usec = (uint32_t)stat->st_atimespec.tv_nsec / 1000;
    sattr->mtime.sec  = (uint32_t)stat->st_mtimespec.tv_sec;
    sattr->mtime.usec = (uint32_t)stat->st_mtimespec.tv_nsec / 1000;
#endif
    sattr->rdev       = (uint32_t)stat->st_rdev;
}

static int get_error(int result) {
    return result < 0 ? errno : result;
}

int vfs_chmod(const struct path_t* path, mode_t mode) {
    int err = vfs_check(path, 0);

    if (err) {
        return err;
    }
#ifdef _WIN32
    return 0; /* not supported */
#else
    return get_error(fchmodat(AT_FDCWD, path->host, mode | S_IWUSR | S_IRUSR, AT_SYMLINK_NOFOLLOW));
#endif
}

int vfs_utimes(const struct path_t* path, struct timeval times[2]) {
    int err = vfs_check(path, 0);

    if (err) {
        return err;
    }
#ifdef _WIN32
    return 0; /* not supported */
#else
    return get_error(lutimes(path->host, times));
#endif
}

int vfs_stat(const struct path_t* path, struct stat* fstat) {
    int err = vfs_check(path, 0);

    if (err) {
        return err;
    }
#ifdef _WIN32
    return get_error(stat(path->host, fstat));
#else
    return get_error(lstat(path->host, fstat));
#endif
}

#if HAVE_SYS_XATTR_H
/* Linux takes only names with a namespace for user attributes */
#if defined(__linux__)
static const char* NFSD_ATTRS = "user.nfsd_fattrs";
#else
static const char* NFSD_ATTRS = ".nfsd_fattrs";
#endif

static void deserialize(const char* buffer, struct sattr_t* sattr) {
    sscanf(buffer, "0%o:%d:%d:%d", &sattr->mode, &sattr->uid, &sattr->gid, &sattr->rdev);
}

static void serialize(const struct sattr_t* sattr, char* buffer) {
    snprintf(buffer, 128, "0%o:%d:%d:%d", sattr->mode, sattr->uid, sattr->gid, sattr->rdev);
}

static const char* vfs_get_filename(const char* vfs_path) {
    const char* sep = strrchr(vfs_path, '/');
    return sep ? (sep + 1) : vfs_path;
}
#endif

void vfs_set_sattr(struct vfs_t* vfs, const struct path_t* path, struct sattr_t* sattr) {
#if HAVE_SYS_XATTR_H
    char buffer[128];

    if (vfs_check(path, 0)) {
        return;
    }
    const char* fname = vfs_get_filename(path->vfs);
    
    /* the attributes of '.' or '..' would go to another file */
    if (!strcmp(fname, ".") || !strcmp(fname, "..")) {
        nn_log("VFS: no attributes for '%s'\n", path->vfs);
        return;
    }
    
    serialize(sattr, buffer);
#if HAVE_LXETXATTR
    if (lsetxattr(path->host, NFSD_ATTRS, buffer, strlen(buffer), 0) != 0)
#else
    if (setxattr(path->host, NFSD_ATTRS, buffer, strlen(buffer), 0, XATTR_NOFOLLOW) != 0)
#endif
        nn_log("setxattr(%s) failed\n", path->host);
#endif
}

void vfs_get_sattr(struct vfs_t* vfs, const struct path_t* path, struct sattr_t* sattr) {
#if HAVE_SYS_XATTR_H
    char buffer[128];
    memset(buffer, 0, sizeof(buffer));
#if HAVE_LXETXATTR
    if (vfs_check(path, 0) == 0 && lgetxattr(path->host, NFSD_ATTRS, buffer, sizeof(buffer) - 1) > 0)
#else
    if (vfs_check(path, 0) == 0 &&
        getxattr(path->host, NFSD_ATTRS, buffer, sizeof(buffer), 0, XATTR_NOFOLLOW) > 0)
#endif
        deserialize(buffer, sattr);
    else
#endif
    {
        struct stat fstat;

        memset(&fstat, 0, sizeof(fstat));
        vfs_stat(path, &fstat);
        fstat.st_uid = vfs->uid;
        fstat.st_gid = vfs->gid;
        fstat.st_mode |= S_ISDIR(fstat.st_mode) ? 0755 : 0644; /* strcmp(vfs->base_path.vfs, path->vfs) ? 0 : 0755; */
        vfs_stat_to_sattr(&fstat, sattr);
    }
}

/* ----- file handle */
#ifndef _WIN32
static uint64_t rotl(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

static uint64_t make_file_handle(struct stat* fstat) {
    uint64_t result = fstat->st_dev;
    result = rotl(result, 32) ^ fstat->st_ino;
    if (result == 0) result = ~result;
    return result;
}
#endif

uint64_t vfs_get_fhandle(const struct path_t* path) {
    struct stat fstat;
    uint64_t result = 0;
    
    if (vfs_stat(path, &fstat) == 0) {
#ifndef _WIN32
        result = make_file_handle(&fstat);
#else
        HANDLE fhandle;
        fhandle = CreateFileA(path->host,
                              GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (fhandle) {
            BY_HANDLE_FILE_INFORMATION finfo;
            if (GetFileInformationByHandle(fhandle, &finfo)) {
                result = ((uint64_t)(finfo.nFileIndexHigh) << 32) | (uint64_t)(finfo.nFileIndexLow);
            }
            CloseHandle(fhandle);
        }
#endif
    } else {
        nn_log("No file handle for %s\n", path->vfs);
    }
    
    return result;
}

/* ----- VFS functions */
int vfs_readlink(const struct path_t* path, struct path_t* result) {
#ifdef _WIN32
    return EACCES; /* not supported */
#else
    struct stat sb;
    size_t  bufsiz;
    ssize_t nbytes;
    int err = vfs_check(path, 0);
    
    if (err)
        return err;
    if (lstat(path->host, &sb) == -1)
        return errno;
    
    /* Add one to the link size, so that we can determine whether
     the buffer returned by readlink() was truncated. */
    
    bufsiz = sb.st_size + 1;
    
    /* Some magic symlinks under (for example) /proc and /sys
     report 'st_size' as zero. In that case, take PATH_MAX as
     a "good enough" estimate. */
    
    if (sb.st_size == 0)
        bufsiz = sizeof(result->host);
    
    if (bufsiz > sizeof(result->host))
        return ENAMETOOLONG;
    
    nbytes = readlink(path->host, result->host, bufsiz - 1);
    if (nbytes == -1)
        return errno;
    
    result->host[nbytes] = '\0';
    
    return 0;
#endif
}

int vfs_read(const struct path_t* path, uint32_t offset, uint8_t* data, uint32_t* len) {
    int err = 0;
    struct file_t* file = file_open(path, "rb");
    err = file_is_open(file);
    if (err == 0) {
        err = file_seek(file, offset);
        if (err == 0) {
            *len = (uint32_t)file_read(file, data, *len);
        }
    }
    file_close(path, file);
    return err;
}

int vfs_write(const struct path_t* path, uint32_t offset, uint8_t* data, uint32_t len) {
    int err;
    struct file_t* file = file_open(path, "r+b");
    err = file_is_open(file);
    if (err == 0) {
        err = file_seek(file, offset);
        if (err == 0) {
            err = (file_write(file, data, len) < len) ? ENOSPC : 0;
        }
    }
    file_close(path, file);
    return err;
}

int vfs_create(const struct path_t* path, uint8_t* data, uint32_t len) {
    int err = 0;
    struct file_t* file = file_open(path, "wb");
    err = file_is_open(file);
    if (data && err == 0) {
        err = (file_write(file, data, len) < len) ? ENOSPC : 0;
    }
    file_close(path, file);
    return err;
}

int vfs_remove(const struct path_t* path) {
    int err = vfs_check(path, 0);

    return err ? err : get_error(remove(path->host));
}

int vfs_rename(const struct path_t* path_from, const struct path_t* path_to) {
    int err = vfs_check(path_from, 0);

    if (!err) {
        err = vfs_check(path_to, 0);
    }
    return err ? err : get_error(rename(path_from->host, path_to->host));
}

int vfs_link(const struct path_t* path_from, const struct path_t* path_to, int soft) {
#ifdef _WIN32
    return EACCES; /* not supported */
#else
    int err = vfs_check(path_to, 0);

    if (err) {
        return err;
    }
    /* a symbolic link keeps what the guest gave: the host never follows it */
    if (soft) {
        return get_error(symlink(path_from->vfs, path_to->host));
    }
    err = vfs_check(path_from, 0);
    if (err) {
        return err;
    }
    /* a hard link to a symbolic link links the link, not its target */
    return get_error(linkat(AT_FDCWD, path_from->host, AT_FDCWD,
                            path_to->host, 0));
#endif
}

int vfs_mkdir(const struct path_t* path) {
    int err = vfs_check(path, 0);

    if (err) {
        return err;
    }
#ifdef _WIN32
    return get_error(mkdir(path->host));
#else
    return get_error(mkdir(path->host, DEFAULT_PERM));
#endif
}

int vfs_rmdir(const struct path_t* path) {
    int err = vfs_check(path, 0);

    return err ? err : get_error(rmdir(path->host));
}

DIR* vfs_opendir(const struct path_t* path) {
    int err = vfs_check(path, 1);

    if (err) {
        errno = err;
        return NULL;
    }
    return opendir(path->host);
}

int vfs_statfs(const struct path_t* path, struct statvfs* fsstat) {
    int err = vfs_check(path, 1);

    if (err) {
        return err;
    }
#ifndef _WIN32
    return get_error(statvfs(path->host, fsstat));
#else
    DWORD sectorsPerCluster, bytesPerSector, freeClusters, totalClusters;
    BOOL res = GetDiskFreeSpaceA(path->host, &sectorsPerCluster,
                                 &bytesPerSector, &freeClusters, &totalClusters);
    if (!res) {
        return GetLastError();
    }
    fsstat->f_bsize  = sectorsPerCluster * bytesPerSector; /* BLOCK_SIZE */
    fsstat->f_frsize = sectorsPerCluster * bytesPerSector;
    fsstat->f_blocks = totalClusters;
    fsstat->f_bfree  = freeClusters;
    fsstat->f_bavail = freeClusters;
    return 0;
#endif
}

int vfs_access(const struct path_t* path, int mode) {
    int err = vfs_check(path, 1);

    return err ? err : get_error(access(path->host, mode));
}

/*----- VFS */
struct vfs_t* vfs_init(const char* host_path, const char* vfs_path_alias) {
    struct vfs_t* vfs = NULL;
    if (host_path && vfs_path_alias && strlen(host_path) && strlen(vfs_path_alias)) {
        vfs = (struct vfs_t*)g_malloc(sizeof(struct vfs_t));
        if (vfs) {
            char* p;
            vfscpy(vfs->base_path.vfs, vfs_path_alias, sizeof(vfs->base_path.vfs));
            vfscpy(vfs->base_path.host, host_path, sizeof(vfs->base_path.host));
            vfs->uid = 20;
            vfs->gid = 20;
            /* Make sure there is no trailing separator in host path */
            p = vfs->base_path.host + strlen(vfs->base_path.host) - strlen(HOST_SEPARATOR);
            if (strcmp(p, HOST_SEPARATOR) == 0 && p != vfs->base_path.host) {
                p[0] = '\0';
            }
            vfscpy(vfs_host_base, vfs->base_path.host, sizeof(vfs_host_base));
        }
    }
    return vfs;
}

struct vfs_t* vfs_uninit(struct vfs_t* vfs) {
    vfs_host_base[0] = '\0';
    g_free(vfs);
    return NULL;
}

void vfs_set_process_uid_gid(struct vfs_t* vfs, uint32_t uid, uint32_t gid) {
    vfs->uid = uid;
    vfs->gid = gid;
}

void vfs_get_basepath_alias(struct vfs_t* vfs, char* path, size_t maxlen) {
    vfscpy(path, vfs->base_path.vfs, maxlen);
}
