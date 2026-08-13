/* Resumable chunk-tree identity for very large model artifacts.
 *
 * Usage: cnet_chunk_hash MODEL IDENTITY_FILE
 * Env: CNET_HASH_CHUNK_BYTES (default 1<<30)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <process.h>
#define MKDIR(p) _mkdir(p)
#define FSYNC(fd) _commit(fd)
#define FILENO(f) _fileno(f)
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#else
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#define FSYNC(fd) fsync(fd)
#define FILENO(f) fileno(f)
#endif

#define DEFAULT_CHUNK_BYTES (1ull << 30)
#define READ_BYTES (8ull << 20)

/* ---- SHA-256 ---- */
typedef struct {
    uint32_t h[8];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
} Sha256;

static const uint32_t SHA_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static void sha_transform(Sha256 *c, const unsigned char b[64]) {
    uint32_t w[64], a, d, e, f, g, h, i0, j, t1, t2;
    for (j = 0; j < 16; ++j)
        w[j] = ((uint32_t)b[j*4]<<24)|((uint32_t)b[j*4+1]<<16)|
               ((uint32_t)b[j*4+2]<<8)|(uint32_t)b[j*4+3];
    for (; j < 64; ++j) {
        uint32_t s0 = rotr(w[j-15],7)^rotr(w[j-15],18)^(w[j-15]>>3);
        uint32_t s1 = rotr(w[j-2],17)^rotr(w[j-2],19)^(w[j-2]>>10);
        w[j] = w[j-16] + s0 + w[j-7] + s1;
    }
    a=c->h[0]; i0=c->h[1]; d=c->h[2]; e=c->h[3];
    f=c->h[4]; g=c->h[5]; h=c->h[6]; j=c->h[7];
    for (uint32_t r = 0; r < 64; ++r) {
        uint32_t s1 = rotr(f,6)^rotr(f,11)^rotr(f,25);
        uint32_t ch = (f&g)^((~f)&h);
        uint32_t s0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
        uint32_t maj = (a&i0)^(a&d)^(i0&d);
        t1 = j + s1 + ch + SHA_K[r] + w[r];
        t2 = s0 + maj;
        j=h; h=g; g=f; f=e+t1; e=d; d=i0; i0=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=i0; c->h[2]+=d; c->h[3]+=e;
    c->h[4]+=f; c->h[5]+=g; c->h[6]+=h; c->h[7]+=j;
}

static void sha_init(Sha256 *c) {
    static const uint32_t init[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(c->h, init, sizeof init);
    c->bits = 0; c->used = 0;
}

static void sha_update(Sha256 *c, const unsigned char *p, size_t n) {
    c->bits += (uint64_t)n * 8u;
    while (n > 0) {
        size_t take = 64u - c->used;
        if (take > n) take = n;
        memcpy(c->block + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64u) { sha_transform(c, c->block); c->used = 0; }
    }
}

static void sha_final(Sha256 *c, unsigned char out[32]) {
    uint64_t bits = c->bits;
    unsigned i;
    c->block[c->used++] = 0x80u;
    if (c->used > 56u) {
        memset(c->block + c->used, 0, 64u - c->used);
        sha_transform(c, c->block);
        c->used = 0;
    }
    memset(c->block + c->used, 0, 56u - c->used);
    for (i = 0; i < 8; ++i)
        c->block[63u - i] = (unsigned char)(bits >> (i * 8u));
    sha_transform(c, c->block);
    for (i = 0; i < 8; ++i) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)c->h[i];
    }
}

static void hex_of(const unsigned char d[32], char hex[65]) {
    static const char hd[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        hex[i*2] = hd[d[i] >> 4];
        hex[i*2+1] = hd[d[i] & 15];
    }
    hex[64] = 0;
}

static void die(const char *msg) {
    fprintf(stderr, "cnet_chunk_hash: %s\n", msg);
    exit(1);
}

static void ensure_parent(const char *path) {
    char tmp[1024];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
}

static uint64_t mtime_ns_of(const struct stat *st) {
#ifdef _WIN32
    return (uint64_t)st->st_mtime * 1000000000ull;
#else
#if defined(st_mtime)
    #ifdef __APPLE__
    return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ull +
           (uint64_t)st->st_mtimespec.tv_nsec;
    #else
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull +
           (uint64_t)st->st_mtim.tv_nsec;
    #endif
#else
    return (uint64_t)st->st_mtime * 1000000000ull;
#endif
#endif
}

typedef struct {
    int valid;
    int64_t size;
    char sha256[65];
} Rec;

static int parse_header_line(const char *line, int64_t *size, int64_t *mtime_ns,
                             int64_t *chunk_bytes) {
    /* {"chunk_bytes":N,"mtime_ns":N,"size":N,"version":1} sort_keys */
    const char *p;
    *size = *mtime_ns = *chunk_bytes = -1;
    p = strstr(line, "\"chunk_bytes\"");
    if (p) { p = strchr(p, ':'); if (p) *chunk_bytes = (int64_t)strtoll(p + 1, NULL, 10); }
    p = strstr(line, "\"mtime_ns\"");
    if (p) { p = strchr(p, ':'); if (p) *mtime_ns = (int64_t)strtoll(p + 1, NULL, 10); }
    p = strstr(line, "\"size\"");
    if (p) { p = strchr(p, ':'); if (p) *size = (int64_t)strtoll(p + 1, NULL, 10); }
    p = strstr(line, "\"version\"");
    if (!p) return 0;
    return (*size >= 0 && *mtime_ns >= 0 && *chunk_bytes > 0);
}

