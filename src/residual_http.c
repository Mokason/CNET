/* HTTP residual (llama.cpp / OpenAI-compatible) for low-bit models like Bonsai.
 * Window next-token residual matching residual_gguf oracle contract.
 */
#include "../include/residual_http.h"
#include "../include/gap_lane.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ResidualHttp {
    char base[512];
    char completion_url[640];
    char models_url[640];
    int *win;
    int n_win;
    int owns_win;
    long timeout_ms;
    char err[256];
};

struct MemBuf {
    char *data;
    size_t len;
};

static int argmax_d(const double *v, int n) {
    int i, b = 0;
    if (n <= 0) return -1;
    for (i = 1; i < n; i++)
        if (v[i] > v[b]) b = i;
    return b;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct MemBuf *m = (struct MemBuf *)userdata;
    size_t n = size * nmemb;
    char *p;
    if (!m || !ptr || n == 0) return 0;
    p = (char *)realloc(m->data, m->len + n + 1);
    if (!p) return 0;
    m->data = p;
    memcpy(m->data + m->len, ptr, n);
    m->len += n;
    m->data[m->len] = '\0';
    return n;
}

static int http_post_json(const char *url, const char *body, long timeout_ms,
                          struct MemBuf *out, long *http_code) {
    CURL *curl;
    CURLcode rc;
    struct curl_slist *hdrs = NULL;
    if (!url || !body || !out) return -1;
    memset(out, 0, sizeof *out);
    curl = curl_easy_init();
    if (!curl) return -2;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 30000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    rc = curl_easy_perform(curl);
    if (http_code)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? 0 : -3;
}

static int http_get(const char *url, long timeout_ms, struct MemBuf *out,
                    long *http_code) {
    CURL *curl;
    CURLcode rc;
    if (!url || !out) return -1;
    memset(out, 0, sizeof *out);
    curl = curl_easy_init();
    if (!curl) return -2;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    rc = curl_easy_perform(curl);
    if (http_code)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? 0 : -3;
}

