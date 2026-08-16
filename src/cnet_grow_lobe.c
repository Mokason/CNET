#include "cnet_grow_lobe.h"

#include "cce/cce_transformer_qat.h"
#include "cnet_held_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GROW_V 96
#define GROW_T 48
#define GROW_PAD 0
#define GROW_ASST_MARK "<|im_start|>assistant"

/* In-house English, not harvested assistant prose. External fuel. */
static const char *const k_english[] = {
    "the cat sat on the mat. ",
    "the cat sat on the mat. ",
    "the cat sat on the mat. ",
    "the cat sat on the mat. ",
    "the cat sat on the mat. ",
    "the cat sat on the mat. ",
    "the dog ran in the park. ",
    "birds fly south in winter. ",
    "ask a short clear question. ",
    "the sun is warm today. ",
    "she reads a book at night. ",
    "he walks home after work. ",
    "we open the door and sit. ",
    NULL
};

static int enc_char(char c) {
    unsigned u = (unsigned char)c;
    if (u >= 32u && u <= 126u) return (int)(u - 31u);
    return GROW_PAD;
}

static char dec_char(int id) {
    if (id <= 0 || id > 95) return ' ';
    return (char)(id + 31);
}

struct CnetGrowLobe {
    CnetGrowPair pairs[CNET_GROW_PAIRS];
    int n;
    int head;
    int refused;
    cce_transformer_qat *net;
    int trained;
};

static int looks_tier_a(const char *text) {
    if (text == NULL) return 0;
    if (strstr(text, "Marble reports") != NULL) return 1;
    if (strstr(text, "claimed_cert=1") != NULL) return 1;
    if (strstr(text, "increment_mod256") != NULL && strstr(text, "42") != NULL)
        return 1;
    return 0;
}

int cnet_grow_admit(const CnetGrowPair *pair) {
    if (pair == NULL || pair->text[0] == '\0') return 1;
    if (pair->source == CNET_GROW_SRC_TIER_A) return 1;
    if (pair->source == CNET_GROW_SRC_NONE) return 1;
    if (looks_tier_a(pair->text)) return 1;
    if (pair->source != CNET_GROW_SRC_EXTERNAL &&
        pair->source != CNET_GROW_SRC_USER &&
        pair->source != CNET_GROW_SRC_TOOL)
        return 1;
    return 0;
}

static void fill_ctx(const char *s, int end, int *tok, int T) {
    int n = s ? (int)strlen(s) : 0;
    int i;
    for (i = 0; i < T; ++i) {
        int src = end - T + i;
        tok[i] = (src >= 0 && src < n) ? enc_char(s[src]) : GROW_PAD;
    }
}

static int argmax_logits(const float *lg, int n) {
    int b = 1, i;
    if (lg == NULL || n <= 1) return GROW_PAD;
    for (i = 2; i < n; ++i)
        if (lg[i] > lg[b]) b = i;
    return b;
}

int cnet_grow_open(CnetGrowLobe **out) {
    CnetGrowLobe *l;
    cce_transformer_qat_config cfg;
    if (out == NULL) return -1;
    *out = NULL;
    l = (CnetGrowLobe *)calloc(1, sizeof *l);
    if (l == NULL) return -1;
    memset(&cfg, 0, sizeof cfg);
    cfg.n_layer = 1;
    cfg.n_embd = 48;
    cfg.n_head = 4;
    cfg.mlp_hidden = 96;
    cfg.vocab = GROW_V;
    cfg.block_size = GROW_T;
    cfg.seed = 20260816u;
    cfg.qat_qkv = 0;
    cfg.qat_proj = 0;
    cfg.qat_mlp = 0;
    cfg.qat_head = 0;
    cfg.qat_emb = 0;
    l->net = cce_transformer_qat_create(&cfg);
    if (l->net == NULL) {
        free(l);
        return -1;
    }
    *out = l;
    return 0;
}

void cnet_grow_close(CnetGrowLobe *l) {
    if (l == NULL) return;
    cce_transformer_qat_free(l->net);
    free(l);
}

