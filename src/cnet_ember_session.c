#include "cnet_ember_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cnet_ember_session_init(CnetEmberSession *s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->soft_compact_chars = 48 * 1024;
    s->hard_compact_chars = 96 * 1024;
    s->tail_keep_chars = 8 * 1024;
    s->open = 1;
    snprintf(s->id, sizeof s->id, "ember-%p", (void *)s);
}

void cnet_ember_session_free(CnetEmberSession *s) {
    if (!s) return;
    free(s->tx);
    memset(s, 0, sizeof *s);
}

static int ensure_cap(CnetEmberSession *s, size_t need) {
    char *n;
    size_t nc;
    if (need <= s->tx_cap) return 0;
    nc = s->tx_cap ? s->tx_cap * 2 : 4096;
    while (nc < need) nc *= 2;
    if (nc > CNET_EMBER_TX_MAX) nc = CNET_EMBER_TX_MAX;
    if (need > nc) return -1;
    n = (char *)realloc(s->tx, nc);
    if (!n) return -1;
    s->tx = n;
    s->tx_cap = nc;
    return 0;
}

static void rehash(CnetEmberSession *s) {
    if (!s->tx || s->tx_len == 0) {
        s->prefix_sha[0] = '\0';
        return;
    }
    cnet_ember_sha1_hex(s->tx, s->tx_len, s->prefix_sha);
}

static int set_tx(CnetEmberSession *s, const char *text, size_t len) {
    if (ensure_cap(s, len + 1) != 0) return -1;
    if (len) memcpy(s->tx, text, len);
    s->tx[len] = '\0';
    s->tx_len = len;
    rehash(s);
    return 0;
}

static int append_raw(CnetEmberSession *s, const char *p, size_t n) {
    if (!p || n == 0) return 0;
    if (ensure_cap(s, s->tx_len + n + 1) != 0) return -1;
    memcpy(s->tx + s->tx_len, p, n);
    s->tx_len += n;
    s->tx[s->tx_len] = '\0';
    rehash(s);
    return 0;
}

int cnet_ember_session_append_pair(CnetEmberSession *s, const char *user,
                                   const char *assistant) {
    char block[CNET_EMBER_TURN_MAX * 2 + 64];
    int n;
    if (!s || !s->open) return -1;
    n = snprintf(block, sizeof block, "U: %s\nA: %s\n", user ? user : "",
                 assistant ? assistant : "");
    if (n < 0 || (size_t)n >= sizeof block) return -1;
    if (append_raw(s, block, (size_t)n) != 0) return -1;
    s->n_turns++;
    return 0;
}

int cnet_ember_session_sync(CnetEmberSession *s, CnetEmberCkptStore *ckpt,
                            const char *full_prompt, CnetEmberSyncResult *out) {
    size_t plen;
    CnetEmberSyncResult local;
    if (out) memset(out, 0, sizeof *out);
    else out = &local;
    out->claimed_cert = 0;
    if (!s || !s->open) return -1;
    if (!full_prompt || !full_prompt[0]) {
        out->kind = CNET_EMBER_SYNC_EMPTY;
        return 1;
    }
    plen = strlen(full_prompt);

    /* EXTEND: full prompt starts with current transcript */
    if (s->tx_len > 0 && plen >= s->tx_len &&
        memcmp(full_prompt, s->tx, s->tx_len) == 0) {
        if (plen > s->tx_len) {
            if (append_raw(s, full_prompt + s->tx_len, plen - s->tx_len) != 0)
                return -1;
        }
        out->kind = CNET_EMBER_SYNC_EXTEND;
        out->common_chars = (int)s->tx_len;
        return 0;
    }

    /* LOAD_CKPT: longest checkpoint prefix of prompt */
    if (ckpt && ckpt->open) {
        int idx = cnet_ember_ckpt_find_prefix(ckpt, full_prompt, plen);
        if (idx >= 0) {
            char *tmp = (char *)malloc(CNET_EMBER_TX_MAX);
            size_t got = 0;
            if (!tmp) return -1;
            if (cnet_ember_ckpt_load(ckpt, idx, tmp, CNET_EMBER_TX_MAX, &got) >= 0) {
                if (set_tx(s, tmp, got) != 0) {
                    free(tmp);
                    return -1;
                }
                if (plen > got && memcmp(full_prompt, tmp, got) == 0) {
                    if (append_raw(s, full_prompt + got, plen - got) != 0) {
                        free(tmp);
                        return -1;
                    }
                }
                free(tmp);
                out->kind = CNET_EMBER_SYNC_LOAD_CKPT;
                out->common_chars = (int)got;
                return 0;
            }
            free(tmp);
        }
    }

    /* REBUILD */
    if (set_tx(s, full_prompt, plen) != 0) return -1;
    out->kind = CNET_EMBER_SYNC_REBUILD;
    out->common_chars = 0;
    return 0;
}

