/* Light .lut brick bank — live RESULT without GGUF/admit link weight. */
#include "cnet_core_serve.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
            for (i = 0; i < 16; ++i) {
                while (*p == ' ' || *p == ',') p++;
                br->lut[i] = (float)strtod(p, &p);
            }
            got_lut = 1;
        }
    }
    fclose(f);
    if (!got_lut || !br->tag[0]) return -1;
    if (!br->name[0]) copy_text(br->name, sizeof br->name, br->tag);
    br->live = 1;
    return 0;
}

int cnet_serve_save_lut(const char *dir, const char *tag, const char *name,
                        const float lut[16]) {
    char path[768];
    FILE *f;
    int i;
    if (!dir || !tag || !lut) return -1;
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s.lut", dir, tag);
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# CNET_LUT_V1\n");
    fprintf(f, "tag=%s\n", tag);
    fprintf(f, "name=%s\n", name && name[0] ? name : tag);
    fprintf(f, "lut=");
    for (i = 0; i < 16; ++i)
        fprintf(f, "%s%g", i ? "," : "", (double)lut[i]);
    fprintf(f, "\n");
    fclose(f);
    return 0;
}

int cnet_serve_bank_load_dir(CnetServeBank *b, const char *dir) {
    DIR *d;
    struct dirent *e;
    if (!b || !dir || !dir[0]) return -1;
    cnet_serve_bank_init(b);
    copy_text(b->dir, sizeof b->dir, dir);
    d = opendir(dir);
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        char path[768];
        CnetServeBrick br;
        size_t n;
        if (e->d_name[0] == '.') continue;
        n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".lut") != 0) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (load_one_lut(path, &br) != 0) continue;
        if (b->n >= CNET_SERVE_MAX_BRICKS) break;
        b->bricks[b->n++] = br;
    }
    closedir(d);
    b->reloads++;
    return b->n >= 0 ? 0 : -1;
}

int cnet_serve_bank_reload(CnetServeBank *b) {
    char dir[512];
    if (!b || !b->dir[0]) return -1;
    copy_text(dir, sizeof dir, b->dir);
    return cnet_serve_bank_load_dir(b, dir);
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
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return -1;
    while (isdigit((unsigned char)*p)) {
        saw = 1;
        v = v * 10u + (unsigned)(*p - '0');
        if (v > 15u) return -1;
        p++;
    }
    if (!saw) return -1;
    *out_x = v;
    return 0;
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
        if (tag[0] && strcmp(tag, br->tag) != 0) continue;
        y = (unsigned)(br->lut[x] + 0.5f) & 15u;
        out->proved = 1;
        out->claimed_cert = 1;
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
    if (!dir || !dir[0]) return 1;
    if (cnet_serve_bank_load_dir(&g_serve, dir) != 0) return -1;
    g_serve_ready = (g_serve.n > 0);
    return g_serve_ready ? 0 : 1;
}