int cnet_grow_ingest(CnetGrowLobe *l, const CnetGrowPair *pair) {
    if (l == NULL || pair == NULL) return -1;
    if (cnet_grow_admit(pair) != 0) {
        l->refused++;
        return 1;
    }
    if (l->n < CNET_GROW_PAIRS) {
        l->pairs[l->n] = *pair;
        l->n++;
        return 0;
    }
    l->pairs[l->head] = *pair;
    l->head = (l->head + 1) % CNET_GROW_PAIRS;
    return 0;
}

int cnet_grow_wrap_chat(char *out, size_t cap, const char *user,
                        const char *assistant) {
    int n;
    if (out == NULL || cap < 64) return -1;
    n = snprintf(out, cap,
                 "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n"
                 "<|im_start|>user\n%s<|im_end|>\n"
                 "<|im_start|>assistant\n%s<|im_end|>\n",
                 user ? user : "", assistant ? assistant : "");
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

int cnet_grow_seed_chat_template(CnetGrowLobe *l) {
    static const struct {
        const char *user;
        const char *asst;
    } turns[] = {
        {"hello", "hello."},
        {"what is rain?", "rain is water from clouds."},
        {"say one word", "yes."},
        {"continue", ""},
    };
    int i, n = 0;
    if (l == NULL) return -1;
    for (i = 0; i < 4; ++i) {
        CnetGrowPair p;
        memset(&p, 0, sizeof p);
        if (cnet_grow_wrap_chat(p.text, sizeof p.text, turns[i].user,
                                turns[i].asst) != 0)
            continue;
        p.target = -1;
        p.source = CNET_GROW_SRC_EXTERNAL;
        if (cnet_grow_ingest(l, &p) == 0) n++;
    }
    return n;
}

int cnet_grow_seed_english(CnetGrowLobe *l) {
    int i, n = 0;
    if (l == NULL) return -1;
    for (i = 0; k_english[i] != NULL; ++i) {
        CnetGrowPair p;
        memset(&p, 0, sizeof p);
        snprintf(p.text, sizeof p.text, "%s", k_english[i]);
        p.target = -1;
        p.source = CNET_GROW_SRC_EXTERNAL;
        if (cnet_grow_ingest(l, &p) == 0) n++;
    }
    return n;
}

int cnet_grow_distill_held(CnetGrowLobe *l, const char *const *prompts, int n) {
    int i, got = 0;
    if (l == NULL || prompts == NULL || n <= 0) return -1;
    for (i = 0; i < n; ++i) {
        CnetGrowPair p;
        char buf[CNET_GROW_TEXT];
        if (prompts[i] == NULL || prompts[i][0] == '\0') continue;
        if (cnet_held_model_ask(prompts[i], buf, sizeof buf) != 0 ||
            buf[0] == '\0')
            continue;
        {
            /* One short sentence is what a 48d char LM can actually fit. */
            int bl = (int)strlen(buf);
            if (bl > 96) {
                int cut = 96;
                while (cut > 48 && buf[cut] != ' ' && buf[cut] != '.')
                    cut--;
                buf[cut] = '\0';
            }
        }
        memset(&p, 0, sizeof p);
        /* Compact turn: skip the long system block so T=48 sees English. */
        if (snprintf(p.text, sizeof p.text,
                     "<|im_start|>user\n%.80s<|im_end|>\n"
                     "<|im_start|>assistant\n%.96s<|im_end|>\n",
                     prompts[i], buf) < 0) {
            snprintf(p.text, sizeof p.text, "%s", buf);
        }
        p.target = -1;
        p.source = CNET_GROW_SRC_EXTERNAL;
        if (cnet_grow_ingest(l, &p) == 0) got++;
    }
    return got;
}

int cnet_grow_count(const CnetGrowLobe *l) { return l ? l->n : 0; }

int cnet_grow_refused(const CnetGrowLobe *l) { return l ? l->refused : 0; }

static int last_index(const CnetGrowLobe *l) {
    if (l == NULL || l->n <= 0) return -1;
    if (l->n < CNET_GROW_PAIRS) return l->n - 1;
    return (l->head + CNET_GROW_PAIRS - 1) % CNET_GROW_PAIRS;
}

/* age 0 = newest. */
static int pair_at_age(const CnetGrowLobe *l, int age) {
    if (l == NULL || l->n <= 0 || age < 0 || age >= l->n) return -1;
    if (l->n < CNET_GROW_PAIRS) return l->n - 1 - age;
    return (l->head + CNET_GROW_PAIRS - 1 - age) % CNET_GROW_PAIRS;
}

/* First char of the assistant body, or 3 for bare English. */
static int body_start(const char *s) {
    const char *p, *nl;
    if (s == NULL || s[0] == '\0') return 0;
    p = strstr(s, GROW_ASST_MARK);
    if (p != NULL) {
        nl = strchr(p, '\n');
        if (nl != NULL && nl[1] != '\0') return (int)(nl + 1 - s);
        return (int)(p - s);
    }
    return (s[1] && s[2]) ? 3 : 1;
}

static int skip_markup(const char *s, int pos, int n) {
    if (pos < n && s[pos] == '<') {
        while (pos < n && s[pos] != '>')
            pos++;
        if (pos < n) pos++;
    }
    return pos;
}

static double train_one_text(CnetGrowLobe *l, const char *s, float lr) {
    int n, pos, start, body_only;
    double last = 0.0;
    if (l == NULL || s == NULL) return 0.0;
    n = (int)strlen(s);
    if (n < 2) return 0.0;
    start = body_start(s);
    body_only = (start > 8);
    for (pos = 1; pos < n; ++pos) {
        int tok[GROW_T], tgt;
        if (body_only && pos < start && (pos % 4) != 0) continue;
        if (s[pos] == '<') {
            pos = skip_markup(s, pos, n);
            if (pos >= n) break;
            pos--; /* for-loop ++ lands on the char after '>' */
            continue;
        }
        fill_ctx(s, pos, tok, GROW_T);
        tgt = enc_char(s[pos]);
        if (tgt == GROW_PAD) tgt = enc_char(' ');
        last = cce_transformer_qat_step(l->net, tok, GROW_T, NULL, tgt, lr);
    }
    return last;
}

int cnet_grow_train(CnetGrowLobe *l, int steps, double *loss_out) {
    int s, pi;
    double last = 0.0;
    if (l == NULL || l->net == NULL || l->n <= 0 || steps <= 0) return -1;
    /* Stay FP unless there is a real second half. train(1) used to flip QAT
       immediately (s == steps/2 → 0 == 0) and wipe the last pair. */
    cce_transformer_qat_set_qat(l->net, 0, 0, 0, 0, 0);
    for (s = 0; s < steps; ++s) {
        if (steps > 1 && s == steps / 2)
            cce_transformer_qat_set_qat(l->net, 1, 1, 1, 1, 1);
        for (pi = 0; pi < l->n; ++pi)
            last = train_one_text(l, l->pairs[pi].text, 0.012f);
    }
    l->trained = 1;
    if (loss_out) *loss_out = last;
    return 0;
}

int cnet_grow_train_last(CnetGrowLobe *l, int passes, double *loss_out) {
    int idx, s;
    double last = 0.0;
    const CnetGrowPair *p;
    if (l == NULL || l->net == NULL || passes <= 0) return -1;
    idx = last_index(l);
    if (idx < 0) return -1;
    p = &l->pairs[idx];
    if ((int)strlen(p->text) < 2) return 1;
    cce_transformer_qat_set_qat(l->net, 0, 0, 0, 0, 0);
    for (s = 0; s < passes; ++s)
        last = train_one_text(l, p->text, 0.025f);
    l->trained = 1;
    if (loss_out) *loss_out = last;
    return 0;
}

int cnet_grow_train_recent(CnetGrowLobe *l, int window, int passes,
                           double *loss_out) {
    int s, age, w;
    double last = 0.0;
    if (l == NULL || l->net == NULL || l->n <= 0 || passes <= 0) return -1;
    if (window < 1) window = 1;
    w = window < l->n ? window : l->n;
    cce_transformer_qat_set_qat(l->net, 0, 0, 0, 0, 0);
    for (s = 0; s < passes; ++s) {
        for (age = 0; age < w; ++age) {
            int idx = pair_at_age(l, age);
            if (idx < 0) continue;
            last = train_one_text(l, l->pairs[idx].text, 0.018f);
        }
    }
    l->trained = 1;
    if (loss_out) *loss_out = last;
    return 0;
}

static int score_text(CnetGrowLobe *l, const char *s, int *hits, int *tot) {
    int n, pos, start;
    float lg[GROW_V];
    if (l == NULL || s == NULL || hits == NULL || tot == NULL) return -1;
    n = (int)strlen(s);
    start = body_start(s);
    if (n < start + 2) return 0;
    for (pos = start; pos < n; ++pos) {
        int tok[GROW_T], want, got;
        if (s[pos] == '<') {
            pos = skip_markup(s, pos, n);
            if (pos >= n) break;
            pos--;
            continue;
        }
        fill_ctx(s, pos, tok, GROW_T);
        if (cce_transformer_qat_logits(l->net, tok, GROW_T, lg) != 0)
            continue;
        want = enc_char(s[pos]);
        got = argmax_logits(lg, GROW_V);
        if (got == want) (*hits)++;
        (*tot)++;
    }
    return 0;
}

int cnet_grow_prefix_acc(CnetGrowLobe *l, double *acc_out) {
    int pi, hits = 0, tot = 0;
    if (acc_out) *acc_out = 0.0;
    if (l == NULL || l->net == NULL || l->n <= 0) return -1;
    for (pi = 0; pi < l->n; ++pi)
        score_text(l, l->pairs[pi].text, &hits, &tot);
    if (tot <= 0) return 1;
    if (acc_out) *acc_out = (double)hits / (double)tot;
    return 0;
}

int cnet_grow_prefix_acc_recent(CnetGrowLobe *l, int window, double *acc_out) {
    int age, w, hits = 0, tot = 0;
    if (acc_out) *acc_out = 0.0;
    if (l == NULL || l->net == NULL || l->n <= 0) return -1;
    if (window < 1) window = 1;
    w = window < l->n ? window : l->n;
    for (age = 0; age < w; ++age) {
        int idx = pair_at_age(l, age);
        if (idx < 0) continue;
        score_text(l, l->pairs[idx].text, &hits, &tot);
    }
    if (tot <= 0) return 1;
    if (acc_out) *acc_out = (double)hits / (double)tot;
    return 0;
}

int cnet_grow_may_speak(const CnetGrowLobe *l) {
    return (l && l->trained && l->n > 0) ? 1 : 0;
}

int cnet_grow_speak(CnetGrowLobe *l, const char *prompt, char *out, size_t cap,
                    int n_new) {
    char ctx[CNET_GROW_TEXT];
    int t, tok[GROW_T];
    float lg[GROW_V];
    size_t o = 0;
    if (out == NULL || cap == 0) return -1;
    out[0] = '\0';
    if (!cnet_grow_may_speak(l) || l->net == NULL) return 1;
    if (n_new < 1) n_new = 1;
    if (n_new > 64) n_new = 64;
    snprintf(ctx, sizeof ctx, "%s", prompt ? prompt : "");
    for (t = 0; t < n_new && o + 1u < cap; ++t) {
        int id;
        int n = (int)strlen(ctx);
        fill_ctx(ctx, n, tok, GROW_T);
        if (cce_transformer_qat_logits(l->net, tok, GROW_T, lg) != 0) break;
        id = argmax_logits(lg, GROW_V);
        out[o++] = dec_char(id);
        out[o] = '\0';
        if (n + 1 < (int)sizeof ctx) {
            ctx[n] = dec_char(id);
            ctx[n + 1] = '\0';
        }
        if (out[o - 1u] == '.') break;
    }
    return o > 0u ? 0 : 1;
}

int cnet_grow_claimed_cert(const CnetGrowLobe *l) {
    (void)l;
    return 0;
}
