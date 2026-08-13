/*
 * gate_evidence.c — run a gate so its log cannot be mistaken for a stale one.
 *
 * C port of the CORE logic in scripts/gate_evidence.py. Standalone: no CNET
 * headers, no OpenSSL. Content-addressed pre/post binding via git; marker is a
 * whole-line prefix; conflicting terminal verdicts refused; binding drift
 * refused.
 *
 * Usage: gate_evidence GATE LOG MARKER -- CMD [ARG...]
 * Build: gcc -std=c11 -O2 -o bin/gate_evidence tools/gate_evidence.c
 */

#define _DEFAULT_SOURCE
#define _CRT_SECURE_NO_WARNINGS

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define strtok_r strtok_s
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* -------------------------------------------------------------------------- */
/* Constants                                                                  */
/* -------------------------------------------------------------------------- */

#define SPECIAL_INDEX_BYTE_CAP (512ULL * 1024ULL * 1024ULL)

static const char *TERMINAL_WORDS[] = {
    "PASS", "FAIL", "WITHHELD", "BLOCKED", "NO_VERDICT", "AMBIGUOUS", NULL
};

/* -------------------------------------------------------------------------- */
/* SHA-256 (self-contained, public-domain style compact impl)                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    unsigned char data[64];
    unsigned long long bitlen;
    unsigned int datalen;
    unsigned int state[8];
} Sha256Ctx;

static unsigned int rotr32(unsigned int x, unsigned int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(Sha256Ctx *ctx, const unsigned char data[]) {
    static const unsigned int k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    unsigned int a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((unsigned int)data[j] << 24) | ((unsigned int)data[j + 1] << 16) |
               ((unsigned int)data[j + 2] << 8) | ((unsigned int)data[j + 3]);
    for (; i < 64; ++i)
        m[i] = (rotr32(m[i - 2], 17) ^ rotr32(m[i - 2], 19) ^ (m[i - 2] >> 10)) +
               m[i - 7] +
               (rotr32(m[i - 15], 7) ^ rotr32(m[i - 15], 18) ^ (m[i - 15] >> 3)) +
               m[i - 16];

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) +
             ((e & f) ^ (~e & g)) + k[i] + m[i];
        t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) +
             ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(Sha256Ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(Sha256Ctx *ctx, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = p[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(Sha256Ctx *ctx, unsigned char hash[32]) {
    unsigned int i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56)
            ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64)
            ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (unsigned long long)ctx->datalen * 8ULL;
    ctx->data[63] = (unsigned char)(ctx->bitlen);
    ctx->data[62] = (unsigned char)(ctx->bitlen >> 8);
    ctx->data[61] = (unsigned char)(ctx->bitlen >> 16);
    ctx->data[60] = (unsigned char)(ctx->bitlen >> 24);
    ctx->data[59] = (unsigned char)(ctx->bitlen >> 32);
    ctx->data[58] = (unsigned char)(ctx->bitlen >> 40);
    ctx->data[57] = (unsigned char)(ctx->bitlen >> 48);
    ctx->data[56] = (unsigned char)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i] = (unsigned char)((ctx->state[0] >> (24 - i * 8)) & 0xff);
        hash[i + 4] = (unsigned char)((ctx->state[1] >> (24 - i * 8)) & 0xff);
        hash[i + 8] = (unsigned char)((ctx->state[2] >> (24 - i * 8)) & 0xff);
        hash[i + 12] = (unsigned char)((ctx->state[3] >> (24 - i * 8)) & 0xff);
        hash[i + 16] = (unsigned char)((ctx->state[4] >> (24 - i * 8)) & 0xff);
        hash[i + 20] = (unsigned char)((ctx->state[5] >> (24 - i * 8)) & 0xff);
        hash[i + 24] = (unsigned char)((ctx->state[6] >> (24 - i * 8)) & 0xff);
        hash[i + 28] = (unsigned char)((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}

static void sha256_hex(const void *data, size_t len, char out[65]) {
    unsigned char hash[32];
    Sha256Ctx ctx;
    size_t i;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);
    for (i = 0; i < 32; ++i)
        sprintf(out + i * 2, "%02x", hash[i]);
    out[64] = '\0';
}

static int sha256_file_raw(const char *path, unsigned char out[32]) {
    FILE *fp;
    unsigned char buf[65536];
    size_t n;
    Sha256Ctx ctx;

    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    sha256_init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        sha256_update(&ctx, buf, n);
    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    sha256_final(&ctx, out);
    return 0;
}

static int sha256_file_hex(const char *path, char out[65]) {
    unsigned char hash[32];
    size_t i;
    if (sha256_file_raw(path, hash) != 0)
        return -1;
    for (i = 0; i < 32; ++i)
        sprintf(out + i * 2, "%02x", hash[i]);
    out[64] = '\0';
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Dynbuf / string helpers                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DynBuf;

static int db_reserve(DynBuf *b, size_t need) {
    char *n;
    size_t cap;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    n = (char *)realloc(b->data, cap);
    if (!n)
        return -1;
    b->data = n;
    b->cap = cap;
    return 0;
}

static int db_append(DynBuf *b, const void *p, size_t n) {
    if (db_reserve(b, b->len + n + 1) != 0)
        return -1;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int db_append_str(DynBuf *b, const char *s) {
    return db_append(b, s, strlen(s));
}

static int db_append_fmt(DynBuf *b, const char *fmt, ...) {
    va_list ap;
    int n;
    char stack[512];
    char *heap = NULL;
    const char *use;
    va_list ap2;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return -1;
    }
    if ((size_t)n < sizeof(stack)) {
        use = stack;
    } else {
        heap = (char *)malloc((size_t)n + 1);
        if (!heap) {
            va_end(ap2);
            return -1;
        }
        vsnprintf(heap, (size_t)n + 1, fmt, ap2);
        use = heap;
    }
    va_end(ap2);
    if (db_append(b, use, (size_t)n) != 0) {
        free(heap);
        return -1;
    }
    free(heap);
    return 0;
}

static void db_free(DynBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static int cmp_cstr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* -------------------------------------------------------------------------- */
/* Path / filesystem (never follow symlinks for identity)                     */
/* -------------------------------------------------------------------------- */

