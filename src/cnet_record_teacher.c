#include "../include/cnet_record_teacher.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same FNV-1a-64 as cnet_auto_learn.c — record identity must be a pure
   function of the record bytes so re-teaching an unchanged record mints an
   identical provenance digest. */
static unsigned long long fnv1a(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    unsigned long long h = 14695981039346656037ULL;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int cnet_record_words_load(const char *words_path,
                           char (*words)[CNET_RECORD_WORD_MAX], size_t cap) {
    FILE *f;
    char line[256];
    size_t n = 0;
    if (!words_path || !words_path[0] || !words || cap == 0) return -1;
    f = fopen(words_path, "r");
    if (!f) return -1;
    while (n < cap && fgets(line, sizeof line, f)) {
        /* "<id><ws><word>" — skip the numeric id, take the word, lowercase. */
        char *p = line;
        size_t o = 0;
        while (*p && isdigit((unsigned char)*p)) p++;
        while (*p && isspace((unsigned char)*p)) p++;
        while (*p && !isspace((unsigned char)*p) && o + 1 < CNET_RECORD_WORD_MAX)
            words[n][o++] = (char)tolower((unsigned char)*p++);
        words[n][o] = '\0';
        if (o == 0) { fclose(f); return -1; }   /* malformed line: refuse whole file */
        n++;
    }
    fclose(f);
    return (int)n;
}

static int word_index(const char *word, char (*words)[CNET_RECORD_WORD_MAX], int w) {
    int i;
    for (i = 0; i < w; i++)
        if (strcmp(word, words[i]) == 0) return i;
    return -1;
}

int cnet_record_table_build(CnetRecordCtx *ctx, int w, int k, const char *text,
                            char (*words)[CNET_RECORD_WORD_MAX]) {
    int i, prev = -1, transitions = 0;
    const char *p = text;
    if (!ctx || !text || !words || w <= 0 || w > CNET_RECORD_W_MAX) return -1;
    ctx->w = w;
    ctx->k = (k >= 1 && k <= 8) ? k : 1;
    for (i = 0; i < w; i++) ctx->next[i] = i;   /* identity default */

    while (*p) {
        char word[CNET_RECORD_WORD_MAX];
        size_t o = 0;
        while (*p && !isalpha((unsigned char)*p)) p++;
        while (*p && isalpha((unsigned char)*p)) {
            if (o + 1 < sizeof word) word[o++] = (char)tolower((unsigned char)*p);
            p++;
        }
        word[o] = '\0';
        if (o == 0) continue;
        i = word_index(word, words, w);
        if (i < 0) continue;                    /* out-of-window: skip over */
        if (prev >= 0 && prev != i && ctx->next[prev] == prev) {
            ctx->next[prev] = i;                /* first occurrence wins */
            transitions++;
        }
        prev = i;
    }
    return transitions;
}

int cnet_record_teacher(const double *in, double *out, void *vctx) {
    const CnetRecordCtx *ctx = (const CnetRecordCtx *)vctx;
    int hot = 0, i, r, cur;
    if (!in || !out || !ctx || ctx->w <= 0) return -1;
    for (i = 1; i < ctx->w; i++)
        if (in[i] > in[hot]) hot = i;
    memset(out, 0, (size_t)ctx->w * (size_t)ctx->k * sizeof *out);
    cur = hot;
    for (r = 0; r < ctx->k; r++) {
        cur = ctx->next[cur];
        out[(size_t)r * (size_t)ctx->w + cur] = 1.0;
    }
    return 0;   /* every point answered — never abstain (oracle_unfit gate) */
}

/* Candidate filter mirroring gap_lane_run's gap_oracle_candidate: open, or a
   NO_PLAN parked specifically because no oracle was present. */
static int record_candidate(const GapRecord *g) {
    if (g->status == GAP_OPEN) return 1;
    return g->kind == GAP_NO_PLAN && g->status == GAP_DEFERRED &&
           strcmp(g->defer_reason, "waiting_oracle") == 0;
}

size_t cnet_record_bind(GapLane *L, const char *records_dir,
                        const char *words_path,
                        unsigned long long toolchain_fp) {
    static char words[CNET_RECORD_W_MAX][CNET_RECORD_WORD_MAX];
    static CnetRecordCtx pool[CNET_RECORD_MAX];
    static char text[CNET_RECORD_TEXT_MAX];
    size_t g, pool_n = 0, bound = 0;
    int w;
    if (!L || !records_dir || !records_dir[0]) return 0;
    w = cnet_record_words_load(words_path, words, CNET_RECORD_W_MAX);
    if (w <= 0) return 0;

    for (g = 0; g < L->ledger.count && pool_n < CNET_RECORD_MAX &&
                L->oracles.count < ACQUIRE_MAX_ORACLES; g++) {
        const GapRecord *gap = &L->ledger.gaps[g];
        char path[1024], name[ACQUIRE_NAME_MAX];
        FILE *f;
        size_t len;
        CnetRecordCtx *ctx;

        if (!record_candidate(gap) || gap->kind != GAP_NO_PLAN) continue;
        if (strncmp(gap->goal_port.tag, CNET_RECORD_TAG_PREFIX,
                    sizeof CNET_RECORD_TAG_PREFIX - 1) != 0) continue;
        if (gap->input_port.family != PORT_ONEHOT ||
            gap->goal_port.family != PORT_ONEHOT ||
            gap->input_port.field_width != (size_t)w ||
            gap->goal_port.field_width != (size_t)w ||
            gap->input_port.field_count != 1) continue;

        snprintf(path, sizeof path, "%s/%s.txt", records_dir, gap->goal_port.tag);
        f = fopen(path, "rb");
        if (!f) continue;                       /* no record: leave for other teachers */
        len = fread(text, 1, sizeof text - 1, f);
        fclose(f);
        if (len == 0) continue;                 /* empty record binds no truth */
        text[len] = '\0';

        ctx = &pool[pool_n];
        if (cnet_record_table_build(ctx, w, (int)gap->goal_port.field_count,
                                    text, words) < 0) continue;

        snprintf(name, sizeof name, "rec_%.44s", gap->goal_port.tag);
        if (acquire_oracle_register(&L->oracles, name, gap->input_port,
                                    gap->goal_port, cnet_record_teacher,
                                    ctx) != 0) continue;
        {
            OracleEntry *oe = &L->oracles.entries[L->oracles.count - 1];
            memset(&oe->identity, 0, sizeof oe->identity);
            oe->identity.abi_version = CNET_ORACLE_ABI_VERSION;
            oe->identity.struct_size = (uint32_t)sizeof oe->identity;
            /* The record IS the artifact: identity follows the bytes, so an
               edited record is truthfully a different teacher. */
            oe->identity.artifact_digest = fnv1a(text, len);
            oe->identity.contract_digest =
                fnv1a(gap->goal_port.tag, strlen(gap->goal_port.tag));
            oe->identity.config_digest = fnv1a(words, (size_t)w * CNET_RECORD_WORD_MAX);
            oe->identity.toolchain_digest =
                toolchain_fp ? toolchain_fp : 0x5245435F54430001ULL; /* "REC_TC" */
            oe->behavior_digest = cnet_oracle_identity_digest(&oe->identity);
        }
        pool_n++;
        bound++;
    }
    return bound;
}