/* Very small JSON helpers — enough for llama.cpp completion_probabilities. */
static const char *find_key(const char *j, const char *key) {
    char pat[96];
    const char *p;
    if (!j || !key) return NULL;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(j, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    return p ? p + 1 : NULL;
}

static int parse_int_after(const char *p, long *out) {
    char *end = NULL;
    long v;
    if (!p || !out) return -1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    v = strtol(p, &end, 10);
    if (end == p) return -1;
    *out = v;
    return 0;
}

/* Pick best token id among window from completion JSON.
 * Prefers top_logprobs ids that land in window; else tokens[0]; else fail. */
static int pick_window_slot(const ResidualHttp *r, const char *json) {
    int j, best_j = -1;
    double best_lp = -1e300;
    const char *p;
    long tid;
    int in_win;

    if (!r || !json || r->n_win <= 0) return -1;

    /* Scan all "id": <num> near logprob pairs inside top_logprobs. */
    p = json;
    while ((p = strstr(p, "\"id\"")) != NULL) {
        const char *q = strchr(p, ':');
        const char *lp;
        long idv;
        double logp = -1e9;
        int slot = -1;
        p += 4;
        if (!q) continue;
        if (parse_int_after(q + 1, &idv) != 0) continue;
        in_win = 0;
        for (j = 0; j < r->n_win; j++) {
            if (r->win[j] == (int)idv) {
                slot = j;
                in_win = 1;
                break;
            }
        }
        if (!in_win) continue;
        lp = strstr(q, "\"logprob\"");
        if (lp && lp - q < 80) {
            const char *c = strchr(lp, ':');
            if (c) logp = strtod(c + 1, NULL);
        }
        if (slot >= 0 && logp > best_lp) {
            best_lp = logp;
            best_j = slot;
        }
    }
    if (best_j >= 0) return best_j;

    /* Fallback: tokens array first id */
    p = find_key(json, "tokens");
    if (p) {
        const char *b = strchr(p, '[');
        if (b && parse_int_after(b + 1, &tid) == 0) {
            for (j = 0; j < r->n_win; j++)
                if (r->win[j] == (int)tid) return j;
            /* not in window: map via mod (deterministic fallback) */
            if (r->n_win > 0) return (int)((unsigned long)tid % (unsigned)r->n_win);
        }
    }
    return -1;
}

int residual_http_open(ResidualHttp **out, const char *base_url,
                       const char *window_path, int synthetic_n) {
    ResidualHttp *r;
    size_t n;
    const char *tmo;
    if (!out || !base_url || !base_url[0]) return -1;
    r = (ResidualHttp *)calloc(1, sizeof *r);
    if (!r) return -2;
    snprintf(r->base, sizeof r->base, "%s", base_url);
    /* strip trailing slash */
    n = strlen(r->base);
    while (n > 0 && r->base[n - 1] == '/') {
        r->base[--n] = '\0';
    }
    snprintf(r->completion_url, sizeof r->completion_url, "%s/completion",
             r->base);
    snprintf(r->models_url, sizeof r->models_url, "%s/v1/models", r->base);
    tmo = getenv("CNET_RESIDUAL_HTTP_TIMEOUT_MS");
    r->timeout_ms = (tmo && tmo[0]) ? atol(tmo) : 30000;

    if (window_path && window_path[0]) {
        int tmp[4096];
        int nw = gap_lane_load_ids(window_path, tmp, 4096);
        if (nw <= 0) {
            free(r);
            return -4;
        }
        r->win = (int *)malloc((size_t)nw * sizeof(int));
        if (!r->win) {
            free(r);
            return -2;
        }
        memcpy(r->win, tmp, (size_t)nw * sizeof(int));
        r->n_win = nw;
        r->owns_win = 1;
    } else {
        int i, nw = synthetic_n > 0 ? synthetic_n : 32;
        if (nw < 4) nw = 4;
        r->win = (int *)malloc((size_t)nw * sizeof(int));
        if (!r->win) {
            free(r);
            return -2;
        }
        for (i = 0; i < nw; i++) r->win[i] = 1000 + i;
        r->n_win = nw;
        r->owns_win = 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    *out = r;
    return 0;
}

void residual_http_close(ResidualHttp *r) {
    if (!r) return;
    if (r->owns_win && r->win) free(r->win);
    free(r);
}

int residual_http_window_n(const ResidualHttp *r) {
    return r ? r->n_win : 0;
}

const int *residual_http_window_ids(const ResidualHttp *r) {
    return r ? r->win : NULL;
}

Port residual_http_input_port(const ResidualHttp *r) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = r ? (size_t)r->n_win : 0;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "res_tok");
    return p;
}

Port residual_http_output_port(const ResidualHttp *r) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = r ? (size_t)r->n_win : 0;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "res_next");
    return p;
}

int residual_http_ping(const ResidualHttp *r) {
    struct MemBuf mb;
    long code = 0;
    int rc;
    if (!r) return -1;
    rc = http_get(r->models_url, r->timeout_ms, &mb, &code);
    free(mb.data);
    if (rc != 0) return -2;
    if (code < 200 || code >= 300) return -3;
    return 0;
}

int residual_http_oracle(const double *in, double *out, void *ctx) {
    ResidualHttp *r = (ResidualHttp *)ctx;
    int hot, tok, slot, i;
    char body[256];
    struct MemBuf mb;
    long code = 0;
    if (!r || !in || !out || r->n_win <= 0) return -1;
    hot = argmax_d(in, r->n_win);
    if (hot < 0 || hot >= r->n_win) return -1;
    tok = r->win[hot];
    /* llama.cpp completion: prompt as token id array, greedy next, probs. */
    snprintf(body, sizeof body,
             "{\"prompt\":[%d],\"n_predict\":1,\"temperature\":0,"
             "\"n_probs\":64,\"return_tokens\":true}",
             tok);
    if (http_post_json(r->completion_url, body, r->timeout_ms, &mb, &code) !=
        0) {
        free(mb.data);
        return -2;
    }
    if (code < 200 || code >= 300 || !mb.data) {
        free(mb.data);
        return -3;
    }
    slot = pick_window_slot(r, mb.data);
    free(mb.data);
    if (slot < 0 || slot >= r->n_win) return -4;
    for (i = 0; i < r->n_win; i++) out[i] = 0.0;
    out[slot] = 1.0;
    return 0;
}