static void path_join(char *out, size_t n, const char *root, const char *rel) {
    size_t rl = strlen(root);
    if (rl > 0 && (root[rl - 1] == '/' || root[rl - 1] == '\\'))
        snprintf(out, n, "%s%s", root, rel);
    else
        snprintf(out, n, "%s/%s", root, rel);
}

#ifdef _WIN32
static int is_symlink_win(const char *path) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return 0;
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

static int readlink_win(const char *path, char *out, size_t n) {
    HANDLE h;
    BYTE buf[1024];
    DWORD bytes = 0;
    /* REPARSE_DATA_BUFFER layout (minimal) */
    typedef struct {
        ULONG ReparseTag;
        USHORT ReparseDataLength;
        USHORT Reserved;
        USHORT SubstituteNameOffset;
        USHORT SubstituteNameLength;
        USHORT PrintNameOffset;
        USHORT PrintNameLength;
        WCHAR PathBuffer[1];
    } REPARSE_DATA_BUFFER_MIN;
    REPARSE_DATA_BUFFER_MIN *rdb = (REPARSE_DATA_BUFFER_MIN *)buf;
    WCHAR *name;
    int len;

    h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof(buf),
                         &bytes, NULL)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    if (rdb->ReparseTag != IO_REPARSE_TAG_SYMLINK &&
        rdb->ReparseTag != IO_REPARSE_TAG_MOUNT_POINT)
        return -1;
    name = rdb->PathBuffer + (rdb->PrintNameOffset / sizeof(WCHAR));
    len = WideCharToMultiByte(CP_UTF8, 0, name, rdb->PrintNameLength / sizeof(WCHAR),
                              out, (int)n - 1, NULL, NULL);
    if (len <= 0)
        return -1;
    out[len] = '\0';
    return 0;
}
#endif

/*
 * path_identity: append identity bytes into digest.
 * absent / symlink:TARGET / special:OCTAL / raw 32-byte sha256 of regular file.
 */
static void path_identity_update(Sha256Ctx *digest, const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    char link[1024];
    unsigned char hash[32];
    char special[64];

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
        sha256_update(digest, "absent", 6);
        return;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (readlink_win(path, link, sizeof(link)) == 0) {
            sha256_update(digest, "symlink:", 8);
            sha256_update(digest, link, strlen(link));
        } else {
            sha256_update(digest, "absent", 6);
        }
        return;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        /* S_IFDIR-ish: match POSIX S_IFMT(dir) ≈ 0040000 */
        snprintf(special, sizeof(special), "special:%o", 0040000);
        sha256_update(digest, special, strlen(special));
        return;
    }
    if (sha256_file_raw(path, hash) != 0) {
        sha256_update(digest, "absent", 6);
        return;
    }
    sha256_update(digest, hash, 32);
#else
    struct stat st;
    char link[4096];
    ssize_t lr;
    unsigned char hash[32];
    char special[64];

    if (lstat(path, &st) != 0) {
        sha256_update(digest, "absent", 6);
        return;
    }
    if (S_ISLNK(st.st_mode)) {
        lr = readlink(path, link, sizeof(link) - 1);
        if (lr < 0) {
            sha256_update(digest, "absent", 6);
        } else {
            link[lr] = '\0';
            sha256_update(digest, "symlink:", 8);
            sha256_update(digest, link, (size_t)lr);
        }
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(special, sizeof(special), "special:%o", (unsigned)(st.st_mode & S_IFMT));
        sha256_update(digest, special, strlen(special));
        return;
    }
    if (sha256_file_raw(path, hash) != 0) {
        sha256_update(digest, "absent", 6);
        return;
    }
    sha256_update(digest, hash, 32);
#endif
}

static long long path_lsize(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULARGE_INTEGER u;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        return -1;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return 0; /* size of link itself not used for cap the same way; Python uses lstat size */
    u.LowPart = fad.nFileSizeLow;
    u.HighPart = fad.nFileSizeHigh;
    return (long long)u.QuadPart;
#else
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    return (long long)st.st_size;
#endif
}

static int mkdir_p_parent(const char *file_path) {
    char *tmp = xstrdup(file_path);
    char *p;
    if (!tmp)
        return -1;
    for (p = tmp + 1; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = c;
        }
    }
    free(tmp);
    return 0;
}

static void unlink_quiet(const char *path) {
#ifdef _WIN32
    _unlink(path);
#else
    unlink(path);
#endif
}

