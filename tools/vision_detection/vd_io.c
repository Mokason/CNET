#include "vd_io.h"
#include "vd_sha256.h"

#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int vd_path_ok(const char *path) {
    size_t n, i;
    if (!path) return -1;
    n = strlen(path);
    if (n == 0 || n >= VD_PATH_MAX) return -1;
    /* No shell ever sees these paths, but a metacharacter in an output path is
       a sign the caller is doing something unintended -- refuse rather than
       silently create a strangely named directory. */
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20 || c == 0x7f) return -1;
        if (strchr("*?[]{}()<>|&;$`'\"\\\n\t", (int)c)) return -1;
    }
    /* reject any ".." path component */
    for (i = 0; i < n; ) {
        size_t j = i;
        while (j < n && path[j] != '/') j++;
        if (j - i == 2 && path[i] == '.' && path[i + 1] == '.') return -1;
        i = (j < n) ? j + 1 : j;
    }
    return 0;
}

int vd_mkdir_p(const char *path) {
    char tmp[VD_PATH_MAX];
    size_t n, i;
    int depth = 0;
    struct stat st;
    if (vd_path_ok(path) != 0) return -1;
    n = strlen(path);
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;   /* trim trailing slashes */

    for (i = 1; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != 0) continue;
        if (++depth > VD_MKDIR_MAX_DEPTH) return -1;
        {
            char save = tmp[i];
            tmp[i] = 0;
            if (mkdir(tmp, 0777) != 0) {
                if (errno != EEXIST) { tmp[i] = save; return -1; }
                if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) { tmp[i] = save; return -1; }
            }
            tmp[i] = save;
        }
    }
    if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    return 0;
}

int vd_publish_file(const char *path, const void *buf, size_t len) {
    char tmp[VD_PATH_MAX], dir[VD_PATH_MAX];
    const char *slash;
    int fd, dfd;
    const unsigned char *p = (const unsigned char *)buf;
    size_t off = 0;

    if (vd_path_ok(path) != 0) return -1;
    if (!buf && len) return -1;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return -1;

    (void)unlink(tmp);   /* a leftover temp must not silently win */
    /* O_NOFOLLOW|O_EXCL: refuse to write through a symlink planted at the temp
       path, and refuse to reuse an existing one. */
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;

    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(tmp); return -1;
        }
        if (w == 0) { close(fd); unlink(tmp); return -1; }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }
    if (close(fd) != 0) { unlink(tmp); return -1; }

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }

    /* fsync the containing directory so the rename itself is durable */
    slash = strrchr(path, '/');
    if (slash) {
        size_t dn = (size_t)(slash - path);
        if (dn == 0) dn = 1;
        if (dn >= sizeof dir) return -1;
        memcpy(dir, path, dn);
        dir[dn] = 0;
    } else {
        dir[0] = '.'; dir[1] = 0;
    }
    dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { (void)fsync(dfd); close(dfd); }
    return 0;
}

/* ---------------- directory-atomic cache publication --------------------- */
#include <dirent.h>

#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif
static int do_renameat2(int ofd, const char *o, int nfd, const char *n, unsigned f) {
    return (int)syscall(SYS_renameat2, ofd, o, nfd, n, f);
}
#endif

static int rand_suffix(char *out, size_t n) {
    static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned char r[12];
    FILE *f = fopen("/dev/urandom", "rb");
    size_t i;
    if (!f) return -1;
    if (fread(r, 1, sizeof r, f) != sizeof r) { fclose(f); return -1; }
    fclose(f);
    if (n < sizeof r + 1) return -1;
    for (i = 0; i < sizeof r; i++) out[i] = cs[r[i] % (sizeof cs - 1)];
    out[sizeof r] = 0;
    return 0;
}

/* Flat directories only -- the cache has no subdirectories, and refusing to
   recurse keeps this from becoming a general-purpose deletion routine. */
static int rm_flat_dir_at(int parent_fd, const char *name) {
    int dfd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    DIR *d;
    struct dirent *e;
    if (dfd < 0) return -1;
    d = fdopendir(dfd);
    if (!d) { close(dfd); return -1; }
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (unlinkat(dirfd(d), e->d_name, 0) != 0) {
            if (unlinkat(dirfd(d), e->d_name, AT_REMOVEDIR) != 0) { closedir(d); return -1; }
        }
    }
    closedir(d);
    return unlinkat(parent_fd, name, AT_REMOVEDIR);
}

