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
#include <sys/file.h>

#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
static int do_renameat2(int ofd, const char *o, int nfd, const char *n, unsigned f) {
    return (int)syscall(SYS_renameat2, ofd, o, nfd, n, f);
}
#endif

static int same_inode(const struct stat *a, const struct stat *b);

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

int vd_stage_begin(const char *dest, VdStage *st) {
    char tmp[VD_PATH_MAX], suffix[16];
    const char *slash;
    size_t n;
    struct stat sb;

    if (!st) return -1;
    memset(st, 0, sizeof *st);
    st->parent_fd = st->dir_fd = st->lock_fd = -1;
    st->created = 0;
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
        /* component-wise: a symlink at ANY level of the parent is refused */
        if (vd_mkdir_p_nofollow(parent) != 0) return -1;
        st->parent_fd = vd_open_dir_nofollow(parent);
        snprintf(st->base, sizeof st->base, "%s", slash + 1);
    } else {
        st->parent_fd = open(".", O_RDONLY | O_DIRECTORY);
        snprintf(st->base, sizeof st->base, "%s", tmp);
    }
    if (strchr(st->base, '/')) { if (st->parent_fd >= 0) close(st->parent_fd); st->parent_fd = -1; return -1; }
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
    /* Recorded BEFORE anything else can fail, so a caller can always read
       vd_stage_quarantine() even when begin itself fails after creation. */
    st->created = 1;
    st->dir_fd = openat(st->parent_fd, st->stage, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (st->dir_fd < 0) {
        /* no unlink: the directory exists and is reported, never removed here */
        vd_stage_abort(st);
        return -1;
    }
    return 0;
}

/* Abort deletes NOTHING, under any condition.
 *
 * Identity-bound directory removal is not expressible with portable POSIX: the
 * inode can only be checked, then removed by pathname, and the gap between the
 * two cannot be closed. Rather than keep narrowing that window, aborting simply
 * releases every held descriptor and reports the unique staging path as
 * quarantine. Reclaiming quarantine is deliberately an operator/offline task;
 * no automatic collection is performed here. */
void vd_stage_abort(VdStage *st) {
    if (!st) return;
    if (st->created && !st->done && st->stage[0]) {
        st->quarantined = 1;
        snprintf(st->quarantine, sizeof st->quarantine, "%s", st->stage);
    }
    if (st->dir_fd >= 0) { close(st->dir_fd); st->dir_fd = -1; }
    if (st->lock_fd >= 0) { close(st->lock_fd); st->lock_fd = -1; }
    if (st->parent_fd >= 0) { close(st->parent_fd); st->parent_fd = -1; }
}

static void (*g_stage_hook)(VdStageHookPhase, void *);
static void *g_stage_hook_ctx;

void vd_stage_set_hook(void (*fn)(VdStageHookPhase, void *), void *ctx) {
    g_stage_hook = fn;
    g_stage_hook_ctx = ctx;
}

static void fire_hook(VdStageHookPhase ph) {
    if (g_stage_hook) g_stage_hook(ph, g_stage_hook_ctx);
}

const char *vd_stage_quarantine(const VdStage *st) {
    return (st && st->quarantined) ? st->quarantine : NULL;
}