/* -------------------------------------------------------------------------- */
/* Repo root (parent of bin/ or scripts/)                                     */
/* -------------------------------------------------------------------------- */

static void dirname_inplace(char *path) {
    char *slash = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    char *cut = slash;
    if (bslash && (!cut || bslash > cut))
        cut = bslash;
    if (!cut) {
        path[0] = '.';
        path[1] = '\0';
        return;
    }
    if (cut == path) {
        path[1] = '\0';
        return;
    }
    *cut = '\0';
}

static int find_root(char *out, size_t n, const char *argv0) {
    char path[4096];
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (len == 0 || len >= sizeof(path)) {
        if (!argv0 || !argv0[0])
            return -1;
        strncpy(path, argv0, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
#else
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) {
        path[len] = '\0';
    } else {
        if (!argv0 || !argv0[0])
            return -1;
        if (!realpath(argv0, path)) {
            strncpy(path, argv0, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }
    }
#endif
    dirname_inplace(path); /* .../bin or .../scripts */
    dirname_inplace(path); /* repo root */
    if (strlen(path) + 1 > n)
        return -1;
    memcpy(out, path, strlen(path) + 1);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* git capture (controlled argv — no shell)                                   */
/* -------------------------------------------------------------------------- */

#ifdef _WIN32
static char *win_quote_arg(const char *arg) {
    DynBuf b = {0};
    size_t i;
    int need_quote = 0;
    for (i = 0; arg[i]; ++i) {
        if (arg[i] == ' ' || arg[i] == '\t' || arg[i] == '"' || arg[i] == '\n')
            need_quote = 1;
    }
    if (!need_quote)
        return xstrdup(arg);
    if (db_append_str(&b, "\"") != 0)
        goto fail;
    for (i = 0; arg[i]; ++i) {
        if (arg[i] == '"') {
            if (db_append_str(&b, "\\\"") != 0)
                goto fail;
        } else if (arg[i] == '\\') {
            /* count backslashes before a quote or end — keep simple: double before " */
            size_t bs = 0;
            while (arg[i] == '\\') {
                bs++;
                i++;
            }
            if (arg[i] == '"' || arg[i] == '\0') {
                size_t k;
                for (k = 0; k < bs * 2; ++k)
                    if (db_append(&b, "\\", 1) != 0)
                        goto fail;
                if (arg[i] == '\0') {
                    break;
                }
                if (db_append_str(&b, "\\\"") != 0)
                    goto fail;
            } else {
                size_t k;
                for (k = 0; k < bs; ++k)
                    if (db_append(&b, "\\", 1) != 0)
                        goto fail;
                i--; /* re-process current non-backslash */
            }
        } else {
            if (db_append(&b, &arg[i], 1) != 0)
                goto fail;
        }
    }
    if (db_append_str(&b, "\"") != 0)
        goto fail;
    return b.data;
fail:
    db_free(&b);
    return NULL;
}

static char *win_join_argv(char *const argv[]) {
    DynBuf b = {0};
    int i;
    for (i = 0; argv[i]; ++i) {
        char *q = win_quote_arg(argv[i]);
        if (!q) {
            db_free(&b);
            return NULL;
        }
        if (i && db_append_str(&b, " ") != 0) {
            free(q);
            db_free(&b);
            return NULL;
        }
        if (db_append_str(&b, q) != 0) {
            free(q);
            db_free(&b);
            return NULL;
        }
        free(q);
    }
    return b.data;
}

static int run_capture_win(char *const argv[], const char *cwd, DynBuf *out,
                           int *status_out) {
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *cmdline;
    char *env_block = NULL;
    DWORD exit_code = 1;
    char buf[4096];
    DWORD nread;

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;

    cmdline = win_join_argv(argv);
    if (!cmdline) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }

    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, env_block,
                        cwd && cwd[0] ? cwd : NULL, &si, &pi)) {
        free(cmdline);
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    free(cmdline);
    CloseHandle(wr);

    for (;;) {
        if (!ReadFile(rd, buf, sizeof(buf), &nread, NULL) || nread == 0)
            break;
        if (db_append(out, buf, nread) != 0)
            break;
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    *status_out = (int)exit_code;
    return 0;
}
#else
static int run_capture_unix(char *const argv[], const char *cwd, DynBuf *out,
                            int *status_out) {
    int pipefd[2];
    pid_t pid;
    char buf[4096];
    ssize_t n;

    if (pipe(pipefd) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[1] != STDOUT_FILENO && pipefd[1] != STDERR_FILENO)
            close(pipefd[1]);
        if (cwd && cwd[0] && chdir(cwd) != 0)
            _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        if (db_append(out, buf, (size_t)n) != 0)
            break;
    }
    close(pipefd[0]);
    {
        int st = 0;
        if (waitpid(pid, &st, 0) < 0)
            return -1;
        if (WIFEXITED(st))
            *status_out = WEXITSTATUS(st);
        else if (WIFSIGNALED(st))
            *status_out = 128 + WTERMSIG(st);
        else
            *status_out = 1;
    }
    return 0;
}
#endif

static int run_capture(char *const argv[], const char *cwd, DynBuf *out,
                       int *status_out) {
#ifdef _WIN32
    return run_capture_win(argv, cwd, out, status_out);
#else
    return run_capture_unix(argv, cwd, out, status_out);
#endif
}

static char *git_run(const char *root, char *const args[]) {
    DynBuf out = {0};
    int status = 1;
    char *argv[16];
    int i;
    argv[0] = (char *)"git";
    for (i = 0; args[i] && i < 14; ++i)
        argv[i + 1] = args[i];
    argv[i + 1] = NULL;
    if (run_capture(argv, root, &out, &status) != 0 || status != 0) {
        db_free(&out);
        return xstrdup("");
    }
    if (!out.data)
        return xstrdup("");
    return out.data;
}

/* -------------------------------------------------------------------------- */
/* Binding capture                                                            */
/* -------------------------------------------------------------------------- */

typedef struct {
    char commit[128];
    char worktree_sha256[65];
    int worktree_dirty_files;
    char special_index_sha256[65];
    int special_index_files;
    unsigned long long special_index_bytes;
    char untracked_sha256[65];
} Binding;

typedef struct {
    char *path;
    char flag;
} SpecEntry;

static void strip_quotes(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static void porcelain_entry(const char *line, char *entry, size_t n) {
    const char *p = line + 3;
    const char *arrow;
    char tmp[4096];
    strncpy(tmp, p, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    /* trim */
    {
        char *e = tmp + strlen(tmp);
        while (e > tmp && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
            *--e = '\0';
    }
    arrow = strstr(tmp, " -> ");
    if (arrow)
        strncpy(entry, arrow + 4, n - 1);
    else
        strncpy(entry, tmp, n - 1);
    entry[n - 1] = '\0';
    strip_quotes(entry);
}

static void tracked_binding(const char *root, Binding *b) {
    char *args[] = {(char *)"status", (char *)"--porcelain=v1", NULL};
    char *text = git_run(root, args);
    Sha256Ctx ctx;
    char *line, *save = NULL;
    int dirty = 0;
    unsigned char hash[32];
    size_t i;

    sha256_init(&ctx);
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char entry[4096];
        char full[8192];
        size_t L = strlen(line);
        if (L < 4)
            continue;
        porcelain_entry(line, entry, sizeof(entry));
        sha256_update(&ctx, line, 3);
        sha256_update(&ctx, entry, strlen(entry));
        path_join(full, sizeof(full), root, entry);
        path_identity_update(&ctx, full);
        dirty++;
    }
    sha256_final(&ctx, hash);
    for (i = 0; i < 32; ++i)
        sprintf(b->worktree_sha256 + i * 2, "%02x", hash[i]);
    b->worktree_sha256[64] = '\0';
    b->worktree_dirty_files = dirty;
    free(text);
}

static int cmp_spec_entry(const void *a, const void *b) {
    return strcmp(((const SpecEntry *)a)->path, ((const SpecEntry *)b)->path);
}

static void special_index_binding(const char *root, Binding *b) {
    DynBuf out = {0};
    int status = 1;
    char *argv[] = {(char *)"git", (char *)"ls-files", (char *)"-v", (char *)"-z", NULL};
    SpecEntry *entries = NULL;
    size_t nent = 0, cap = 0, i;
    Sha256Ctx ctx;
    unsigned char hash[32];
    unsigned long long total = 0;

    if (run_capture(argv, root, &out, &status) == 0 && status == 0 && out.data) {
        size_t off = 0;
        while (off < out.len) {
            char *rec = out.data + off;
            size_t L = strlen(rec);
            if (L >= 3 && rec[1] == ' ' && rec[0] != 'H') {
                if (nent + 1 > cap) {
                    size_t ncap = cap ? cap * 2 : 64;
                    SpecEntry *ne = (SpecEntry *)realloc(entries, ncap * sizeof(*ne));
                    if (!ne)
                        break;
                    entries = ne;
                    cap = ncap;
                }
                entries[nent].flag = rec[0];
                entries[nent].path = xstrdup(rec + 2);
                if (entries[nent].path)
                    nent++;
            }
            off += L + 1;
        }
    }
    db_free(&out);

    if (nent > 1)
        qsort(entries, nent, sizeof(*entries), cmp_spec_entry);

    sha256_init(&ctx);
    for (i = 0; i < nent; ++i) {
        char full[8192];
        char flagbuf[2] = {entries[i].flag, '\0'};
        long long sz;
        path_join(full, sizeof(full), root, entries[i].path);
        sz = path_lsize(full);
        if (sz >= 0)
            total += (unsigned long long)sz;
        sha256_update(&ctx, flagbuf, 1);
        sha256_update(&ctx, entries[i].path, strlen(entries[i].path));
        path_identity_update(&ctx, full);
        free(entries[i].path);
    }
    free(entries);
    sha256_final(&ctx, hash);
    for (i = 0; i < 32; ++i)
        sprintf(b->special_index_sha256 + i * 2, "%02x", hash[i]);
    b->special_index_sha256[64] = '\0';
    b->special_index_files = (int)nent;
    b->special_index_bytes = total;
}

static void untracked_binding(const char *root, Binding *b) {
    DynBuf out = {0};
    int status = 1;
    char *argv[] = {(char *)"git", (char *)"ls-files", (char *)"--others",
                    (char *)"--exclude-standard", (char *)"-z", NULL};
    char **paths = NULL;
    size_t n = 0, cap = 0, i, j;
    Sha256Ctx ctx;
    unsigned char hash[32];

    sha256_init(&ctx);
    if (run_capture(argv, root, &out, &status) == 0 && status == 0 && out.data) {
        size_t off = 0;
        while (off < out.len) {
            char *rec = out.data + off;
            size_t L = strlen(rec);
            if (L > 0) {
                if (n + 1 > cap) {
                    size_t ncap = cap ? cap * 2 : 64;
                    char **np = (char **)realloc(paths, ncap * sizeof(char *));
                    if (!np)
                        break;
                    paths = np;
                    cap = ncap;
                }
                paths[n] = xstrdup(rec);
                if (paths[n])
                    n++;
            }
            off += L + 1;
        }
    }
    db_free(&out);

    qsort(paths, n, sizeof(char *), cmp_cstr);
    for (i = 0; i < n; ++i) {
        char full[8192];
        sha256_update(&ctx, paths[i], strlen(paths[i]));
        path_join(full, sizeof(full), root, paths[i]);
        path_identity_update(&ctx, full);
        free(paths[i]);
    }
    free(paths);
    sha256_final(&ctx, hash);
    for (j = 0; j < 32; ++j)
        sprintf(b->untracked_sha256 + j * 2, "%02x", hash[j]);
    b->untracked_sha256[64] = '\0';
}

static void capture_binding(const char *root, Binding *b) {
    char *args_head[] = {(char *)"rev-parse", (char *)"HEAD", NULL};
    char *head = git_run(root, args_head);
    size_t hl;
    memset(b, 0, sizeof(*b));
    while (head[0] && (head[strlen(head) - 1] == '\n' || head[strlen(head) - 1] == '\r'))
        head[strlen(head) - 1] = '\0';
    hl = strlen(head);
    if (hl == 0)
        strncpy(b->commit, "unknown", sizeof(b->commit) - 1);
    else {
        strncpy(b->commit, head, sizeof(b->commit) - 1);
        b->commit[sizeof(b->commit) - 1] = '\0';
    }
    free(head);
    tracked_binding(root, b);
    special_index_binding(root, b);
    untracked_binding(root, b);
}

/* -------------------------------------------------------------------------- */
/* Marker / verdicts                                                          */
/* -------------------------------------------------------------------------- */

static int line_is_marker(const char *line, const char *marker) {
    size_t m = strlen(marker);
    if (strncmp(line, marker, m) != 0)
        return 0;
    return line[m] == '\0' || line[m] == ' ' || line[m] == '\t' || line[m] == '\r';
}

static int marker_lines(const char *text, const char *marker) {
    const char *p = text;
    int count = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line = (char *)malloc(len + 1);
        if (!line)
            return count;
        memcpy(line, p, len);
        line[len] = '\0';
        if (len && line[len - 1] == '\r')
            line[len - 1] = '\0';
        if (line_is_marker(line, marker))
            count++;
        free(line);
        p = nl ? nl + 1 : NULL;
    }
    return count;
}

static void marker_prefix(const char *marker, char *prefix, size_t n) {
    size_t i;
    strncpy(prefix, marker, n - 1);
    prefix[n - 1] = '\0';
    for (i = 0; TERMINAL_WORDS[i]; ++i) {
        char suffix[64];
        size_t sl, ml;
        snprintf(suffix, sizeof(suffix), "_%s", TERMINAL_WORDS[i]);
        sl = strlen(suffix);
        ml = strlen(marker);
        if (ml >= sl && strcmp(marker + ml - sl, suffix) == 0) {
            size_t keep = ml - sl;
            if (keep >= n)
                keep = n - 1;
            memcpy(prefix, marker, keep);
            prefix[keep] = '\0';
            return;
        }
    }
    prefix[0] = '\0'; /* no terminal suffix → no conflict check */
}

typedef struct {
    char **items;
    size_t n;
} StrList;

static void sl_free(StrList *s) {
    size_t i;
    for (i = 0; i < s->n; ++i)
        free(s->items[i]);
    free(s->items);
    s->items = NULL;
    s->n = 0;
}

static int sl_add(StrList *s, const char *item) {
    char **ni = (char **)realloc(s->items, (s->n + 1) * sizeof(char *));
    if (!ni)
        return -1;
    s->items = ni;
    s->items[s->n] = xstrdup(item);
    if (!s->items[s->n])
        return -1;
    s->n++;
    return 0;
}

static void conflicting_verdicts(const char *text, const char *marker, StrList *out) {
    char prefix[512];
    size_t i;
    marker_prefix(marker, prefix, sizeof(prefix));
    if (!prefix[0])
        return;
    for (i = 0; TERMINAL_WORDS[i]; ++i) {
        char other[576];
        snprintf(other, sizeof(other), "%s_%s", prefix, TERMINAL_WORDS[i]);
        if (strcmp(other, marker) == 0)
            continue;
        if (marker_lines(text, other) > 0)
            sl_add(out, other);
    }
}

/* -------------------------------------------------------------------------- */
/* Environment binding                                                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    char env_sha256[65];
    int env_variables;
    StrList knobs;
} EnvBind;

static void environment_binding(EnvBind *eb) {
    char **names = NULL;
    char **vals = NULL;
    size_t n = 0, cap = 0, i;
    DynBuf ser = {0};
    Sha256Ctx ctx;
    unsigned char hash[32];

    memset(eb, 0, sizeof(*eb));
#ifdef _WIN32
    {
        char *block = GetEnvironmentStringsA();
        char *p;
        if (!block)
            return;
        for (p = block; *p; p += strlen(p) + 1) {
            char *eq = strchr(p, '=');
            char *name, *val;
            if (!eq || p[0] == '=')
                continue;
            name = (char *)malloc((size_t)(eq - p) + 1);
            if (!name)
                continue;
            memcpy(name, p, (size_t)(eq - p));
            name[eq - p] = '\0';
            val = xstrdup(eq + 1);
            if (n + 1 > cap) {
                size_t ncap = cap ? cap * 2 : 64;
                char **nn = (char **)realloc(names, ncap * sizeof(char *));
                char **nv = (char **)realloc(vals, ncap * sizeof(char *));
                if (!nn || !nv) {
                    free(name);
                    free(val);
                    free(nn);
                    free(nv);
                    break;
                }
                names = nn;
                vals = nv;
                cap = ncap;
            }
            names[n] = name;
            vals[n] = val ? val : xstrdup("");
            n++;
        }
        FreeEnvironmentStringsA(block);
    }
#else
    {
        extern char **environ;
        size_t e;
        for (e = 0; environ && environ[e]; ++e) {
            char *eq = strchr(environ[e], '=');
            char *name, *val;
            if (!eq)
                continue;
            name = (char *)malloc((size_t)(eq - environ[e]) + 1);
            if (!name)
                continue;
            memcpy(name, environ[e], (size_t)(eq - environ[e]));
            name[eq - environ[e]] = '\0';
            val = xstrdup(eq + 1);
            if (n + 1 > cap) {
                size_t ncap = cap ? cap * 2 : 64;
                char **nn = (char **)realloc(names, ncap * sizeof(char *));
                char **nv = (char **)realloc(vals, ncap * sizeof(char *));
                if (!nn || !nv) {
                    free(name);
                    free(val);
                    break;
                }
                names = nn;
                vals = nv;
                cap = ncap;
            }
            names[n] = name;
            vals[n] = val ? val : xstrdup("");
            n++;
        }
    }
#endif

    /* sort by name (bubble + swap vals) */
    for (i = 0; i < n; ++i) {
        size_t j;
        for (j = i + 1; j < n; ++j) {
            if (strcmp(names[i], names[j]) > 0) {
                char *tn = names[i], *tv = vals[i];
                names[i] = names[j];
                vals[i] = vals[j];
                names[j] = tn;
                vals[j] = tv;
            }
        }
    }

    for (i = 0; i < n; ++i) {
        if (i)
            db_append_str(&ser, "\n");
        db_append_str(&ser, names[i]);
        db_append_str(&ser, "=");
        db_append_str(&ser, vals[i] ? vals[i] : "");
        if (strncmp(names[i], "CNET_", 5) == 0 || strncmp(names[i], "CCE_", 4) == 0)
            sl_add(&eb->knobs, names[i]);
    }
    /* knobs already in name-sorted order from the sorted walk */
    sha256_init(&ctx);
    if (ser.data)
        sha256_update(&ctx, ser.data, ser.len);
    sha256_final(&ctx, hash);
    for (i = 0; i < 32; ++i)
        sprintf(eb->env_sha256 + i * 2, "%02x", hash[i]);
    eb->env_sha256[64] = '\0';
    eb->env_variables = (int)n;

    for (i = 0; i < n; ++i) {
        free(names[i]);
        free(vals[i]);
    }
    free(names);
    free(vals);
    db_free(&ser);
}

/* -------------------------------------------------------------------------- */
/* JSON                                                                       */
/* -------------------------------------------------------------------------- */

static int json_escape_append(DynBuf *b, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (db_append_str(b, "\"") != 0)
        return -1;
    for (; *p; ++p) {
        char esc[8];
        switch (*p) {
        case '\\':
            if (db_append_str(b, "\\\\") != 0)
                return -1;
            break;
        case '"':
            if (db_append_str(b, "\\\"") != 0)
                return -1;
            break;
        case '\n':
            if (db_append_str(b, "\\n") != 0)
                return -1;
            break;
        case '\r':
            if (db_append_str(b, "\\r") != 0)
                return -1;
            break;
        case '\t':
            if (db_append_str(b, "\\t") != 0)
                return -1;
            break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", *p);
                if (db_append_str(b, esc) != 0)
                    return -1;
            } else {
                if (db_append(b, p, 1) != 0)
                    return -1;
            }
        }
    }
    return db_append_str(b, "\"");
}