int vd_stage_begin(const char *dest, VdStage *st) {
    char tmp[VD_PATH_MAX], suffix[16];
    const char *slash;
    size_t n;
    struct stat sb;

    if (!st) return -1;
    memset(st, 0, sizeof *st);
    st->parent_fd = st->dir_fd = -1;
    if (vd_path_ok(dest) != 0) return -1;

    n = strlen(dest);
    memcpy(tmp, dest, n + 1);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    slash = strrchr(tmp, '/');
    if (slash) {
        char parent[VD_PATH_MAX];
        size_t pn = (size_t)(slash - tmp);
        if (pn == 0) pn = 1;
        memcpy(parent, tmp, pn);
        parent[pn] = 0;
        if (vd_mkdir_p(parent) != 0) return -1;
        /* O_NOFOLLOW here is what rejects a symlinked parent directory. */
        st->parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        snprintf(st->base, sizeof st->base, "%s", slash + 1);
    } else {
        st->parent_fd = open(".", O_RDONLY | O_DIRECTORY);
        snprintf(st->base, sizeof st->base, "%s", tmp);
    }
    if (st->parent_fd < 0) return -1;

    /* A destination that is itself a symlink is refused rather than replaced. */
    if (fstatat(st->parent_fd, st->base, &sb, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(sb.st_mode)) {
        close(st->parent_fd); st->parent_fd = -1;
        return -1;
    }
    if (rand_suffix(suffix, sizeof suffix) != 0) {
        close(st->parent_fd); st->parent_fd = -1; return -1;
    }
    if ((size_t)snprintf(st->stage, sizeof st->stage, "%s.stage.%s", st->base, suffix)
        >= sizeof st->stage) { close(st->parent_fd); st->parent_fd = -1; return -1; }

    if (mkdirat(st->parent_fd, st->stage, 0777) != 0) {   /* exclusive by definition */
        close(st->parent_fd); st->parent_fd = -1; return -1;
    }
    st->dir_fd = openat(st->parent_fd, st->stage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (st->dir_fd < 0) {
        (void)unlinkat(st->parent_fd, st->stage, AT_REMOVEDIR);
        close(st->parent_fd); st->parent_fd = -1; return -1;
    }
    return 0;
}

void vd_stage_abort(VdStage *st) {
    if (!st) return;
    if (st->dir_fd >= 0) { close(st->dir_fd); st->dir_fd = -1; }
    if (st->parent_fd >= 0) {
        if (!st->done) (void)rm_flat_dir_at(st->parent_fd, st->stage);
        close(st->parent_fd);
        st->parent_fd = -1;
    }
}

int vd_stage_commit(VdStage *st, int replace) {
    struct stat sb;
    int existed;
    if (!st || st->dir_fd < 0 || st->parent_fd < 0) return -1;
    if (fsync(st->dir_fd) != 0) return -1;
    close(st->dir_fd);
    st->dir_fd = -1;

    existed = (fstatat(st->parent_fd, st->base, &sb, AT_SYMLINK_NOFOLLOW) == 0);
    if (existed && S_ISLNK(sb.st_mode)) { vd_stage_abort(st); return -1; }

    if (!existed) {
        if (renameat(st->parent_fd, st->stage, st->parent_fd, st->base) != 0) {
            vd_stage_abort(st); return -1;
        }
    } else if (!replace) {
        vd_stage_abort(st);
        return -1;   /* fail loud rather than write into an existing cache */
    } else {
#ifdef __linux__
        /* Exchange leaves no instant where the destination is a mixed cache. */
        if (do_renameat2(st->parent_fd, st->stage, st->parent_fd, st->base,
                         RENAME_EXCHANGE) != 0) {
            vd_stage_abort(st); return -1;
        }
        (void)rm_flat_dir_at(st->parent_fd, st->stage);   /* now the old cache */
#else
        vd_stage_abort(st);
        return -1;
#endif
    }
    st->done = 1;
    (void)fsync(st->parent_fd);
    close(st->parent_fd);
    st->parent_fd = -1;
    return 0;
}

int vd_out_open(VdStage *st, const char *name, VdOut *o) {
    if (!st || !o || st->dir_fd < 0 || !name || strchr(name, '/')) return -1;
    memset(o, 0, sizeof *o);
    o->cap = 1u << 20;
    o->buf = (unsigned char *)malloc(o->cap);
    if (!o->buf) return -1;
    o->sha = malloc(sizeof(VdSha256));
    if (!o->sha) { free(o->buf); o->buf = NULL; return -1; }
    vd_sha256_init((VdSha256 *)o->sha);
    /* O_EXCL|O_NOFOLLOW: a planted symlink or pre-existing file is refused. */
    o->fd = openat(st->dir_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (o->fd < 0) { free(o->buf); free(o->sha); o->buf = NULL; o->sha = NULL; return -1; }
    o->hex[0] = 0;
    return 0;
}

static int out_flush(VdOut *o) {
    size_t off = 0;
    while (off < o->n) {
        ssize_t w = write(o->fd, o->buf + off, o->n - off);
        if (w < 0) { if (errno == EINTR) continue; o->err = 1; return -1; }
        if (w == 0) { o->err = 1; return -1; }
        off += (size_t)w;
    }
    o->n = 0;
    return 0;
}

int vd_out_write(VdOut *o, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    if (!o || o->fd < 0 || o->err) return -1;
    if (!b && n) { o->err = 1; return -1; }
    if (o->sha) vd_sha256_update((VdSha256 *)o->sha, p, n);
    while (n) {
        size_t t = o->cap - o->n;
        if (t > n) t = n;
        memcpy(o->buf + o->n, b, t);
        o->n += t; b += t; n -= t;
        if (o->n == o->cap && out_flush(o) != 0) return -1;
    }
    return 0;
}

int vd_out_finish(VdOut *o) {
    int rc = 0;
    if (!o || o->fd < 0) return -1;
    if (o->err) rc = -1;
    if (rc == 0 && out_flush(o) != 0) rc = -1;
    if (rc == 0 && fsync(o->fd) != 0) rc = -1;
    if (close(o->fd) != 0) rc = -1;
    o->fd = -1;
    free(o->buf);
    o->buf = NULL;
    if (rc == 0 && o->sha) vd_sha256_hex((VdSha256 *)o->sha, o->hex);
    free(o->sha);
    o->sha = NULL;
    return rc;
}

int vd_out_digest(VdOut *o, char *hex_out) {
    if (!o || !hex_out || !o->hex[0]) return -1;
    memcpy(hex_out, o->hex, 65);
    return 0;
}
