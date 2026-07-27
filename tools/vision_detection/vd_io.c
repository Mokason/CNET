#include "vd_io.h"

#include <errno.h>
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