static int json_str_array(DynBuf *b, char *const *items, size_t n) {
    size_t i;
    if (db_append_str(b, "[") != 0)
        return -1;
    for (i = 0; i < n; ++i) {
        if (i && db_append_str(b, ", ") != 0)
            return -1;
        if (json_escape_append(b, items[i] ? items[i] : "") != 0)
            return -1;
    }
    return db_append_str(b, "]");
}

static int json_binding_obj(DynBuf *b, const Binding *bind) {
    if (db_append_str(b, "{\n") != 0)
        return -1;
    if (db_append_fmt(b, "    \"commit\": ") != 0)
        return -1;
    if (json_escape_append(b, bind->commit) != 0)
        return -1;
    if (db_append_fmt(b,
                      ",\n    \"special_index_bytes\": %llu,\n"
                      "    \"special_index_files\": %d,\n"
                      "    \"special_index_sha256\": ",
                      (unsigned long long)bind->special_index_bytes,
                      bind->special_index_files) != 0)
        return -1;
    if (json_escape_append(b, bind->special_index_sha256) != 0)
        return -1;
    if (db_append_str(b, ",\n    \"untracked_sha256\": ") != 0)
        return -1;
    if (json_escape_append(b, bind->untracked_sha256) != 0)
        return -1;
    if (db_append_fmt(b,
                      ",\n    \"worktree_dirty_files\": %d,\n"
                      "    \"worktree_sha256\": ",
                      bind->worktree_dirty_files) != 0)
        return -1;
    if (json_escape_append(b, bind->worktree_sha256) != 0)
        return -1;
    return db_append_str(b, "\n  }");
}