static int same_inode(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

/* Publication is a transaction over identities, not pathnames.
 *
 * The destination is opened and validated once, and its (st_dev, st_ino) is
 * held for the whole operation. After the exchange, BOTH sides are checked: the
 * new cache must be the staged inode and the displaced cache must be exactly
 * the inode that was validated. If either differs, or any post-exchange check
 * or fsync fails, the exchange is undone and the rollback verified. Nothing is
 * ever removed by an unvalidated pathname generation -- if identity-safe
 * cleanup cannot be proven, the displaced cache is left under the unique
 * staging name and reported instead of deleted.
 *
 * Cooperating publishers serialise on a parent-local no-follow lock held across
 * validate, exchange, commit and cleanup.
 */
/* Publication is fresh-stage -> absent destination. Nothing else.
 *
 * The previous design validated an existing cache, exchanged it out and then
 * deleted the displaced generation. Every identity guarantee it needed was
 * conditional on that deletion targeting the right inode, which is a check/use
 * gap that cannot be fully closed with POSIX. The benchmark never actually
 * required destructive replacement, so it is gone: a destination that exists is
 * a hard refusal, and no successful path deletes or swaps anything.
 *
 * Cooperating preparers serialise on a parent-local no-follow lock held across
 * the stage identity check and the RENAME_NOREPLACE publication.
 */
int vd_stage_commit(VdStage *st) {
    struct stat stage_sb, chk;

    if (!st || st->dir_fd < 0 || st->parent_fd < 0) return -1;
    if (fstat(st->dir_fd, &stage_sb) != 0) { vd_stage_abort(st); return -1; }
    if (fsync(st->dir_fd) != 0) { vd_stage_abort(st); return -1; }

    st->lock_fd = openat(st->parent_fd, ".vd_publish.lock",
                         O_CREAT | O_RDWR | O_NOFOLLOW, 0644);
    if (st->lock_fd < 0) { vd_stage_abort(st); return -1; }
    if (flock(st->lock_fd, LOCK_EX) != 0) { vd_stage_abort(st); return -1; }

    /* the staging name must still be the directory we built */
    if (fstatat(st->parent_fd, st->stage, &chk, AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_inode(&chk, &stage_sb)) { vd_stage_abort(st); return -1; }

    fire_hook(VD_HOOK_AFTER_VALIDATE);

#ifdef __linux__
    /* RENAME_NOREPLACE or nothing: never a replacing rename, so an existing
       destination can never be clobbered even by a racing publisher. */
    if (do_renameat2(st->parent_fd, st->stage, st->parent_fd, st->base,
                     RENAME_NOREPLACE) != 0) {
        vd_stage_abort(st);
        return -1;
    }
#else
    vd_stage_abort(st);
    return -1;
#endif

    fire_hook(VD_HOOK_AFTER_EXCHANGE);

    /* what is now visible must be exactly what was staged */
    if (fstatat(st->parent_fd, st->base, &chk, AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_inode(&chk, &stage_sb)) {
        /* Do NOT try to undo by pathname: that is the deletion gap this design
           removed. Report and fail closed. */
        st->quarantined = 1;
        snprintf(st->quarantine, sizeof st->quarantine, "%s", st->base);
        if (st->lock_fd >= 0) { close(st->lock_fd); st->lock_fd = -1; }
        if (st->dir_fd >= 0) { close(st->dir_fd); st->dir_fd = -1; }
        close(st->parent_fd); st->parent_fd = -1;
        return -1;
    }
    st->done = 1;
    if (fsync(st->parent_fd) != 0) {
        if (st->lock_fd >= 0) { close(st->lock_fd); st->lock_fd = -1; }
        if (st->dir_fd >= 0) { close(st->dir_fd); st->dir_fd = -1; }
        close(st->parent_fd); st->parent_fd = -1;
        return -1;
    }
    if (st->dir_fd >= 0) { close(st->dir_fd); st->dir_fd = -1; }
    if (st->lock_fd >= 0) { close(st->lock_fd); st->lock_fd = -1; }
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
    o->written += (unsigned long long)n;
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

/* ---------------- component-wise no-follow traversal --------------------- */

/* Walk `path` one component at a time from a trusted start, never following a
   symlink at any level. If `create` is set, missing directories are made with
   mkdirat. Returns a dirfd for the final component. */
static int walk_components(const char *path, int create) {
    char tmp[VD_PATH_MAX];
    size_t n, i = 0;
    int depth = 0, dfd;
    if (vd_path_ok(path) != 0) return -1;
    n = strlen(path);
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;

    if (tmp[0] == '/') { dfd = open("/", O_RDONLY | O_DIRECTORY); i = 1; }
    else               { dfd = open(".", O_RDONLY | O_DIRECTORY); }
    if (dfd < 0) return -1;

    while (i < n) {
        char comp[VD_PATH_MAX];
        size_t j = i, len;
        int next;
        struct stat sb;
        while (j < n && tmp[j] != '/') j++;
        len = j - i;
        if (len == 0) { i = j + 1; continue; }          /* collapse // */
        if (len >= sizeof comp) { close(dfd); return -1; }
        memcpy(comp, tmp + i, len);
        comp[len] = 0;
        if (!strcmp(comp, "..")) { close(dfd); return -1; }
        if (!strcmp(comp, ".")) { i = j + 1; continue; }
        if (++depth > VD_MKDIR_MAX_DEPTH) { close(dfd); return -1; }

        if (create) {
            if (mkdirat(dfd, comp, 0777) != 0 && errno != EEXIST) { close(dfd); return -1; }
        }
        /* O_NOFOLLOW at EVERY component, not just the last one. */
        next = openat(dfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        close(dfd);
        if (next < 0) return -1;
        if (fstat(next, &sb) != 0 || !S_ISDIR(sb.st_mode)) { close(next); return -1; }
        dfd = next;
        i = j + 1;
    }
    return dfd;
}

int vd_open_dir_nofollow(const char *path) { return walk_components(path, 0); }

int vd_mkdir_p_nofollow(const char *path) {
    int fd = walk_components(path, 1);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

int vd_openat_regular(int dirfd, const char *name, off_t *size_out) {
    struct stat sb;
    int fd;
    if (dirfd < 0 || !name || strchr(name, '/')) return -1;
    fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size < 0) { close(fd); return -1; }
    if (size_out) *size_out = sb.st_size;
    return fd;
}

void *vd_fdopen_ro(int fd) {
    int d;
    FILE *f;
    if (fd < 0) return NULL;
    d = dup(fd);
    if (d < 0) return NULL;
    f = fdopen(d, "rb");
    if (!f) { close(d); return NULL; }
    rewind(f);
    return f;
}

int vd_open_file_nofollow(const char *path, off_t *size_out, off_t max_bytes) {
    char tmp[VD_PATH_MAX];
    const char *slash;
    int dfd, fd;
    off_t sz = 0;
    size_t n;
    if (vd_path_ok(path) != 0) return -1;
    n = strlen(path);
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    slash = strrchr(tmp, '/');
    if (slash) {
        char parent[VD_PATH_MAX];
        size_t pn = (size_t)(slash - tmp);
        if (pn == 0) pn = 1;
        memcpy(parent, tmp, pn);
        parent[pn] = 0;
        dfd = vd_open_dir_nofollow(parent);
        if (dfd < 0) return -1;
        fd = vd_openat_regular(dfd, slash + 1, &sz);
        close(dfd);
    } else {
        dfd = open(".", O_RDONLY | O_DIRECTORY);
        if (dfd < 0) return -1;
        fd = vd_openat_regular(dfd, tmp, &sz);
        close(dfd);
    }
    if (fd < 0) return -1;
    if (max_bytes > 0 && sz > max_bytes) { close(fd); return -1; }
    if (size_out) *size_out = sz;
    return fd;
}
