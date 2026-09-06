#define _GNU_SOURCE
#include "cnet_capsule_core.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int private_fd(int fd, int directory) {
    struct stat s;
    return fd >= 0 && !fstat(fd, &s) && s.st_uid == geteuid() &&
        !(s.st_mode & 077) && (directory ? S_ISDIR(s.st_mode) : S_ISREG(s.st_mode));
}
static int atom(const char *s) {
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    return 1;
}
int cnet_capsule_demand_note(const char *root, const char *request) {
    char verb[16], input[32], output[32], number[32], extra;
    char name[128], body[96], temp[64] = "";
    int dir = -1, lock = -1, fd = -1, result = -1;
    if (!root || !request || sscanf(request, "%15s %31s %31s %31s %c",
        verb, input, output, number, &extra) != 4 || strcmp(verb, "capsule") ||
        !atom(input) || !atom(output)) return -1;
    for (size_t i = 0; number[i]; i++) if (!isdigit((unsigned char)number[i])) return -1;
    errno = 0; char *end;
    unsigned long value = strtoul(number, &end, 10);
    if (errno || *end || value > 65535) return -1;
    snprintf(name, sizeof name, "i%zu_%s_o%zu_%s_v%lu.req", strlen(input), input, strlen(output), output, value);
    int len = snprintf(body, sizeof body, "%s %s %lu\n", input, output, value);
    dir = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!private_fd(dir, 1)) goto done;
    lock = openat(dir, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (!private_fd(lock, 0) || flock(lock, LOCK_EX | LOCK_NB)) goto done;
    fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        char previous[96];
        if (private_fd(fd, 0) && read(fd, previous, sizeof previous) == len && !memcmp(previous, body, (size_t)len)) result = 1;
        goto done;
    }
    if (errno != ENOENT) goto done;
    DIR *scan = fdopendir(dup(dir));
    if (!scan) goto done;
    size_t pending = 0, entries = 0;
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(scan))) {
        size_t n = strlen(ent->d_name);
        if (n > 4 && !strcmp(ent->d_name + n - 4, ".req")) pending++;
        if (++entries > 512) break;
    }
    int scan_error = errno;
    closedir(scan);
    if (scan_error || pending >= 256 || entries > 512) goto done;
    for (unsigned i = 0; i < 32; i++) {
        snprintf(temp, sizeof temp, ".demand-%ld-%u", (long)getpid(), i);
        fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { temp[0] = 0; goto done; }
    if (write(fd, body, (size_t)len) != len || fsync(fd)) goto done;
    if (linkat(dir, temp, dir, name, 0) || fsync(dir)) goto done;
    result = 0;
done:
    if (temp[0]) unlinkat(dir, temp, 0);
    if (fd >= 0) close(fd);
    if (lock >= 0) close(lock);
    if (dir >= 0) close(dir);
    return result;
}
