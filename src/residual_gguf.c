#include "../include/residual_gguf.h"
#include "../include/gap_lane.h"
#include "../include/cnet_pilot.h"
#include "../include/cce/cce_detect.h"
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ResidualGguf {
    cce_anymodel *am;
    cce_gguf_qwen2 *m;
    float *logits;
    int *win;
    int n_win;
    int vocab;
    int owns_win;
    int session_kv; /* CNET_RESIDUAL_SESSION_KV=1: grow cur_pos */
    int last_out_hot;
    CnetPilot pilot;
    char path[512];
};

static int argmax_d(const double *v, int n) {
    int i, b = 0;
    for (i = 1; i < n; i++)
        if (v[i] > v[b]) b = i;
    return b;
}

int residual_gguf_open(ResidualGguf **out, const char *gguf_path,
                       const char *window_path, int synthetic_n) {
    ResidualGguf *r;
    cce_result rc;
    if (!out || !gguf_path || !gguf_path[0]) return -1;
    r = (ResidualGguf *)calloc(1, sizeof *r);
    if (!r) return -2;
    snprintf(r->path, sizeof r->path, "%s", gguf_path);

    rc = cce_anymodel_open(&r->am, gguf_path);
    if (rc != CCE_OK || !r->am || !r->am->transformer) {
        free(r);
        return -3;
    }
    r->m = r->am->transformer;
    r->vocab = r->m->vocab_size;
    r->logits = (float *)malloc((size_t)r->vocab * sizeof(float));
    if (!r->logits) {
        residual_gguf_close(r);
        return -2;
    }

    if (window_path && window_path[0]) {
        int tmp[4096];
        int n = gap_lane_load_ids(window_path, tmp, 4096);
        if (n <= 0) {
            residual_gguf_close(r);
            return -4;
        }
        r->win = (int *)malloc((size_t)n * sizeof(int));
        if (!r->win) {
            residual_gguf_close(r);
            return -2;
        }
        memcpy(r->win, tmp, (size_t)n * sizeof(int));
        r->n_win = n;
        r->owns_win = 1;
        cce_gguf_qwen2_set_head_window(r->m, r->win, r->n_win);
    } else {
        int i, n = synthetic_n > 0 ? synthetic_n : 32;
        if (n > r->vocab) n = r->vocab;
        if (n < 4) n = 4;
        r->win = (int *)malloc((size_t)n * sizeof(int));
        if (!r->win) {
            residual_gguf_close(r);
            return -2;
        }
        /* Prefer mid-vocab region (avoid specials at 0..n) */
        {
            int base = r->vocab > 2000 ? 1000 : 0;
            if (base + n > r->vocab) base = 0;
            for (i = 0; i < n; i++) r->win[i] = base + i;
        }
        r->n_win = n;
        r->owns_win = 1;
        cce_gguf_qwen2_set_head_window(r->m, r->win, r->n_win);
    }

    {
        const char *sk = getenv("CNET_RESIDUAL_SESSION_KV");
        r->session_kv = (sk && sk[0] == '1') ? 1 : 0;
    }
    cnet_pilot_init(&r->pilot);
    cnet_pilot_from_env(&r->pilot);
    r->last_out_hot = -1;

    *out = r;
    return 0;
}

void residual_gguf_close(ResidualGguf *r) {
    if (!r) return;
    free(r->logits);
    if (r->owns_win) free(r->win);
    if (r->am) cce_anymodel_free(r->am);
    free(r);
}

void residual_gguf_session_reset(ResidualGguf *r) {
    if (!r || !r->m) return;
    r->m->cur_pos = 0;
    r->last_out_hot = -1;
}

int residual_gguf_session_mode(const ResidualGguf *r) {
    return r ? r->session_kv : 0;
}

uint64_t residual_gguf_pilot_recorded(const ResidualGguf *r) {
    return r ? r->pilot.recorded : 0;
}

uint64_t residual_gguf_pilot_hits(const ResidualGguf *r) {
    return r ? r->pilot.hits : 0;
}

int residual_gguf_pilot_consume(ResidualGguf *r, int *slot_order, int max_slots) {
    int n, i, j, w, s;
    unsigned char used[512];
    if (!r || !slot_order || max_slots <= 0) return 0;
    w = r->n_win > 0 ? r->n_win : max_slots;
    if (w > 512) w = 512;
    memset(used, 0, sizeof used);
    n = cnet_pilot_drain(&r->pilot, slot_order, max_slots);
    j = 0;
    for (i = 0; i < n; i++) {
        int h = slot_order[i];
        if (h < 0 || h >= w) continue;
        if (used[h]) continue;
        used[h] = 1;
        slot_order[j++] = h;
    }
    n = j;
    for (s = 0; s < w && n < max_slots; s++) {
        if (used[s]) continue;
        used[s] = 1;
        slot_order[n++] = s;
    }
    return n;
}

