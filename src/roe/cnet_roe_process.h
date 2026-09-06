#ifndef CNET_ROE_PROCESS_H
#define CNET_ROE_PROCESS_H

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static inline int roe_process_canonical_path(const char *path, char *out,
                                             size_t cap) {
    char *resolved;
    size_t n;
    if (!path || !out || cap < 2) return -1;
    resolved = realpath(path, NULL);
    if (!resolved) return -1;
    n = strlen(resolved);
    if (n >= cap) {
        free(resolved);
        return -1;
    }
    memcpy(out, resolved, n + 1);
    free(resolved);
    return 0;
}

static inline int roe_process_wait(pid_t pid) {
    int status;
    pid_t got;
    do {
        got = waitpid(pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    return got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static inline int roe_process_run_quiet(char *const argv[]) {
    posix_spawn_file_actions_t actions;
    pid_t pid;
    int rc;
    if (!argv || !argv[0]) return -1;
    if (posix_spawn_file_actions_init(&actions) != 0) return -1;
    rc = posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (rc == 0)
        rc = posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (rc == 0)
        rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return -1;
    return roe_process_wait(pid);
}

static inline int roe_process_capture_stdout(char *const argv[], char *out,
                                             size_t cap) {
    posix_spawn_file_actions_t actions;
    char chunk[4096];
    int pipefd[2] = {-1, -1};
    pid_t pid;
    size_t used = 0;
    int rc;

    if (!argv || !argv[0] || !out || cap < 2) return -1;
    out[0] = '\0';
    if (pipe(pipefd) != 0) return -1;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    rc = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    if (rc == 0)
        rc = posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    if (rc == 0)
        rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    pipefd[1] = -1;
    if (rc != 0) {
        close(pipefd[0]);
        return -1;
    }

    for (;;) {
        ssize_t got = read(pipefd[0], chunk, sizeof chunk);
        if (got > 0) {
            size_t take = (size_t)got;
            if (take > cap - 1 - used) take = cap - 1 - used;
            if (take) {
                memcpy(out + used, chunk, take);
                used += take;
            }
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) rc = -1;
        break;
    }
    close(pipefd[0]);
    out[used] = '\0';
    if (roe_process_wait(pid) != 0) rc = -1;
    return rc == 0 ? 0 : -1;
}

static inline long long roe_process_milliseconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Receipt capture: success requires complete bounded text, EOF and exit zero.
 * Kill the isolated process group and reap the direct child on failure. */
static inline int roe_process_capture_bounded(char *const argv[], char *out,
                                              size_t cap, int timeout_ms) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int fds[2], rc, status = 0, exited = 0, eof = 0;
    pid_t pid;
    size_t used = 0;
    long long start = roe_process_milliseconds();
    if (!argv || !argv[0] || !out || cap < 2 || timeout_ms < 1 || start < 0)
        return -1;
    out[0] = 0;
    if (pipe(fds)) return -1;
    if (fds[0] < 3 || fds[1] < 3) goto fail_pipe;
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) ||
        fcntl(fds[1], F_SETFD, FD_CLOEXEC) ||
        fcntl(fds[0], F_SETFL, O_NONBLOCK)) goto fail_pipe;
    if (posix_spawn_file_actions_init(&actions)) goto fail_pipe;
    if (posix_spawnattr_init(&attr)) {
        posix_spawn_file_actions_destroy(&actions);
        goto fail_pipe;
    }
    rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    if (!rc) rc = posix_spawnattr_setpgroup(&attr, 0);
    if (!rc) rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    if (!rc) rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_addclose(&actions, fds[0]);
    if (!rc) rc = posix_spawn_file_actions_addclose(&actions, fds[1]);
    if (!rc) rc = posix_spawnp(&pid, argv[0], &actions, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (rc) goto fail_pipe;
    close(fds[1]);
    while (!eof || !exited) {
        char chunk[4096];
        long long now = roe_process_milliseconds();
        if (now < 0 || now - start >= timeout_ms) goto fail_child;
        if (!eof) {
            ssize_t n = read(fds[0], chunk, sizeof chunk);
            if (n > 0) {
                if ((size_t)n >= cap - used || memchr(chunk, 0, (size_t)n))
                    goto fail_child;
                memcpy(out + used, chunk, (size_t)n);
                used += (size_t)n;
                continue;
            }
            if (!n) eof = 1;
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                goto fail_child;
        }
        if (!exited) {
            pid_t got = waitpid(pid, &status, WNOHANG);
            if (got == pid) exited = 1;
            else if (got < 0 && errno != EINTR) goto fail_child;
        }
        if (!eof || !exited) {
            struct pollfd pfd = {fds[0], POLLIN, 0};
            int wait_ms = timeout_ms - (int)(now - start);
            if (wait_ms > 10) wait_ms = 10;
            if (poll(eof ? NULL : &pfd, eof ? 0 : 1, wait_ms) < 0 && errno != EINTR)
                goto fail_child;
        }
    }
    close(fds[0]);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { out[0] = 0; return -1; }
    out[used] = 0;
    return 0;
fail_child:
    (void)kill(-pid, SIGKILL);
    if (!exited) (void)roe_process_wait(pid);
    close(fds[0]);
    out[0] = 0;
    return -1;
fail_pipe:
    close(fds[0]);
    close(fds[1]);
    return -1;
}

#endif
