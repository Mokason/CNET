/* Obsidian â†’ CORE structured learn harvest (no free-text auto-CERT). */
#include "cnet_obsidian_learn.h"
#include "cnet_live_miss.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static int starts_ci(const char *s, const char *pfx) {
    size_t i;
    if (!s || !pfx) return 0;
    for (i = 0; pfx[i]; ++i)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)pfx[i]))
            return 0;
    return 1;
}

int cnet_obsidian_resolve_vault(char *out, size_t cap) {
    const char *e;
    if (!out || !cap) return -1;
    e = getenv("CNET_OBSIDIAN_VAULT");
    if (e && e[0] && access(e, R_OK) == 0) {
        copy_text(out, cap, e);
        return 0;
    }
    e = getenv("OBSIDIAN_VAULT_PATH");
    if (e && e[0] && access(e, R_OK) == 0) {
        copy_text(out, cap, e);
        return 0;
    }
    {
        char p[512];
        const char *home = getenv("HOME");
        if (!home) home = "/home/marble";
        snprintf(p, sizeof p, "%s/Obsidian", home);
        if (access(p, R_OK) == 0) {
            copy_text(out, cap, p);
            return 0;
        }
        snprintf(p, sizeof p, "%s/obsidian-vault", home);
        if (access(p, R_OK) == 0) {
            copy_text(out, cap, p);
            return 0;
        }
        snprintf(p, sizeof p, "%s/Documents/Obsidian Vault", home);
        if (access(p, R_OK) == 0) {
            copy_text(out, cap, p);
            return 0;
        }
        snprintf(p, sizeof p, "%s/AI/Obsidian", home);
        if (access(p, R_OK) == 0) {
            copy_text(out, cap, p);
            return 0;
        }
    }
    return -1;
}

static int parse_pairs_blob(const char *blob, float lut[16], int *got) {
    const char *p = blob;
    unsigned seen[16];
    int n = 0;
    memset(seen, 0, sizeof seen);
    memset(lut, 0, 16 * sizeof(float));
    while (*p) {
        unsigned a, b;
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (!isdigit((unsigned char)*p)) {
            p++;
            continue;
        }
        a = (unsigned)strtoul(p, (char **)&p, 10);
        if (*p == ':') p++;
        else
            break;
        b = (unsigned)strtoul(p, (char **)&p, 10);
        if (a < 16 && b < 16) {
            lut[a] = (float)b;
            if (!seen[a]) {
                seen[a] = 1;
                n++;
            }
        }
    }
    if (got) *got = n;
    return n == 16 ? 0 : -1;
}

static int emit_lut_teaches(const char *miss, const char *domain, const float lut[16],
                            CnetObsidianLearnReport *rep) {
    int i;
    for (i = 0; i < 16; ++i) {
        unsigned o = (unsigned)(lut[i] + 0.5f) & 15u;
        if (cnet_live_miss_append(miss, domain, (unsigned)i, 1, o) == 0) {
            rep->teaches++;
        }
    }
    rep->domains_complete_emitted++;
    return 0;
}