/* Parse top_logprobs into per-window-slot scores. */
static void fill_wl_from_json(const ResidualHttp *r, const char *json,
                              float *wl) {
    const char *p;
    int j;
    if (!r || !json || !wl) return;
    for (j = 0; j < r->n_win; j++) wl[j] = -1e30f;
    p = json;
    while ((p = strstr(p, "\"id\"")) != NULL) {
        const char *q = strchr(p, ':');
        const char *lp;
        long idv;
        double logp = -1e9;
        int slot = -1;
        p += 4;
        if (!q) continue;
        if (parse_int_after(q + 1, &idv) != 0) continue;
        for (j = 0; j < r->n_win; j++) {
            if (r->win[j] == (int)idv) {
                slot = j;
                break;
            }
        }
        if (slot < 0) continue;
        lp = strstr(q, "\"logprob\"");
        if (lp && lp - q < 120) {
            const char *c = strchr(lp, ':');
            if (c) logp = strtod(c + 1, NULL);
        }
        if ((float)logp > wl[slot]) wl[slot] = (float)logp;
    }
}

int residual_http_window_logits(ResidualHttp *r, int hot_slot, float *wl) {
    char body[256];
    struct MemBuf mb;
    long code = 0;
    int tok;
    if (!r || !wl || hot_slot < 0 || hot_slot >= r->n_win) return -1;
    tok = r->win[hot_slot];
    snprintf(body, sizeof body,
             "{\"prompt\":[%d],\"n_predict\":1,\"temperature\":0,"
             "\"n_probs\":128,\"return_tokens\":true}",
             tok);
    if (http_post_json(r->completion_url, body, r->timeout_ms, &mb, &code) !=
        0) {
        free(mb.data);
        return -2;
    }
    if (code < 200 || code >= 300 || !mb.data) {
        free(mb.data);
        return -3;
    }
    fill_wl_from_json(r, mb.data, wl);
    free(mb.data);
    return 0;
}

int residual_http_oracle_topk(const double *in, double *out, void *ctx, int k) {
    ResidualHttp *r = (ResidualHttp *)ctx;
    float *wl = NULL;
    int hot, i, rnk, j;
    int used[8];
    if (!r || !in || !out || r->n_win <= 0) return -1;
    if (k < 1) k = 1;
    if (k > 8) k = 8;
    hot = argmax_d(in, r->n_win);
    if (hot < 0 || hot >= r->n_win) return -1;
    wl = (float *)malloc((size_t)r->n_win * sizeof(float));
    if (!wl) return -2;
    if (residual_http_window_logits(r, hot, wl) != 0) {
        free(wl);
        return -3;
    }
    for (i = 0; i < k * r->n_win; i++) out[i] = 0.0;
    for (rnk = 0; rnk < k; rnk++) {
        int best = -1;
        for (j = 0; j < r->n_win; j++) {
            int taken = 0, u;
            for (u = 0; u < rnk; u++)
                if (used[u] == j) taken = 1;
            if (taken) continue;
            if (best < 0 || wl[j] > wl[best]) best = j;
        }
        if (best < 0) best = 0;
        used[rnk] = best;
        out[rnk * r->n_win + best] = 1.0;
    }
    free(wl);
    return 0;
}

#ifndef CNET_RESIDUAL_HTTP_STANDALONE
int personal_ai_bind_residual_http(PersonalAi *ai, ResidualHttp *r,
                                   const char *name) {
    if (!ai || !r) return -1;
    return personal_ai_bind_residual(ai, name ? name : "residual_http",
                                     residual_http_oracle, r);
}

int personal_ai_auto_residual_http(PersonalAi *ai, ResidualHttp **owned) {
    const char *url;
    const char *win;
    ResidualHttp *r = NULL;
    int rc;
    if (owned) *owned = NULL;
    if (!ai) return -1;
    if (ai->hybrid.residual.bound) {
        return 0;
    }
    url = getenv("CNET_RESIDUAL_HTTP");
    if (!url || !url[0]) return 1;
    win = getenv("CNET_RESIDUAL_WINDOW");
    rc = residual_http_open(&r, url, win, 32);
    if (rc != 0) return -2;
    if (residual_http_ping(r) != 0) {
        /* Still bind — ping may race during server warm; oracle fails closed. */
    }
    if (personal_ai_bind_residual_http(ai, r, "residual_http") != 0) {
        residual_http_close(r);
        return -3;
    }
    if (owned) *owned = r;
    return 0;
}
#endif
