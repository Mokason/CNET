#include "cnet_ember_steer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cnet_ember_steer_init(CnetEmberSteer *st) {
    if (!st) return;
    memset(st, 0, sizeof *st);
    st->scale = 1.0f;
}

void cnet_ember_steer_clear(CnetEmberSteer *st) {
    if (!st) return;
    memset(st, 0, sizeof *st);
    st->scale = 1.0f;
}

static int looks_binary_f32(const unsigned char *b, size_t n) {
    size_t i, nz = 0;
    if (n < 64) return 0;
    for (i = 0; i < n && i < 256; i++)
        if (b[i] == 0) nz++;
    return nz > 8;
}

int cnet_ember_steer_load(CnetEmberSteer *st, const char *path) {
    FILE *f;
    char buf[8192];
    size_t n;
    if (!st) return -1;
    cnet_ember_steer_init(st);
    if (!path || !path[0]) return 1;
    snprintf(st->path, sizeof st->path, "%s", path);
    f = fopen(path, "rb");
    if (!f) return 1;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (n == 0) return 1;

    if (looks_binary_f32((const unsigned char *)buf, n)) {
        /* f32 direction dump: cannot apply to CORE; enable default style card. */
        snprintf(st->card, sizeof st->card,
                 "Style: prefer concise, fail-closed, no claims of CERT or consciousness. "
                 "Answer residual draft only.");
        st->scale = 1.0f;
        st->loaded = 1;
        return 0;
    }

    /* Best-effort JSON style field */
    {
        const char *p = strstr(buf, "\"style\"");
        if (p) {
            const char *q = strchr(p, ':');
            if (q) {
                while (*q && (*q == ':' || isspace((unsigned char)*q))) q++;
                if (*q == '"') {
                    size_t i = 0;
                    q++;
                    while (*q && *q != '"' && i + 1 < sizeof st->card)
                        st->card[i++] = *q++;
                    st->card[i] = '\0';
                }
            }
        }
        p = strstr(buf, "\"scale\"");
        if (p) {
            const char *q = strchr(p, ':');
            if (q) st->scale = (float)atof(q + 1);
        }
    }

    if (st->card[0] == '\0') {
        /* Plain text: skip # comments, take first ~1.5k */
        size_t i = 0, o = 0;
        while (i < n && o + 1 < sizeof st->card) {
            if (buf[i] == '#') {
                while (i < n && buf[i] != '\n') i++;
                continue;
            }
            st->card[o++] = buf[i++];
        }
        st->card[o] = '\0';
        while (o > 0 && isspace((unsigned char)st->card[o - 1])) st->card[--o] = '\0';
    }
    if (st->card[0] == '\0') return 1;
    if (st->scale <= 0.0f) st->scale = 1.0f;
    st->loaded = 1;
    return 0;
}

int cnet_ember_steer_apply(const CnetEmberSteer *st, const char *turn, char *dst,
                           size_t cap) {
    if (!dst || cap == 0) return -1;
    dst[0] = '\0';
    if (!turn) return 1;
    if (!st || !st->loaded || st->card[0] == '\0') {
        snprintf(dst, cap, "%s", turn);
        return 0;
    }
    /* scale>1 → stronger style prefix repetition cue */
    if (st->scale >= 1.5f)
        snprintf(dst, cap, "[EMBER_STEER x2 — residual draft only, never CERT]\n%s\n%s\n\n%s",
                 st->card, st->card, turn);
    else
        snprintf(dst, cap, "[EMBER_STEER — residual draft only, never CERT]\n%s\n\n%s",
                 st->card, turn);
    return 0;
}