static int binding_field_eq(const Binding *a, const Binding *b, const char *key) {
    if (strcmp(key, "commit") == 0)
        return strcmp(a->commit, b->commit) == 0;
    if (strcmp(key, "worktree_sha256") == 0)
        return strcmp(a->worktree_sha256, b->worktree_sha256) == 0;
    if (strcmp(key, "worktree_dirty_files") == 0)
        return a->worktree_dirty_files == b->worktree_dirty_files;
    if (strcmp(key, "special_index_sha256") == 0)
        return strcmp(a->special_index_sha256, b->special_index_sha256) == 0;
    if (strcmp(key, "special_index_files") == 0)
        return a->special_index_files == b->special_index_files;
    if (strcmp(key, "special_index_bytes") == 0)
        return a->special_index_bytes == b->special_index_bytes;
    if (strcmp(key, "untracked_sha256") == 0)
        return strcmp(a->untracked_sha256, b->untracked_sha256) == 0;
    return 1;
}

static void compute_drift(const Binding *pre, const Binding *post, StrList *drift) {
    static const char *keys[] = {
        "commit",
        "special_index_bytes",
        "special_index_files",
        "special_index_sha256",
        "untracked_sha256",
        "worktree_dirty_files",
        "worktree_sha256",
        NULL
    };
    size_t i;
    for (i = 0; keys[i]; ++i) {
        if (!binding_field_eq(pre, post, keys[i]))
            sl_add(drift, keys[i]);
    }
}

