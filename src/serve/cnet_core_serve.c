/* Light .lut brick bank — live RESULT without GGUF/admit link weight. */
#include "cnet_core_serve.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || !cap) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void cnet_serve_bank_init(CnetServeBank *b) {
    if (!b) return;
    memset(b, 0, sizeof *b);
}

static int load_one_lut(const char *path, CnetServeBrick *br) {
    FILE *f;
    char line[512];
    int got_lut = 0;
    if (!path || !br) return -1;
    memset(br, 0, sizeof *br);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && !feof(f)) goto invalid;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (strncmp(line, "tag=", 4) == 0) {
            char *p = line + 4;
            while (*p == ' ') p++;
            copy_text(br->tag, sizeof br->tag, p);
            { char *nl = strchr(br->tag, '\n'); if (nl) *nl = 0; }
        } else if (strncmp(line, "name=", 5) == 0) {
            char *p = line + 5;
            while (*p == ' ') p++;
            copy_text(br->name, sizeof br->name, p);
            { char *nl = strchr(br->name, '\n'); if (nl) *nl = 0; }
        } else if (strncmp(line, "lut=", 4) == 0) {
            char *p = line + 4;
            int i;
            if (got_lut) goto invalid;
            for (i = 0; i < 16; ++i) {
                char *end;
                double value;
                while (*p == ' ' || *p == '\t') p++;
                errno = 0;
                value = strtod(p, &end);
                if (end == p || errno || !isfinite(value) || value < 0 ||
                    value > 15 || floor(value) != value) goto invalid;
                br->lut[i] = (float)value;
                p = end;
                while (*p == ' ' || *p == '\t') p++;
                if (i != 15) { if (*p++ != ',') goto invalid; }
            }
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p) goto invalid;
            got_lut = 1;
        }
    }
    if (ferror(f)) goto invalid;
    fclose(f);
    if (!got_lut || !br->tag[0]) return -1;
    if (!br->name[0]) copy_text(br->name, sizeof br->name, br->tag);
    br->live = 1;
    return 0;
invalid:
    fclose(f);
    return -1;
}

int cnet_serve_save_lut(const char *dir, const char *tag, const char *name,
                        const float lut[16]) {
#ifdef _WIN32
    (void)dir; (void)tag; (void)name; (void)lut;
    fprintf(stderr, "BRICK_PUBLISH_FAILED unsupported_atomic_publisher=1\n");
    errno = ENOSYS;
    return -1;
#else
    char path[768], filename[CNET_SERVE_TAG + 5], temporary[64] = "";
    CnetServeBank bank;
    CnetServeBrick previous;
    struct stat st;
    FILE *f = NULL;
    int i, dfd = -1, fd = -1, exists, rc = -1, created = 0, published = 0;
    if (!dir || !dir[0] || !tag || !tag[0] || tag[0] == '.' || !lut ||
        strlen(tag) >= CNET_SERVE_TAG || strpbrk(tag, "/\\\r\n")) goto done;
    if (!(isalpha((unsigned char)tag[0]) || tag[0] == '_')) goto done;
    for (const unsigned char *p = (const unsigned char *)tag; *p; p++)
        if (isspace(*p) || iscntrl(*p)) goto done;
    if (!name || !name[0]) name = tag;
    if (strlen(name) >= CNET_SERVE_NAME || strpbrk(name, "\r\n")) goto done;
    for (i = 0; i < 16; i++)
        if (!isfinite(lut[i]) || lut[i] < 0 || lut[i] > 15 || floorf(lut[i]) != lut[i])
            goto done;
    if (snprintf(path, sizeof path, "%s/%s.lut", dir, tag) >= (int)sizeof path)
        goto done;
    snprintf(filename, sizeof filename, "%s.lut", tag);
    cnet_mkdir(dir, 0755);
    /* Serialize cooperating publishers on the directory inode, with no lock
     * artifact. Readers see the old or new complete file through renameat. */
    dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0 || flock(dfd, LOCK_EX) != 0) goto done;
    if (cnet_serve_bank_load_dir(&bank, dir) != 0) goto done;
    exists = fstatat(dfd, filename, &st, AT_SYMLINK_NOFOLLOW) == 0;
    if (!exists && errno != ENOENT) goto done;
    if (exists && (!S_ISREG(st.st_mode) || load_one_lut(path, &previous) != 0 ||
                   strcmp(previous.tag, tag) != 0)) goto done;
    if (!exists) {
        if (bank.n >= CNET_SERVE_MAX_BRICKS) goto done;
        for (i = 0; i < bank.n; i++)
            if (strcmp(bank.bricks[i].tag, tag) == 0) goto done;
    }
    for (i = 0; i < 32; i++) {
        snprintf(temporary, sizeof temporary, ".cnet-lut-%ld-%d.tmp", (long)getpid(), i);
        fd = openat(dfd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) { created = 1; break; }
        if (errno != EEXIST) goto done;
    }
    if (fd < 0) goto done;
    f = fdopen(fd, "w");
    if (!f) goto done;
    fd = -1; /* owned by f */
    fprintf(f, "# CNET_LUT_V1\n");
    fprintf(f, "tag=%s\n", tag);
    fprintf(f, "name=%s\n", name);
    fprintf(f, "lut=");
    for (i = 0; i < 16; ++i)
        fprintf(f, "%s%g", i ? "," : "", (double)lut[i]);
    fprintf(f, "\n");
    if (ferror(f) || fflush(f) != 0 || fsync(fileno(f)) != 0) goto done;
    if (fclose(f) != 0) { f = NULL; goto done; }
    f = NULL;
    if (renameat(dfd, temporary, dfd, filename) != 0) goto done;
    created = 0;
    published = 1;
    rc = fsync(dfd) == 0 ? 0 : -1; /* a durability failure is reported, never hidden */
