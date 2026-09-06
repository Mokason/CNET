/* cce_ngram — CPU n-gram hash table. Not CERT. Not GPU. */
#include "cce/cce_ngram.h"

#include <stdlib.h>
#include <string.h>

#define LAMBDA 0.4f

typedef struct {
    uint32_t ctx[CCE_NGRAM_MAX_ORDER];
    uint8_t ctx_n;
    uint8_t n_next;
    uint32_t total;
    uint32_t next_tok[CCE_NGRAM_TOPK];
    uint32_t next_cnt[CCE_NGRAM_TOPK];
} Cell;

struct cce_ngram {
    int order;
    size_t slots;
    size_t mask;
    size_t used;
    uint64_t tokens_ingested;
    uint64_t adds;
    uint64_t lookups;
    uint64_t hits;
    uint64_t backoff;
    Cell *cells;
};

static uint64_t hash_ctx(const uint32_t *ctx, int n)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) {
        h ^= (uint64_t)ctx[i] + 1u;
        h *= 1099511628211ull;
    }
    h ^= (uint64_t)(uint8_t)n;
    h *= 1099511628211ull;
    return h;
}

static int ctx_eq(const Cell *c, const uint32_t *ctx, int n)
{
    if ((int)c->ctx_n != n)
        return 0;
    if (n == 0)
        return 1;
    return memcmp(c->ctx, ctx, (size_t)n * sizeof(uint32_t)) == 0;
}

static size_t next_pow2(size_t n)
{
    size_t p = 16;
    while (p < n)
        p <<= 1;
    return p;
}

cce_ngram *cce_ngram_open(int order, size_t slots)
{
    if (order < 1 || order > CCE_NGRAM_MAX_ORDER)
        return NULL;
    if (slots < 16)
        slots = 16;
    slots = next_pow2(slots);
    cce_ngram *t = (cce_ngram *)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->cells = (Cell *)calloc(slots, sizeof(Cell));
    if (!t->cells) {
        free(t);
        return NULL;
    }
    t->order = order;
    t->slots = slots;
    t->mask = slots - 1;
    return t;
}

void cce_ngram_close(cce_ngram *t)
{
    if (!t)
        return;
    free(t->cells);
    free(t);
}

const char *cce_ngram_version(void)
{
    return CCE_NGRAM_VERSION;
}

static Cell *find_cell(const cce_ngram *t, const uint32_t *ctx, int n, int create)
{
    uint64_t h = hash_ctx(ctx, n);
    size_t i = (size_t)h & t->mask;
    Cell *empty = NULL;
    for (size_t p = 0; p < t->slots; p++) {
        Cell *c = &t->cells[i];
        if (c->total == 0 && c->ctx_n == 0 && c->n_next == 0) {
            if (!create)
                return NULL;
            if (!empty)
                empty = c;
            break;
        }
        if (ctx_eq(c, ctx, n))
            return c;
        i = (i + 1) & t->mask;
    }
    if (!create || !empty)
        return NULL;
    memset(empty, 0, sizeof *empty);
    if (n > 0)
        memcpy(empty->ctx, ctx, (size_t)n * sizeof(uint32_t));
    empty->ctx_n = (uint8_t)n;
    return empty;
}

static void bump_next(Cell *c, uint32_t next, uint32_t count)
{
    for (int i = 0; i < c->n_next; i++) {
        if (c->next_tok[i] == next) {
            uint64_t s = (uint64_t)c->next_cnt[i] + count;
            c->next_cnt[i] = s > 0xffffffffu ? 0xffffffffu : (uint32_t)s;
            return;
        }
    }
    if (c->n_next < CCE_NGRAM_TOPK) {
        int i = c->n_next++;
        c->next_tok[i] = next;
        c->next_cnt[i] = count;
        return;
    }
    /* table full for this ctx: replace lightest if incoming is heavier */
    int min_i = 0;
    uint32_t min_c = c->next_cnt[0];
    for (int i = 1; i < CCE_NGRAM_TOPK; i++) {
        if (c->next_cnt[i] < min_c) {
            min_c = c->next_cnt[i];
            min_i = i;
        }
    }
    if (count >= min_c) {
        c->next_tok[min_i] = next;
        c->next_cnt[min_i] = count;
    }
}

int cce_ngram_add(cce_ngram *t, const uint32_t *ctx, int ctx_n, uint32_t next,
                  uint32_t count)
{
    if (!t || count == 0 || ctx_n < 0 || ctx_n >= t->order)
        return CCE_NGRAM_ERR;
    if (ctx_n > 0 && !ctx)
        return CCE_NGRAM_ERR;
    Cell *c = find_cell(t, ctx, ctx_n, 1);
    if (!c)
        return CCE_NGRAM_FULL;
    int was_empty = (c->total == 0 && c->n_next == 0);
    bump_next(c, next, count);
    uint64_t tot = (uint64_t)c->total + count;
    c->total = tot > 0xffffffffu ? 0xffffffffu : (uint32_t)tot;
    if (was_empty)
        t->used++;
    t->adds++;
    return CCE_NGRAM_OK;
}

