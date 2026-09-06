#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if !defined(__x86_64__)
#ifdef SCRIPTLET_SEAL_LIBRARY
int cnet_scriptlet_seal(void) { return -1; }
#else
int main(void) { return 120; }
#endif
#else

/* Exposed for synthetic policy tests; callers never select real credentials. */
int cnet_scriptlet_identity_allowed(uint32_t uid, uint32_t euid,
        uint32_t gid, uint32_t egid, uint64_t permitted,
        uint64_t effective, uint64_t inheritable) {
    return uid && gid && uid == euid && gid == egid &&
        !permitted && !effective && !inheritable;
}
static int unprivileged(void) {
    uid_t uid,euid,suid;gid_t gid,egid,sgid,groups[128];
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2] = {{0}};
    if (getresuid(&uid,&euid,&suid) || getresgid(&gid,&egid,&sgid) ||
        uid != suid || gid != sgid || syscall(SYS_capget, &header, caps)) return 0;
    int count = getgroups(128, groups);if (count < 0) return 0;
    for (int i=0;i<count;i++) if (!groups[i]) return 0;
    return cnet_scriptlet_identity_allowed(uid,euid,gid,egid,
        caps[0].permitted|((uint64_t)caps[1].permitted<<32),
        caps[0].effective|((uint64_t)caps[1].effective<<32),
        caps[0].inheritable|((uint64_t)caps[1].inheritable<<32));
}

#ifdef SCRIPTLET_SEAL_LIBRARY
/* Exact syscall allowlist, native x86-64 only (including refusal of x32 ABI).
 * No sockets, exec/fork, namespace changes, ioctl, cross-process memory,
 * pidfds, io_uring, filesystem mutation or privilege changes are permitted.
 * Landlock, inherited before CLR startup, constrains allowed read-only opens.
 */