done:
    if (f) fclose(f);
    if (fd >= 0) close(fd);
    if (created) unlinkat(dfd, temporary, 0);
    if (dfd >= 0) close(dfd);
    if (rc != 0)
        fprintf(stderr, "BRICK_PUBLISH_FAILED capacity=%d published=%d\n",
                CNET_SERVE_MAX_BRICKS, published);
    return rc;
#endif
}

int cnet_serve_bank_load_dir(CnetServeBank *b, const char *dir) {
    DIR *d;
    struct dirent *e;
    if (!b || !dir || !dir[0]) return -1;
    cnet_serve_bank_init(b);
    if (strlen(dir) >= sizeof b->dir) return -1;
    copy_text(b->dir, sizeof b->dir, dir);
    d = opendir(dir);
    if (!d) return -1;
    for (;;) {
        char path[768];
        CnetServeBrick br;
        size_t n;
        errno = 0;
        e = readdir(d);
        if (!e) { if (errno) goto invalid; break; }
        if (e->d_name[0] == '.') continue;
        n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".lut") != 0) continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path)
            goto invalid;
        if (load_one_lut(path, &br) != 0) {
            fprintf(stderr, "cnet_serve: rejected malformed table %s\n", path);
            goto invalid;
        }
        if (b->n >= CNET_SERVE_MAX_BRICKS) {
            fprintf(stderr,
                    "cnet_serve: lut bank full max=%d skipped=%s (fail-loud)\n",
                    CNET_SERVE_MAX_BRICKS, e->d_name);
            goto invalid;
        }
        for (int i = 0; i < b->n; i++)
            if (strcmp(b->bricks[i].tag, br.tag) == 0) {
                fprintf(stderr, "cnet_serve: duplicate table tag (fail-loud)\n");
                goto invalid;
            }
        b->bricks[b->n++] = br;
    }
    closedir(d);
    b->reloads++;
    return 0;
invalid:
    closedir(d);
    cnet_serve_bank_init(b);
    return -1;
}

int cnet_serve_bank_reload(CnetServeBank *b) {
    CnetServeBank next;
    if (!b || !b->dir[0]) return -1;
    if (cnet_serve_bank_load_dir(&next, b->dir) != 0) return -1;
    next.proves = b->proves;
    next.abstains = b->abstains;
    next.reloads = b->reloads + 1;
    *b = next;
    return 0;
}

static int parse_turn(const char *turn, char *tag_out, size_t tag_cap,
                      unsigned *out_x) {
    const char *p = turn;
    unsigned v = 0;
    int saw = 0;
    char tag[32];
    size_t ti = 0;
    if (!turn || !out_x) return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (isalpha((unsigned char)*p) || *p == '_') {
        while (*p && *p != ' ' && *p != '\t' && ti + 1 < sizeof tag)
            tag[ti++] = *p++;
        tag[ti] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (tag_out && tag_cap) copy_text(tag_out, tag_cap, tag);
    } else if (tag_out && tag_cap)
        tag_out[0] = 0;
    if (!isdigit((unsigned char)*p)) return -1;
    while (isdigit((unsigned char)*p)) {
        saw = 1;
        v = v * 10u + (unsigned)(*p - '0');
        if (v > 15u) return -1;
        p++;
    }
    if (!saw) return -1;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p) return -1;
    *out_x = v;
    return 0;
}