static int load_records(const char *state_path, int64_t size, int64_t mtime_ns,
                        int64_t chunk_bytes, Rec **out_recs, int64_t chunks) {
    FILE *f = fopen(state_path, "r");
    char line[512];
    int64_t hs, hm, hc;
    Rec *recs;
    int64_t i;
    if (!f) return 0;
    if (!fgets(line, sizeof line, f) ||
        !parse_header_line(line, &hs, &hm, &hc) ||
        hs != size || hm != mtime_ns || hc != chunk_bytes) {
        fclose(f);
        return 0;
    }
    recs = (Rec *)calloc((size_t)chunks, sizeof(Rec));
    if (!recs) { fclose(f); return 0; }
    while (fgets(line, sizeof line, f)) {
        int64_t index = -1, rsize = -1;
        char digest[65] = {0};
        const char *p;
        p = strstr(line, "\"index\"");
        if (!p) { free(recs); fclose(f); return 0; }
        p = strchr(p, ':'); if (!p) { free(recs); fclose(f); return 0; }
        index = (int64_t)strtoll(p + 1, NULL, 10);
        p = strstr(line, "\"size\"");
        if (!p) { free(recs); fclose(f); return 0; }
        p = strchr(p, ':'); if (!p) { free(recs); fclose(f); return 0; }
        rsize = (int64_t)strtoll(p + 1, NULL, 10);
        p = strstr(line, "\"sha256\"");
        if (!p) { free(recs); fclose(f); return 0; }
        p = strchr(p, ':'); if (!p) { free(recs); fclose(f); return 0; }
        while (*p && *p != '"') p++;
        if (*p == '"') p++;
        {
            int k = 0;
            while (*p && *p != '"' && k < 64) digest[k++] = *p++;
            digest[k] = 0;
        }
        if (index < 0 || index >= chunks || rsize < 0 || strlen(digest) != 64) {
            free(recs); fclose(f); return 0;
        }
        {
            int ok = 1, k;
            for (k = 0; k < 64; k++) {
                char c = digest[k];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) ok = 0;
            }
            if (!ok) { free(recs); fclose(f); return 0; }
        }
        recs[index].valid = 1;
        recs[index].size = rsize;
        memcpy(recs[index].sha256, digest, 65);
    }
    fclose(f);
    for (i = 0; i < chunks; i++) {
        int64_t expected = chunk_bytes;
        if (i == chunks - 1) {
            int64_t rem = size - i * chunk_bytes;
            if (rem < expected) expected = rem;
        } else if (i * chunk_bytes >= size) {
            free(recs); return 0;
        }
        if (i < chunks) {
            int64_t exp2 = size - i * chunk_bytes;
            if (exp2 > chunk_bytes) exp2 = chunk_bytes;
            if (recs[i].valid && recs[i].size != exp2) {
                free(recs); return 0;
            }
        }
        (void)expected;
    }
    *out_recs = recs;
    return 1;
}

static void initialize_state(const char *state_path, int64_t size, int64_t mtime_ns,
                             int64_t chunk_bytes) {
    char tmp[1100];
    FILE *f;
    ensure_parent(state_path);
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", state_path, (int)getpid());
    f = fopen(tmp, "w");
    if (!f) die("cannot write state");
    /* sort_keys: chunk_bytes, mtime_ns, size, version */
    fprintf(f, "{\"chunk_bytes\":%lld,\"mtime_ns\":%lld,\"size\":%lld,\"version\":1}\n",
            (long long)chunk_bytes, (long long)mtime_ns, (long long)size);
    fflush(f);
    FSYNC(FILENO(f));
    fclose(f);
#ifdef _WIN32
    remove(state_path);
#endif
    if (rename(tmp, state_path) != 0) die("rename state failed");
}

static void append_record(const char *state_path, int64_t index, int64_t size,
                          const char *digest) {
    FILE *f = fopen(state_path, "a");
    if (!f) die("cannot append state");
    /* sort_keys: index, sha256, size */
    fprintf(f, "{\"index\":%lld,\"sha256\":\"%s\",\"size\":%lld}\n",
            (long long)index, digest, (long long)size);
    fflush(f);
    FSYNC(FILENO(f));
    fclose(f);
}