int cce_ngram_ingest(cce_ngram *t, const uint32_t *toks, size_t len)
{
    if (!t || !toks)
        return CCE_NGRAM_ERR;
    t->tokens_ingested += len;
    for (size_t i = 0; i < len; i++) {
        /* unigram */
        if (cce_ngram_add(t, NULL, 0, toks[i], 1) == CCE_NGRAM_FULL)
            return CCE_NGRAM_FULL;
        for (int ctx_n = 1; ctx_n < t->order && (size_t)ctx_n <= i; ctx_n++) {
            const uint32_t *ctx = toks + (i - (size_t)ctx_n);
            if (cce_ngram_add(t, ctx, ctx_n, toks[i], 1) == CCE_NGRAM_FULL)
                return CCE_NGRAM_FULL;
        }
    }
    return CCE_NGRAM_OK;
}

int cce_ngram_merge(cce_ngram *dst, const cce_ngram *src)
{
    if (!dst || !src || dst->order != src->order)
        return CCE_NGRAM_ERR;
    for (size_t i = 0; i < src->slots; i++) {
        const Cell *c = &src->cells[i];
        if (c->total == 0 && c->n_next == 0)
            continue;
        for (int k = 0; k < c->n_next; k++) {
            int rc = cce_ngram_add(dst, c->ctx, c->ctx_n, c->next_tok[k], c->next_cnt[k]);
            if (rc != CCE_NGRAM_OK)
                return rc;
        }
    }
    dst->tokens_ingested += src->tokens_ingested;
    return CCE_NGRAM_OK;
}

static const Cell *lookup(const cce_ngram *t, const uint32_t *ctx, int n)
{
    return find_cell(t, ctx, n, 0);
}

uint32_t cce_ngram_count(const cce_ngram *t, const uint32_t *ctx, int ctx_n,
                         uint32_t next)
{
    if (!t || ctx_n < 0 || ctx_n >= t->order)
        return 0;
    const Cell *c = lookup(t, ctx, ctx_n);
    if (!c)
        return 0;
    for (int i = 0; i < c->n_next; i++)
        if (c->next_tok[i] == next)
            return c->next_cnt[i];
    return 0;
}

int cce_ngram_predict(cce_ngram *t, const uint32_t *ctx, int ctx_n,
                      uint32_t *out_tok, float *out_p, int k, int *k_out)
{
    if (!t || !out_tok || !out_p || !k_out || k < 1)
        return CCE_NGRAM_ERR;
    if (ctx_n < 0)
        ctx_n = 0;
    if (ctx_n >= t->order)
        ctx_n = t->order - 1;
    t->lookups++;
    const Cell *c = NULL;
    int used_n = ctx_n;
    float scale = 1.f;
    while (used_n >= 0) {
        const uint32_t *sub = (used_n > 0) ? (ctx + (ctx_n - used_n)) : NULL;
        c = lookup(t, sub, used_n);
        if (c && c->n_next > 0)
            break;
        if (used_n == 0)
            break;
        used_n--;
        scale *= LAMBDA;
        t->backoff++;
    }
    if (!c || c->n_next == 0 || c->total == 0) {
        *k_out = 0;
        return CCE_NGRAM_MISS;
    }
    t->hits++;
    int nk = c->n_next < k ? c->n_next : k;
    /* pick top-nk by count (small k, insertion) */
    int idx[CCE_NGRAM_TOPK];
    for (int i = 0; i < c->n_next; i++)
        idx[i] = i;
    for (int a = 0; a < c->n_next; a++) {
        for (int b = a + 1; b < c->n_next; b++) {
            if (c->next_cnt[idx[b]] > c->next_cnt[idx[a]]) {
                int tmp = idx[a];
                idx[a] = idx[b];
                idx[b] = tmp;
            }
        }
    }
    float inv = scale / (float)c->total;
    for (int i = 0; i < nk; i++) {
        out_tok[i] = c->next_tok[idx[i]];
        out_p[i] = (float)c->next_cnt[idx[i]] * inv;
    }
    *k_out = nk;
    return CCE_NGRAM_OK;
}

int cce_ngram_get_stats(const cce_ngram *t, cce_ngram_stats *out)
{
    if (!t || !out)
        return CCE_NGRAM_ERR;
    memset(out, 0, sizeof *out);
    out->order = t->order;
    out->slots = t->slots;
    out->used = t->used;
    out->tokens_ingested = t->tokens_ingested;
    out->adds = t->adds;
    out->lookups = t->lookups;
    out->hits = t->hits;
    out->backoff = t->backoff;
    return CCE_NGRAM_OK;
}