int cnet_serve_owns(const CnetServeBank *b, const char *turn) {
    const char *p;
    char tag[CNET_SERVE_TAG];
    size_t ti = 0;
    int i;
    if (!b || !turn) return 0;
    p = turn;
    while (*p == ' ' || *p == '\t') p++;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return 0;
    while (*p && *p != ' ' && *p != '\t' && ti + 1 < sizeof tag)
        tag[ti++] = *p++;
    tag[ti] = 0;
    if (!tag[0]) return 0;
    for (i = 0; i < b->n; ++i) {
        if (!b->bricks[i].live) continue;
        if (strcmp(b->bricks[i].tag, tag) == 0) return 1;
    }
    return 0;
}

int cnet_serve_compose_tags(CnetServeBank *b, const char *tag_a, const char *tag_b,
                            const char *tag_out) {
    int ia = -1, ib = -1, i;
    float lut[16];
    if (!b || !tag_a || !tag_b || !tag_out || !tag_out[0]) return -1;
    for (i = 0; i < b->n; ++i) {
        if (!b->bricks[i].live) continue;
        if (strcmp(b->bricks[i].tag, tag_a) == 0) ia = i;
        if (strcmp(b->bricks[i].tag, tag_b) == 0) ib = i;
    }
    if (ia < 0 || ib < 0) return -1;
    for (i = 0; i < 16; ++i) {
        unsigned ya = (unsigned)(b->bricks[ia].lut[i] + 0.5f) & 15u;
        unsigned yb = (unsigned)(b->bricks[ib].lut[ya] + 0.5f) & 15u;
        lut[i] = (float)yb;
    }
    if (cnet_serve_save_lut(b->dir[0] ? b->dir : ".", tag_out, "composed", lut) != 0)
        return -2;
    return cnet_serve_bank_reload(b);
}

int cnet_serve_result(CnetServeBank *b, const char *turn, CnetServeResult *out) {
    char tag[32];
    unsigned x = 0, y = 0, j;
    int i;
    if (!b || !out) return -1;
    memset(out, 0, sizeof *out);
    if (parse_turn(turn, tag, sizeof tag, &x) != 0) {
        out->abstained = 1;
        copy_text(out->refusal, sizeof out->refusal, "outside_table_abstain");
        copy_text(out->spoken, sizeof out->spoken, "outside_table_abstain");
        b->abstains++;
        return 1;
    }
    for (i = 0; i < b->n; ++i) {
        CnetServeBrick *br = &b->bricks[i];
        if (!br->live) continue;
        /* An unaddressed turn must NOT fall through to whichever brick loaded
           first. This guard used to read `tag[0] && strcmp(...)`, so an empty
           tag skipped the comparison entirely and brick[0] answered with
           claimed_cert = 1. parse_turn leaves the tag empty for any turn not
           starting with a letter or '_', i.e. every symbolic arithmetic query:
           live, "2+2" came back as a certified 3 from the user_pref fixture.
           Bricks are always addressed by tag ("user_pref 7"); no turn means
           "whichever brick you happen to have". Require the tag.
           Gate: tests/test_core_serve_untagged.c */
        if (!tag[0] || strcmp(tag, br->tag) != 0) continue;
        y = (unsigned)(br->lut[x] + 0.5f) & 15u;
        /* Plain LUT files carry no contract or coverage evidence. Their
           calculated values are candidates, never certified responses. */
        out->proved = 0;
        out->claimed_cert = 0;
        out->in_nibble = x;
        out->out_nibble = y;
        snprintf(out->spoken, sizeof out->spoken, "%u", y);
        copy_text(out->brick, sizeof out->brick, br->name);
        b->proves++;
        (void)j;
        return 0;
    }
    out->abstained = 1;
    copy_text(out->refusal, sizeof out->refusal, "outside_table_abstain");
    copy_text(out->spoken, sizeof out->spoken, "outside_table_abstain");
    b->abstains++;
    return 1;
}

static CnetServeBank g_serve;
static int g_serve_ready;

CnetServeBank *cnet_serve_global(void) {
    return g_serve_ready ? &g_serve : NULL;
}

int cnet_serve_global_load_env(void) {
    const char *dir = getenv("CNET_CORE_BUS_BRICKS_DIR");
    CnetServeBank next;
    if (!dir || !dir[0]) return 1;
    if (cnet_serve_bank_load_dir(&next, dir) != 0) return -1;
    next.proves = g_serve.proves;
    next.abstains = g_serve.abstains;
    next.reloads = g_serve.reloads + 1;
    g_serve = next;
    g_serve_ready = (g_serve.n > 0);
    return g_serve_ready ? 0 : 1;
}