static void hash_chunk(FILE *stream, int64_t offset, int64_t size, char hex[65]) {
    unsigned char buf[1 << 16];
    Sha256 c;
    int64_t remaining = size;
    unsigned char dig[32];
#ifdef _WIN32
    _fseeki64(stream, offset, SEEK_SET);
#else
    fseeko(stream, (off_t)offset, SEEK_SET);
#endif
    sha_init(&c);
    while (remaining > 0) {
        size_t want = (size_t)(remaining > (int64_t)sizeof buf ? sizeof buf : remaining);
        size_t n = fread(buf, 1, want, stream);
        if (n == 0) die("unexpected EOF");
        sha_update(&c, buf, n);
        remaining -= (int64_t)n;
    }
    sha_final(&c, dig);
    hex_of(dig, hex);
}

static void write_identity(const char *identity_path, const char *identity) {
    char tmp[1100];
    FILE *f;
    ensure_parent(identity_path);
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", identity_path, (int)getpid());
    f = fopen(tmp, "w");
    if (!f) die("cannot write identity");
    fprintf(f, "%s\n", identity);
    fflush(f);
    FSYNC(FILENO(f));
    fclose(f);
#ifdef _WIN32
    remove(identity_path);
#endif
    if (rename(tmp, identity_path) != 0) die("rename identity failed");
}

static void be64(unsigned char out[8], uint64_t v) {
    int i;
    for (i = 7; i >= 0; i--) { out[i] = (unsigned char)(v & 0xff); v >>= 8; }
}

int main(int argc, char **argv) {
    const char *model_path, *identity_path;
    char state_path[1100];
    struct stat st;
    int64_t chunk_bytes = (int64_t)DEFAULT_CHUNK_BYTES;
    int64_t size, mtime_ns, chunks, index;
    Rec *recs = NULL;
    int reused = 0, hashed = 0;
    FILE *mf;
    const char *env;
    Sha256 root;
    unsigned char dig[32], be[8];
    char identity[96], hex[65];

    if (argc != 3) die("usage: cnet_chunk_hash MODEL IDENTITY_FILE");
    model_path = argv[1];
    identity_path = argv[2];
    if (stat(model_path, &st) != 0 || !S_ISREG(st.st_mode))
        die("model not found");
    env = getenv("CNET_HASH_CHUNK_BYTES");
    if (env && env[0]) {
        chunk_bytes = (int64_t)strtoll(env, NULL, 10);
        if (chunk_bytes <= 0) die("CNET_HASH_CHUNK_BYTES must be positive");
    }
    size = (int64_t)st.st_size;
    mtime_ns = (int64_t)mtime_ns_of(&st);
    chunks = (size + chunk_bytes - 1) / chunk_bytes;
    if (size == 0) chunks = 0;

    snprintf(state_path, sizeof state_path, "%s.chunks.jsonl", identity_path);
    if (!load_records(state_path, size, mtime_ns, chunk_bytes, &recs, chunks > 0 ? chunks : 1)) {
        initialize_state(state_path, size, mtime_ns, chunk_bytes);
        free(recs);
        recs = (Rec *)calloc(chunks > 0 ? (size_t)chunks : 1, sizeof(Rec));
        if (!recs) die("oom");
    } else {
        int64_t i;
        for (i = 0; i < chunks; i++) if (recs[i].valid) reused++;
    }

    mf = fopen(model_path, "rb");
    if (!mf) die("cannot open model");
    for (index = 0; index < chunks; index++) {
        int64_t offset = index * chunk_bytes;
        int64_t csize = size - offset;
        if (csize > chunk_bytes) csize = chunk_bytes;
        if (recs[index].valid) continue;
        hash_chunk(mf, offset, csize, hex);
        append_record(state_path, index, csize, hex);
        recs[index].valid = 1;
        recs[index].size = csize;
        memcpy(recs[index].sha256, hex, 65);
        hashed++;
        printf("chunk=%lld/%lld bytes=%lld sha256=%s\n",
               (long long)(index + 1), (long long)chunks,
               (long long)csize, hex);
        fflush(stdout);
    }
    fclose(mf);

    sha_init(&root);
    sha_update(&root, (const unsigned char *)"cnet-sha256-tree-v1", 18);
    { unsigned char z = 0; sha_update(&root, &z, 1); }
    be64(be, (uint64_t)size); sha_update(&root, be, 8);
    be64(be, (uint64_t)chunk_bytes); sha_update(&root, be, 8);
    for (index = 0; index < chunks; index++) {
        int k;
        unsigned char raw[32];
        be64(be, (uint64_t)index); sha_update(&root, be, 8);
        be64(be, (uint64_t)recs[index].size); sha_update(&root, be, 8);
        for (k = 0; k < 32; k++) {
            unsigned v = 0;
            char a = recs[index].sha256[k*2], b = recs[index].sha256[k*2+1];
            v = (a >= 'a' ? a - 'a' + 10 : a - '0');
            v = (v << 4) | (b >= 'a' ? b - 'a' + 10 : b - '0');
            raw[k] = (unsigned char)v;
        }
        sha_update(&root, raw, 32);
    }
    sha_final(&root, dig);
    hex_of(dig, hex);
    snprintf(identity, sizeof identity, "sha256-tree-v1:%s", hex);
    write_identity(identity_path, identity);
    printf("identity=%s chunks=%lld reused=%d hashed=%d\n",
           identity, (long long)chunks, reused, hashed);
    free(recs);
    return 0;
}