static int handle_line(const char *line, const char *miss, const char *bricks,
                       char *cur_domain, size_t dcap,
                       CnetObsidianLearnReport *rep) {
    char tag[32];
    unsigned in_n = 0, out_n = 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') return 0;

    /* YAML / field */
    if (starts_ci(p, "cnet_domain:")) {
        p += 12;
        while (*p == ' ' || *p == '\t') p++;
        copy_text(cur_domain, dcap, p);
        {
            char *nl = strchr(cur_domain, '\n');
            if (nl) *nl = 0;
            nl = strchr(cur_domain, '\r');
            if (nl) *nl = 0;
        }
        return 0;
    }

    if (cnet_live_parse_teach(p, tag, sizeof tag, &in_n, &out_n) == 0) {
        if (cnet_live_miss_append(miss, tag, in_n, 1, out_n) == 0) {
            rep->teaches++;
            copy_text(cur_domain, dcap, tag);
        }
        return 0;
    }

    if (starts_ci(p, "given domain ")) {
        /* reuse scenario grammar via pairs= */
        char dom[32];
        const char *pp = p + 13;
        float lut[16];
        int got = 0;
        if (sscanf(pp, "%31s", dom) == 1) {
            const char *pairs = strstr(pp, "pairs=");
            const char *lutp = strstr(pp, "lut=");
            if (pairs && parse_pairs_blob(pairs + 6, lut, &got) == 0) {
                emit_lut_teaches(miss, dom, lut, rep);
                copy_text(cur_domain, dcap, dom);
            } else if (lutp) {
                const char *q = lutp + 4;
                int i;
                for (i = 0; i < 16; ++i) {
                    while (*q == ',' || *q == ' ') q++;
                    lut[i] = (float)strtod(q, (char **)&q);
                }
                emit_lut_teaches(miss, dom, lut, rep);
                copy_text(cur_domain, dcap, dom);
            }
        }
        return 0;
    }

    if (starts_ci(p, "pairs=") || starts_ci(p, "cnet_pairs=")) {
        float lut[16];
        int got = 0;
        const char *blob = strchr(p, '=');
        if (blob && cur_domain[0] &&
            parse_pairs_blob(blob + 1, lut, &got) == 0) {
            emit_lut_teaches(miss, cur_domain, lut, rep);
            rep->pair_lines++;
        } else if (blob && cur_domain[0] && got > 0) {
            /* partial pairs still useful as teaches */
            int i;
            unsigned seen[16];
            memset(seen, 0, sizeof seen);
            parse_pairs_blob(blob + 1, lut, &got);
            for (i = 0; i < 16; ++i) {
                /* only emit slots that appeared: heuristic non-zero or got */
            }
            /* re-parse emitting each pair */
            {
                const char *q = blob + 1;
                while (*q) {
                    unsigned a, b;
                    while (*q == ',' || *q == ' ') q++;
                    if (!isdigit((unsigned char)*q)) break;
                    a = (unsigned)strtoul(q, (char **)&q, 10);
                    if (*q == ':') q++;
                    b = (unsigned)strtoul(q, (char **)&q, 10);
                    if (a < 16 && b < 16)
                        (void)cnet_live_miss_append(miss, cur_domain, a, 1, b);
                }
            }
            rep->pair_lines++;
            rep->teaches += got;
        }
        return 0;
    }

    if (starts_ci(p, "cnet_guide:") || starts_ci(p, "guide:")) {
        const char *g = strchr(p, ':');
        if (g && bricks && bricks[0]) {
            char gpath[768];
            FILE *gf;
            g++;
            while (*g == ' ') g++;
            if (*g) {
                snprintf(gpath, sizeof gpath, "%s/guide_questions.txt", bricks);
                gf = fopen(gpath, "a");
                if (gf) {
                    fprintf(gf, "%s\n", g);
                    fclose(gf);
                    rep->goals_queued++; /* count as queued guide */
                }
            }
        }
        return 0;
    }

    if (starts_ci(p, "cnet_goal:") || starts_ci(p, "goal:")) {
        const char *g = strchr(p, ':');
        if (g && bricks && bricks[0]) {
            char gpath[768];
            FILE *gf;
            g++;
            while (*g == ' ') g++;
            if (*g) {
                snprintf(gpath, sizeof gpath, "%s/pending_goals.txt", bricks);
                gf = fopen(gpath, "a");
                if (gf) {
                    fprintf(gf, "%s\n", g);
                    fclose(gf);
                    rep->goals_queued++;
                }
            }
        }
        return 0;
    }

    return 0;
}

static int harvest_file(const char *path, const char *miss, const char *bricks,
                        CnetObsidianLearnReport *rep) {
    FILE *f;
    char line[1024];
    char cur_domain[32];
    int in_fence = 0;
    int hit = 0;
    cur_domain[0] = 0;
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, "```cnet-domain", 14) == 0 ||
            strncmp(line, "```cnet", 7) == 0) {
            in_fence = 1;
            hit = 1;
            continue;
        }
        if (in_fence && strncmp(line, "```", 3) == 0) {
            in_fence = 0;
            continue;
        }
        if (in_fence || strstr(line, "cnet_domain:") || strstr(line, "teach ") ||
            strstr(line, "pairs=") || strstr(line, "given domain") ||
            strstr(line, "cnet_goal:") || strstr(line, "cnet_pairs=")) {
            hit = 1;
            handle_line(line, miss, bricks, cur_domain, sizeof cur_domain, rep);
        }
    }
    fclose(f);
    if (hit) rep->files_hit++;
    return 0;
}

static int walk_md(const char *root, const char *miss, const char *bricks,
                   int *left, CnetObsidianLearnReport *rep) {
    DIR *d;
    struct dirent *e;
    if (*left <= 0) return 0;
    d = opendir(root);
    if (!d) return -1;
    while ((e = readdir(d)) != NULL && *left > 0) {
        char path[1024];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        if (strcmp(e->d_name, ".obsidian") == 0) continue;
        if (strcmp(e->d_name, "node_modules") == 0) continue;
        snprintf(path, sizeof path, "%s/%s", root, e->d_name);
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_md(path, miss, bricks, left, rep);
        } else if (S_ISREG(st.st_mode)) {
            size_t n = strlen(e->d_name);
            if (n > 3 && strcmp(e->d_name + n - 3, ".md") == 0) {
                rep->files_scanned++;
                (*left)--;
                (void)harvest_file(path, miss, bricks, rep);
            }
        }
    }
    closedir(d);
    return 0;
}

int cnet_obsidian_learn(const char *vault, const char *miss_path,
                        const char *bricks_dir, int max_files,
                        CnetObsidianLearnReport *rep) {
    char vbuf[512];
    const char *v = vault;
    int left;
    if (!rep) return -1;
    memset(rep, 0, sizeof *rep);
    if (!v || !v[0]) {
        if (cnet_obsidian_resolve_vault(vbuf, sizeof vbuf) != 0) return -1;
        v = vbuf;
    }
    if (!miss_path || !miss_path[0]) return -1;
    copy_text(rep->vault, sizeof rep->vault, v);
    copy_text(rep->miss_path, sizeof rep->miss_path, miss_path);
    if (max_files <= 0) max_files = 200;
    left = max_files;
    return walk_md(v, miss_path, bricks_dir, &left, rep);
}