/* -------------------------------------------------------------------------- */
/* UUID / time                                                                */
/* -------------------------------------------------------------------------- */

static void make_run_id(char out[37]) {
    unsigned char b[16];
    size_t i;
    int filled = 0;
#ifndef _WIN32
    {
        FILE *ur = fopen("/dev/urandom", "rb");
        if (ur) {
            if (fread(b, 1, sizeof(b), ur) == sizeof(b))
                filled = 1;
            fclose(ur);
        }
    }
#endif
    if (!filled) {
        unsigned seed = (unsigned)time(NULL);
#ifdef _WIN32
        seed ^= (unsigned)GetCurrentProcessId();
        seed ^= (unsigned)(uintptr_t)&seed;
#else
        seed ^= (unsigned)getpid();
        seed ^= (unsigned)(uintptr_t)&seed;
#endif
        srand(seed);
        for (i = 0; i < sizeof(b); ++i)
            b[i] = (unsigned char)(rand() & 0xff);
    }
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    sprintf(out,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
            b[12], b[13], b[14], b[15]);
}

static void utc_now(char out[32]) {
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* -------------------------------------------------------------------------- */
/* main                                                                       */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *gate, *log_arg, *marker;
    char **command;
    int cmdc;
    char root[4096];
    char evidence_path[4224];
    char run_id[40];
    char started[32];
    Binding pre, post;
    DynBuf output = {0};
    int status = 127;
    int present;
    StrList conflicts = {0};
    StrList drift = {0};
    EnvBind envb;
    char evidence_sha[65];
    FILE *logf;
    DynBuf json = {0};
    size_t i;

    if (argc < 6 || strcmp(argv[4], "--") != 0) {
        fprintf(stderr, "usage: gate_evidence GATE LOG MARKER -- CMD [ARG...]\n");
        return 2;
    }
    gate = argv[1];
    log_arg = argv[2];
    marker = argv[3];
    command = &argv[5];
    cmdc = argc - 5;

    if (find_root(root, sizeof(root), argv[0]) != 0) {
        fprintf(stderr, "gate_evidence: cannot resolve repository root\n");
        return 1;
    }

    mkdir_p_parent(log_arg);
    unlink_quiet(log_arg);
    snprintf(evidence_path, sizeof(evidence_path), "%s.evidence.json", log_arg);
    unlink_quiet(evidence_path);

    make_run_id(run_id);
    utc_now(started);
    capture_binding(root, &pre);

    if (pre.special_index_bytes > SPECIAL_INDEX_BYTE_CAP) {
        printf("GATE_FAIL gate=%s reason=special_index_too_large:%llu>%llu "
               "refusing to produce a binding that does not cover the tree\n",
               gate, (unsigned long long)pre.special_index_bytes,
               (unsigned long long)SPECIAL_INDEX_BYTE_CAP);
        fflush(stdout);
        return 1;
    }

    printf("GATE_RUN gate=%s run_id=%s started=%s commit=%s worktree=%.16s "
           "untracked=%.16s special_index=%.16s dirty=%d special_index_files=%d\n",
           gate, run_id, started, pre.commit, pre.worktree_sha256, pre.untracked_sha256,
           pre.special_index_sha256, pre.worktree_dirty_files, pre.special_index_files);
    fflush(stdout);

    if (run_capture(command, root, &output, &status) != 0) {
        db_free(&output);
        db_append_fmt(&output, "gate_evidence: cannot run command\n");
        status = 127;
    }

    logf = fopen(log_arg, "wb");
    if (logf) {
        if (output.data && output.len)
            fwrite(output.data, 1, output.len, logf);
        fclose(logf);
    }
    if (output.data && output.len) {
        fwrite(output.data, 1, output.len, stdout);
        fflush(stdout);
    }

    capture_binding(root, &post);
    present = marker_lines(output.data ? output.data : "", marker);
    conflicting_verdicts(output.data ? output.data : "", marker, &conflicts);
    compute_drift(&pre, &post, &drift);

    if (sha256_file_hex(log_arg, evidence_sha) != 0)
        strncpy(evidence_sha, "absent", sizeof(evidence_sha));

    environment_binding(&envb);

    /* Write evidence JSON (sorted keys, indent=2) */
    db_append_str(&json, "{\n");
    db_append_str(&json, "  \"binding_drift\": ");
    json_str_array(&json, drift.items, drift.n);
    db_append_str(&json, ",\n  \"binding_post\": ");
    json_binding_obj(&json, &post);
    db_append_str(&json, ",\n  \"binding_pre\": ");
    json_binding_obj(&json, &pre);
    db_append_fmt(&json, ",\n  \"binding_stable\": %s,\n  \"command\": ",
                  drift.n == 0 ? "true" : "false");
    json_str_array(&json, command, (size_t)cmdc);
    db_append_str(&json, ",\n  \"conflicting_markers\": ");
    json_str_array(&json, conflicts.items, conflicts.n);
    db_append_str(&json, ",\n  \"env_knob_names\": ");
    json_str_array(&json, envb.knobs.items, envb.knobs.n);
    db_append_str(&json, ",\n  \"env_sha256\": ");
    json_escape_append(&json, envb.env_sha256);
    db_append_fmt(&json, ",\n  \"env_variables\": %d,\n  \"evidence_log\": ",
                  envb.env_variables);
    json_escape_append(&json, log_arg);
    db_append_str(&json, ",\n  \"evidence_sha256\": ");
    json_escape_append(&json, evidence_sha);
    db_append_fmt(&json, ",\n  \"exit_status\": %d,\n  \"gate\": ", status);
    json_escape_append(&json, gate);
    db_append_fmt(&json, ",\n  \"marker_lines\": %d,\n  \"marker_present\": %s,\n",
                  present, present == 1 ? "true" : "false");
    db_append_str(&json, "  \"required_marker\": ");
    json_escape_append(&json, marker);
    db_append_str(&json, ",\n  \"run_id\": ");
    json_escape_append(&json, run_id);
    db_append_str(&json, ",\n  \"started_at\": ");
    json_escape_append(&json, started);
    db_append_str(&json, "\n}\n");

    {
        FILE *ef = fopen(evidence_path, "wb");
        if (ef) {
            if (json.data)
                fwrite(json.data, 1, json.len, ef);
            fclose(ef);
        }
    }

    if (status != 0) {
        printf("GATE_FAIL gate=%s reason=producer_exit_%d evidence=%s\n", gate, status,
               evidence_path);
        fflush(stdout);
        goto done_fail_producer;
    }
    if (present == 0) {
        printf("GATE_FAIL gate=%s reason=missing_marker:%s evidence=%s\n", gate, marker,
               evidence_path);
        fflush(stdout);
        status = 1;
        goto done_fail;
    }
    if (present > 1) {
        printf("GATE_FAIL gate=%s reason=marker_repeated_%dx:%s evidence=%s\n", gate,
               present, marker, evidence_path);
        fflush(stdout);
        status = 1;
        goto done_fail;
    }
    if (conflicts.n) {
        DynBuf reason = {0};
        for (i = 0; i < conflicts.n; ++i) {
            if (i)
                db_append_str(&reason, ",");
            db_append_str(&reason, conflicts.items[i]);
        }
        printf("GATE_FAIL gate=%s reason=conflicting_verdicts:%s evidence=%s\n", gate,
               reason.data ? reason.data : "", evidence_path);
        fflush(stdout);
        db_free(&reason);
        status = 1;
        goto done_fail;
    }
    if (drift.n) {
        DynBuf reason = {0};
        for (i = 0; i < drift.n; ++i) {
            if (i)
                db_append_str(&reason, ",");
            db_append_str(&reason, drift.items[i]);
        }
        printf("GATE_FAIL gate=%s reason=bound_state_changed:%s evidence=%s\n", gate,
               reason.data ? reason.data : "", evidence_path);
        fflush(stdout);
        db_free(&reason);
        status = 1;
        goto done_fail;
    }

    printf("GATE_PASS gate=%s run_id=%s commit=%s evidence_sha256=%s\n", gate, run_id,
           pre.commit, evidence_sha);
    fflush(stdout);
    status = 0;

done_fail:
done_fail_producer:
    db_free(&output);
    db_free(&json);
    sl_free(&conflicts);
    sl_free(&drift);
    sl_free(&envb.knobs);
    return status;
}
