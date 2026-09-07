#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Test subprocess only: fail publication's directory sync, after rename.
 * Regular-file syncs and every other directory retain the real syscall. */
static int data_directory_fsync(int fd) {
    char proc[64], target[4096];
    struct stat value;
    int length = snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    ssize_t used = length > 0 && (size_t)length < sizeof proc
        ? readlink(proc, target, sizeof target - 1) : -1;
    if (used >= 0 && (size_t)used < sizeof target - 1) {
        static const char suffix[] = "/work/data";
        target[used] = '\0';
        if ((size_t)used >= sizeof suffix - 1 &&
            !strcmp(target + used - (sizeof suffix - 1), suffix) &&
            !fstat(fd, &value) && S_ISDIR(value.st_mode)) {
            static const char marker[] = "LEARNING_IMPORT_DIRECTORY_FSYNC_INJECTED\n";
            if (write(STDERR_FILENO, marker, sizeof marker - 1) != sizeof marker - 1) _exit(126);
            errno = EIO;
            return -1;
        }
    }
    return (int)syscall(SYS_fsync, fd);
}

int fsync(int fd) { return data_directory_fsync(fd); }

/* Explicit-handle .NET libc imports use dlsym rather than ELF interposition.
 * Redirect only fsync; resolve all other symbols through the real glibc entry.
 * No process-wide environment or production import resolver is changed. */
void *dlsym(void *handle, const char *name) {
    if (!strcmp(name, "fsync")) return (void *)data_directory_fsync;
    void *(*real_dlsym)(void *, const char *) =
        (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    if (!real_dlsym) _exit(125);
    return real_dlsym(handle, name);
}