#define ALLOW(n) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_##n, 0, 1), BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW)
int cnet_scriptlet_seal(void) {
    struct rlimit lim;
    if (!unprivileged() || prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 || prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) ||
        getrlimit(RLIMIT_AS, &lim) || lim.rlim_max > 2147483648ULL) return -1;
    /* A direct `dotnet worker.dll` is unsupported: the native launcher must
     * already have denied host reads before any CLR thread was created. */
    int denied = open("/etc/passwd", O_RDONLY|O_CLOEXEC);
    if (denied >= 0) { close(denied); return -1; }
    if (errno != EACCES && errno != EPERM) return -1;
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)),
        /* O_PATH bypasses Landlock open checks and could expose host metadata.
         * Post-seal opens must request actual read access, checked by Landlock. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_open, 0, 4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[1])),
        BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K, O_PATH, 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        /* CLR memory hints only. In particular no HWPOISON/SOFT_OFFLINE,
         * shared backing removal, or memory deduplication advice. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_madvise, 0, 8),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[2])),
        BPF_JUMP(BPF_JMP|BPF_JGT|BPF_K, 4, 0, 4),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 8, 3, 0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 14, 2, 0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 15, 1, 0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 16, 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_openat, 0, 4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[2])),
        BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K, O_PATH, 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        /* Read our own limits; never change them or inspect another process. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_prlimit64, 0, 8),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 0, 0, 5),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[2])),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 0, 0, 3),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[2])+4),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 0, 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM),
        /* Private CLR barriers never target another process. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_membarrier, 0, 8),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 0, 4, 0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 8, 3, 0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 16, 2, 0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 32, 1, 0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 64, 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM),
        /* In particular, no F_SETOWN/F_SETSIG external-signal authority. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_fcntl, 0, 5),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[1])),
        BPF_JUMP(BPF_JMP|BPF_JGT|BPF_K, F_SETFL, 0, 1),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, F_DUPFD_CLOEXEC, 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM),
        /* CLR suspension signals may target only this process's threads. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_tgkill, 0, 4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, (uint32_t)getpid(), 0, 1),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM),
        ALLOW(read), ALLOW(readv), ALLOW(pread64), ALLOW(write), ALLOW(writev),
        ALLOW(close), ALLOW(fstat), ALLOW(lseek),
        ALLOW(mmap), ALLOW(mprotect), ALLOW(munmap), ALLOW(mremap), ALLOW(brk),
        /* CLR's pre-existing JIT backing file may grow to RLIMIT_FSIZE.
         * No new memfd or thread can amplify kernel memory after the seal. */
        ALLOW(msync), ALLOW(ftruncate),
        ALLOW(rt_sigaction), ALLOW(rt_sigprocmask), ALLOW(rt_sigreturn),
        ALLOW(sigaltstack), ALLOW(futex), ALLOW(futex_waitv), ALLOW(sched_yield),
        ALLOW(sched_getaffinity), ALLOW(sched_getparam), ALLOW(sched_getscheduler),
        ALLOW(sched_get_priority_max), ALLOW(sched_get_priority_min),
        ALLOW(getpid), ALLOW(getppid), ALLOW(gettid), ALLOW(getuid), ALLOW(geteuid),
        ALLOW(getgid), ALLOW(getegid), ALLOW(getgroups), ALLOW(getcpu),
        ALLOW(clock_gettime), ALLOW(clock_getres), ALLOW(clock_nanosleep),
        ALLOW(nanosleep), ALLOW(gettimeofday), ALLOW(times), ALLOW(getrusage),
        ALLOW(getrlimit), ALLOW(uname), ALLOW(sysinfo), ALLOW(getrandom),
        ALLOW(dup), ALLOW(dup2), ALLOW(dup3), ALLOW(pipe2),
        ALLOW(poll), ALLOW(ppoll), ALLOW(select), ALLOW(pselect6),
        ALLOW(epoll_create1), ALLOW(epoll_ctl), ALLOW(epoll_wait), ALLOW(epoll_pwait),
        ALLOW(eventfd2), ALLOW(set_robust_list), ALLOW(rseq), ALLOW(restart_syscall),
        ALLOW(exit), ALLOW(exit_group),
        BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ERRNO|EPERM)
    };
    struct sock_fprog prog = {sizeof code / sizeof *code, code};
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                   SECCOMP_FILTER_FLAG_TSYNC, &prog) == 0 ? 0 : -1;
}
#else
static int limit(int resource, rlim_t value) {
    struct rlimit lim = {value, value};
    return setrlimit(resource, &lim);
}
static int allow_read(int rules, const char *path, int executable) {
    int fd = open(path, O_PATH|O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return -1; }
    struct landlock_path_beneath_attr rule = {
        .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE |
            (S_ISDIR(st.st_mode) ? LANDLOCK_ACCESS_FS_READ_DIR : 0) |
            (executable ? LANDLOCK_ACCESS_FS_EXECUTE : 0),
        .parent_fd = fd
    };
    int rc = syscall(SYS_landlock_add_rule, rules, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
    close(fd); return rc;
}
static int join(char *out, size_t capacity, const char *dir, const char *leaf) {
    int n = snprintf(out, capacity, "%s/%s", dir, leaf);
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}
int main(int argc, char **argv) {
    /* Arguments are fixed by the trusted host, never taken from script input:
     * exact dotnet executable, private worker payload, exact runtime directory. */
    if (argc != 4 || !unprivileged()) return 120;
    char dotnet[4096], worker[4096], runtime[4096], fxr[4096], dll[4096];
    if (!realpath(argv[1], dotnet) || !realpath(argv[2], worker) ||
        !realpath(argv[3], runtime)) return 120;
    char root[4096]; strcpy(root, dotnet);
    char *slash = strrchr(root, '/'); if (!slash) return 120; *slash = 0;
    if (join(fxr, sizeof fxr, root, "host/fxr") ||
        join(dll, sizeof dll, worker, "CNET.Scriptlet.Worker.dll")) return 120;
    char *version = strrchr(runtime, '/'); if (!version || !version[1]) return 120;
    pid_t parent = getppid();
    if ((setsid() < 0 && getsid(0) != getpid()) || prctl(PR_SET_PDEATHSIG, 9, 0, 0, 0) ||
        getppid() != parent || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return 120;
    if (limit(RLIMIT_AS, 2147483648ULL) || limit(RLIMIT_CPU, 10) ||
        limit(RLIMIT_CORE, 0) || limit(RLIMIT_FSIZE, 67108864) ||
        limit(RLIMIT_NOFILE, 256) || limit(RLIMIT_MEMLOCK, 0)) return 120;
    if (syscall(SYS_close_range, 3U, ~0U, 0)) return 120;
    if (clearenv() || setenv("DOTNET_EnableDiagnostics", "0", 1) ||
        setenv("DOTNET_GCHeapHardLimit", "8000000", 1) ||
        setenv("MALLOC_ARENA_MAX", "2", 1) ||
        setenv("DOTNET_gcServer", "0", 1) ||
        setenv("DOTNET_gcConcurrent", "0", 1) ||
        setenv("DOTNET_TieredCompilation", "0", 1) ||
        setenv("DOTNET_PROCESSOR_COUNT", "2", 1) ||
        setenv("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1", 1) ||
        setenv("DOTNET_CLI_TELEMETRY_OPTOUT", "1", 1)) return 120;
    long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 7) return 121;
    /* ABI 7 has 16 filesystem rights, TCP rights and signal/abstract-socket
     * scoping. No filesystem write/create/truncate/ioctl access is granted. */
    struct { uint64_t fs, net, scoped; } attr = {(1ULL<<16)-1, 3, 3};
    int rules = syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, 0);
    if (rules < 0) return 121;
    if (allow_read(rules, dotnet, 1) || allow_read(rules, worker, 0) ||
        allow_read(rules, runtime, 0) || allow_read(rules, fxr, 0) ||
        allow_read(rules, "/usr/lib/x86_64-linux-gnu/libc.so.6", 0) ||
        allow_read(rules, "/usr/lib/x86_64-linux-gnu/libdl.so.2", 0) ||
        allow_read(rules, "/usr/lib/x86_64-linux-gnu/libpthread.so.0", 0) ||
        allow_read(rules, "/usr/lib/x86_64-linux-gnu/libstdc++.so.6", 0) ||
        allow_read(rules, "/usr/lib/x86_64-linux-gnu/libm.so.6", 0) ||
        allow_read(rules, "/usr/lib/x86_64-linux-gnu/libgcc_s.so.1", 0) ||
        allow_read(rules, "/usr/lib/x86_64-linux-gnu/librt.so.1", 0) ||
        allow_read(rules, "/usr/lib/x86_64-linux-gnu/libcrypto.so.3", 0) ||
        allow_read(rules, "/usr/lib/x86_64-linux-gnu/libssl.so.3", 0) ||
        allow_read(rules, "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", 1) ||
        allow_read(rules, "/etc/ld.so.cache", 0) ||
        /* CoreCLR requires its own mappings during startup. The rule pins
         * this process's proc inode; exec preserves PID, and no other proc
         * entry (including another process's maps) is granted. */
        allow_read(rules, "/proc/self/maps", 0) ||
        /* The runtime's cryptographic RNG reads entropy here. This is a
         * read-only device grant; ioctl and every host-file write stay denied. */
        allow_read(rules, "/dev/urandom", 0) ||
        syscall(SYS_landlock_restrict_self, rules, 0)) return 121;
    close(rules);
    if (chdir(worker)) return 121;
    char *args[] = {dotnet, "exec", "--fx-version", version+1, dll, NULL};
    execv(dotnet, args);
    return 122;
}
#endif
#endif
