#ifndef CNET_ROE_PROCESS_H
#define CNET_ROE_PROCESS_H

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
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

#endif