int residual_gguf_label_batch(ResidualGguf *r, const int *slot_order, int n_slots,
                              double *inputs, double *targets, int in_dim,
                              int out_dim) {
    int s, i, hot;
    int saved_session;
    double *in_row, *out_row;
    if (!r || !inputs || !targets || n_slots <= 0 || in_dim <= 0 || out_dim <= 0)
        return -1;
    /* Force probe mode for bit-stable mine labels. */
    saved_session = r->session_kv;
    r->session_kv = 0;
    residual_gguf_session_reset(r);
    for (s = 0; s < n_slots; s++) {
        hot = slot_order ? slot_order[s] : s;
        if (hot < 0) hot = 0;
        if (hot >= in_dim) hot = hot % in_dim;
        in_row = inputs + (size_t)s * (size_t)in_dim;
        out_row = targets + (size_t)s * (size_t)out_dim;
        for (i = 0; i < in_dim; i++) in_row[i] = (i == hot) ? 1.0 : 0.0;
        if (residual_gguf_oracle(in_row, out_row, r) != 0) {
            for (i = 0; i < out_dim; i++) out_row[i] = 0.0;
            if (out_dim > 0) out_row[hot % out_dim] = 1.0;
        }
    }
    r->session_kv = saved_session;
    residual_gguf_session_reset(r);
    return 0;
}

int residual_gguf_window_n(const ResidualGguf *r) {
    return r ? r->n_win : 0;
}

int residual_gguf_vocab(const ResidualGguf *r) {
    return r ? r->vocab : 0;
}

const int *residual_gguf_window_ids(const ResidualGguf *r) {
    return r ? r->win : NULL;
}

Port residual_gguf_input_port(const ResidualGguf *r) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = r ? (size_t)r->n_win : 0;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "res_tok");
    return p;
}

Port residual_gguf_output_port(const ResidualGguf *r) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = r ? (size_t)r->n_win : 0;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "res_next");
    return p;
}

int residual_gguf_oracle(const double *in, double *out, void *ctx) {
    ResidualGguf *r = (ResidualGguf *)ctx;
    int hot, tok, j, best_j;
    float best_l;
    if (!r || !r->m || !in || !out || r->n_win <= 0) return -1;

    hot = argmax_d(in, r->n_win);
    if (hot < 0 || hot >= r->n_win) return -1;
    tok = r->win[hot];

    /* Default: independent probe (bit-stable). Session mode grows KV for chat. */
    if (!r->session_kv)
        r->m->cur_pos = 0;
    if (cce_gguf_qwen2_forward_probes(r->m, &tok, 1, r->logits, r->vocab) !=
        CCE_OK)
        return -1;

    best_j = 0;
    best_l = r->logits[r->win[0]];
    for (j = 1; j < r->n_win; j++) {
        float l = r->logits[r->win[j]];
        if (l > best_l) {
            best_l = l;
            best_j = j;
        }
    }
    {
        int i;
        for (i = 0; i < r->n_win; i++) out[i] = 0.0;
        out[best_j] = 1.0;
    }
    r->last_out_hot = best_j;
    /* P5 research: hint next window slot for prefetch consumers. */
    if (r->pilot.enabled)
        (void)cnet_pilot_push(&r->pilot, best_j);
    return 0;
}

int personal_ai_bind_residual_gguf(PersonalAi *ai, ResidualGguf *r,
                                   const char *name) {
    if (!ai || !r) return -1;
    return personal_ai_bind_residual(ai, name ? name : "residual_gguf",
                                     residual_gguf_oracle, r);
}

int personal_ai_auto_residual_gguf(PersonalAi *ai, ResidualGguf **owned) {
    const char *path;
    const char *win;
    ResidualGguf *r = NULL;
    int rc;
    if (owned) *owned = NULL;
    if (!ai) return -1;
    /* Already bound (e.g. personal_ai_open auto-bind) — reuse. */
    if (ai->hybrid.residual.bound) {
        if (owned) *owned = ai->owned_residual;
        return 0;
    }
    path = getenv("CNET_RESIDUAL_GGUF");
    if (!path || !path[0]) return 1;
    win = getenv("CNET_RESIDUAL_WINDOW");
    rc = residual_gguf_open(&r, path, win, 32);
    if (rc != 0) return -2;
    if (personal_ai_bind_residual_gguf(ai, r, "residual_gguf") != 0) {
        residual_gguf_close(r);
        return -3;
    }
    if (owned) *owned = r;
    return 0;
}