static int default_summarize(const char *body, size_t len, char *out, size_t cap,
                             void *ud) {
    size_t head = 600, tail = 600;
    size_t take;
    (void)ud;
    if (!body || !out || cap < 32) return -1;
    if (len < head + tail + 32) {
        take = len;
        if (take + 48 > cap) take = cap > 48 ? cap - 48 : 0;
        snprintf(out, cap,
                 "[ember compact durable state — not CERT]\n%.*s",
                 (int)take, body);
        return 0;
    }
    snprintf(out, cap,
             "[ember compact durable state — not CERT; auto_cert=false]\n"
             "HEAD:\n%.*s\n...\nTAIL:\n%.*s\n",
             (int)head, body, (int)tail, body + len - tail);
    return 0;
}

int cnet_ember_session_maybe_compact(CnetEmberSession *s, CnetEmberCkptStore *ckpt,
                                     CnetEmberSummarizeFn fn, void *ud, int force) {
    char summary[CNET_EMBER_SUMMARY_MAX];
    char rebuilt[CNET_EMBER_TX_MAX];
    size_t tail_n;
    const char *tail;
    int soft, hard;
    if (!s || !s->open || !s->tx) return 0;
    soft = s->soft_compact_chars > 0 ? s->soft_compact_chars : 48 * 1024;
    hard = s->hard_compact_chars > 0 ? s->hard_compact_chars : 96 * 1024;
    if (!force && (int)s->tx_len < soft) return 0;
    if (!force && (int)s->tx_len < hard && s->n_turns < 8) return 0;
    if (!fn) fn = default_summarize;
    if (fn(s->tx, s->tx_len, summary, sizeof summary, ud) != 0) return -1;
    snprintf(s->summary, sizeof s->summary, "%s", summary);
    tail_n = (size_t)(s->tail_keep_chars > 0 ? s->tail_keep_chars : 8 * 1024);
    if (tail_n > s->tx_len / 4) tail_n = s->tx_len / 4;
    if (tail_n > s->tx_len) tail_n = s->tx_len;
    tail = s->tx + (s->tx_len - tail_n);
    snprintf(rebuilt, sizeof rebuilt,
             "%s\n--- recent verbatim tail ---\n%.*s", summary, (int)tail_n, tail);
    if (set_tx(s, rebuilt, strlen(rebuilt)) != 0) return -1;
    s->compacted_n++;
    if (ckpt && ckpt->open)
        (void)cnet_ember_ckpt_store(ckpt, s->tx, s->tx_len, (uint32_t)s->n_turns,
                                    CNET_EMBER_CKPT_COMPACT);
    /* Self-improve note: compact is never CERT */
    {
        const char *p = getenv("CNET_MISS_LOG");
        FILE *f;
        if (p && p[0] && (f = fopen(p, "a"))) {
            fprintf(f,
                    "{\"via\":\"cnet_ember\",\"reason\":\"compact\","
                    "\"claimed_cert\":0,\"auto_cert\":false,\"turns\":%d}\n",
                    s->n_turns);
            fclose(f);
        }
    }
    return 1;
}

int cnet_ember_session_checkpoint(CnetEmberSession *s, CnetEmberCkptStore *ckpt,
                                  CnetEmberCkptReason reason) {
    if (!s || !s->open || !ckpt || !ckpt->open || !s->tx) return -1;
    return cnet_ember_ckpt_store(ckpt, s->tx, s->tx_len, (uint32_t)s->n_turns, reason);
}
