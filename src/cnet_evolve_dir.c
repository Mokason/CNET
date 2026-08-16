#include "cnet_evolve_dir.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
}

static void split_csv(const char *csv, char out[][CNET_EVDIR_NAME], int *n,
                      int maxn) {
    char buf[512];
    char *tok, *save = NULL;
    if (!csv || !n) return;
    copy_text(buf, sizeof buf, csv);
    *n = 0;
    for (tok = strtok_r(buf, ",", &save); tok && *n < maxn;
         tok = strtok_r(NULL, ",", &save)) {
        trim(tok);
        if (!tok[0]) continue;
        copy_text(out[(*n)++], CNET_EVDIR_NAME, tok);
    }
}

void cnet_evolve_dir_defaults(CnetEvolveDirection *D) {
    if (!D) return;
    memset(D, 0, sizeof *D);
    D->allow_live_miss = 1;
    D->allow_factory = 1;
    D->allow_goals = 1;
    D->allow_agi_tick = 1;
    D->factory_if_empty = 1;
    D->allow_obsidian = 1;
    D->obsidian_max_files = 200;
    D->obsidian_vault[0] = 0;
    D->max_new_per_tick = 3;
    D->factory[0] =
        (CnetPath2Spec){"blk.0.attn_q.weight", "q1_add16", "brick_q_add", 0};
    D->factory[1] =
        (CnetPath2Spec){"blk.0.attn_k.weight", "q1_xor16", "brick_k_xor", 1};
    D->n_factory = 2;
    copy_text(D->loaded_from, sizeof D->loaded_from, "defaults");
}

static int load_file(CnetEvolveDirection *D, const char *path) {
    FILE *f;
    char line[512];
    if (!D || !path || !path[0]) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    cnet_evolve_dir_defaults(D);
    D->n_factory = 0;
    D->n_goals = 0;
    D->n_prefer = 0;
    D->n_deny = 0;
    copy_text(D->loaded_from, sizeof D->loaded_from, path);
    while (fgets(line, sizeof line, f)) {
        char *eq;
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        trim(line);
        trim(eq + 1);
        if (strcmp(line, "allow_live_miss") == 0)
            D->allow_live_miss = atoi(eq + 1) ? 1 : 0;
        else if (strcmp(line, "allow_factory") == 0)
            D->allow_factory = atoi(eq + 1) ? 1 : 0;
        else if (strcmp(line, "allow_goals") == 0)
            D->allow_goals = atoi(eq + 1) ? 1 : 0;
        else if (strcmp(line, "allow_agi_tick") == 0)
            D->allow_agi_tick = atoi(eq + 1) ? 1 : 0;
        else if (strcmp(line, "factory_if_empty") == 0)
            D->factory_if_empty = atoi(eq + 1) ? 1 : 0;
        else if (strcmp(line, "allow_obsidian") == 0)
            D->allow_obsidian = atoi(eq + 1) ? 1 : 0;
        else if (strcmp(line, "obsidian_max_files") == 0)
            D->obsidian_max_files = atoi(eq + 1);
        else if (strcmp(line, "obsidian_vault") == 0)
            copy_text(D->obsidian_vault, sizeof D->obsidian_vault, eq + 1);
        else if (strcmp(line, "max_new_per_tick") == 0) {
            D->max_new_per_tick = atoi(eq + 1);
            if (D->max_new_per_tick < 0) D->max_new_per_tick = 0;
            if (D->max_new_per_tick > 16) D->max_new_per_tick = 16;
        } else if (strcmp(line, "prefer_domains") == 0)
            split_csv(eq + 1, D->prefer, &D->n_prefer, CNET_EVDIR_MAX_DOM);
        else if (strcmp(line, "deny_domains") == 0)
            split_csv(eq + 1, D->deny, &D->n_deny, CNET_EVDIR_MAX_DOM);
        else if (strcmp(line, "factory") == 0 &&
                 D->n_factory < CNET_EVDIR_MAX_FACTORY) {
            /* tag,mode,tensor */
            char tag[64], tensor[128];
            int mode = 0;
            char name[96];
            if (sscanf(eq + 1, "%63[^,],%d,%127s", tag, &mode, tensor) == 3) {
                trim(tag);
                trim(tensor);
                snprintf(name, sizeof name, "dir_%s", tag);
                D->factory[D->n_factory].tensor = NULL; /* filled below via dup */
                {
                    CnetPath2Spec *sp = &D->factory[D->n_factory];
                    static char tbuf[CNET_EVDIR_MAX_FACTORY][128];
                    static char gbuf[CNET_EVDIR_MAX_FACTORY][64];
                    static char nbuf[CNET_EVDIR_MAX_FACTORY][64];
                    int k = D->n_factory;
                    copy_text(tbuf[k], sizeof tbuf[k], tensor);
                    copy_text(gbuf[k], sizeof gbuf[k], tag);
                    copy_text(nbuf[k], sizeof nbuf[k], name);
                    sp->tensor = tbuf[k];
                    sp->tag = gbuf[k];
                    sp->name = nbuf[k];
                    sp->mode = mode ? 1 : 0;
                    D->n_factory++;
                }
            }
        } else if (strcmp(line, "goal") == 0 && D->n_goals < CNET_EVDIR_MAX_GOALS) {
            copy_text(D->goals[D->n_goals++], sizeof D->goals[0], eq + 1);
        }
    }
    fclose(f);
    if (D->n_factory == 0) {
        /* restore default factory entries if file omitted them */
        D->factory[0] =
            (CnetPath2Spec){"blk.0.attn_q.weight", "q1_add16", "brick_q_add", 0};
        D->factory[1] =
            (CnetPath2Spec){"blk.0.attn_k.weight", "q1_xor16", "brick_k_xor", 1};
        D->n_factory = 2;
    }
    return 0;
}

int cnet_evolve_dir_load(CnetEvolveDirection *D, const char *bricks_dir) {
    const char *envp;
    char path[768];
    if (!D) return -1;
    cnet_evolve_dir_defaults(D);
    envp = getenv("CNET_EVOLVE_DIRECTION");
    if (envp && envp[0] && load_file(D, envp) == 0) return 0;
    if (bricks_dir && bricks_dir[0]) {
        snprintf(path, sizeof path, "%s/evolve_direction.conf", bricks_dir);
        if (load_file(D, path) == 0) return 0;
    }
    if (load_file(D, "config/cnet_evolve_direction.conf") == 0) return 0;
    /* keep defaults */
    return 1;
}

int cnet_evolve_dir_domain_ok(const CnetEvolveDirection *D, const char *domain) {
    int i;
    if (!D || !domain || !domain[0]) return 0;
    for (i = 0; i < D->n_deny; ++i)
        if (strcmp(D->deny[i], domain) == 0) return 0;
    if (D->n_prefer == 0) return 1;
    for (i = 0; i < D->n_prefer; ++i)
        if (strcmp(D->prefer[i], domain) == 0) return 1;
    return 0;
}
