#include "cnet_c_speak.h"

#include "cce/cce_wordlm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tiny wrap leaf. Not the 321k compete intent WordLM. Not Teacher. */
#define CS_V 12
#define CS_CTX 2
#define CS_D 16
#define CS_HID 32
#define CS_EPOCHS 150
#define CS_GEN_MAX 12

static const char *const k_vocab[CS_V] = {
    "marble", "reports", "<val>", "from", "<host>", "via",
    "<contract>", ".", "under", "a", "certified", "slot"
};

#define CS_MARBLE 0
#define CS_REPORTS 1
#define CS_VAL 2
#define CS_FROM 3
#define CS_HOST 4
#define CS_VIA 5
#define CS_CONTRACT 6
#define CS_DOT 7
#define CS_UNDER 8

static const char *const k_corpus[] = {
    "marble reports <val> from <host> .",
    "marble reports <val> via <contract> .",
    "marble reports <val> from <host> under <contract> .",
    "marble reports <val> from <host> .",
    "marble reports <val> via <contract> .",
    "marble reports <val> from <host> .",
    "marble reports <val> via <contract> .",
    "marble reports <val> from <host> .",
};

static cce_wordlm *g_leaf;
static int g_ready;

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (dst == NULL || cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int tok_id(const char *w) {
    int i;
    if (w == NULL) return -1;
    for (i = 0; i < CS_V; i++)
        if (strcmp(k_vocab[i], w) == 0) return i;
    return -1;
}

static int tokenize_sent(const char *sent, int *ids, int cap) {
    char buf[128];
    char *save = NULL, *p;
    int n = 0;
    if (sent == NULL || ids == NULL || cap <= 0) return -1;
    snprintf(buf, sizeof buf, "%s", sent);
    for (p = strtok_r(buf, " ", &save); p != NULL;
         p = strtok_r(NULL, " ", &save)) {
        int id = tok_id(p);
        if (id < 0 || n >= cap) return -1;
        ids[n++] = id;
    }
    return n;
}

static int slot_filled(int id, const CnetCSpeakSlots *slots) {
    if (slots == NULL) return 0;
    if (id == CS_VAL) return slots->value[0] != '\0';
    if (id == CS_HOST) return slots->host[0] != '\0';
    if (id == CS_CONTRACT) return slots->contract[0] != '\0';
    return 0;
}

static const char *slot_text(int id, const CnetCSpeakSlots *slots) {
    if (slots == NULL) return "";
    if (id == CS_VAL) return slots->value;
    if (id == CS_HOST) return slots->host;
    if (id == CS_CONTRACT) return slots->contract;
    return "";
}

static int is_slot_tok(int id) {
    return id == CS_VAL || id == CS_HOST || id == CS_CONTRACT;
}

static int append_word(char *out, size_t cap, size_t *used, const char *word,
                       int first) {
    size_t n, need;
    if (out == NULL || used == NULL || word == NULL || word[0] == '\0')
        return -1;
    n = strlen(word);
    need = n + (first ? 0u : 1u);
    if (*used + need >= cap) return -1;
    if (!first) out[(*used)++] = ' ';
    memcpy(out + *used, word, n);
    *used += n;
    out[*used] = '\0';
    return 0;
}

static int generate_tokens(int *toks, int cap, unsigned *leaf_calls) {
    int ctx[CS_CTX];
    int n = 0, step, w;
    if (g_leaf == NULL || toks == NULL || cap < 3) return -1;
    ctx[0] = CS_MARBLE;
    ctx[1] = CS_REPORTS;
    toks[n++] = CS_MARBLE;
    toks[n++] = CS_REPORTS;
    for (step = 0; step < CS_GEN_MAX && n < cap; step++) {
        w = cce_wordlm_predict(g_leaf, ctx, NULL);
        if (leaf_calls != NULL) (*leaf_calls)++;
        if (w < 0 || w >= CS_V) return -1;
        toks[n++] = w;
        if (w == CS_DOT) break;
        ctx[0] = ctx[1];
        ctx[1] = w;
    }
    return n;
}

static int tokens_have(const int *toks, int n, int id) {
    int i;
    if (toks == NULL) return 0;
    for (i = 0; i < n; i++)
        if (toks[i] == id) return 1;
    return 0;
}

static int render_wrap(const int *toks, int n, const CnetCSpeakSlots *slots,
                       char *out, size_t cap) {
    size_t used = 0;
    int i, first = 1;
    char word[CNET_CSPEAK_SLOT];
    if (out == NULL || cap == 0 || toks == NULL || n <= 0) return -1;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        int id = toks[i];
        if (id == CS_DOT) {
            if (used + 1 >= cap) return -1;
            out[used++] = '.';
            out[used] = '\0';
            break;
        }
        if (is_slot_tok(id)) {
            if (!slot_filled(id, slots)) {
                /* Drop a dangling from/via/under before an empty slot. */
                if (!first && used > 0) {
                    char *sp = strrchr(out, ' ');
                    const char *last = sp ? sp + 1 : out;
                    if (strcmp(last, "from") == 0 || strcmp(last, "via") == 0 ||
                        strcmp(last, "under") == 0) {
                        if (sp) {
                            *sp = '\0';
                            used = (size_t)(sp - out);
                        } else {
                            out[0] = '\0';
                            used = 0;
                            first = 1;
                        }
                    }
                }
                continue;
            }
            copy_text(word, sizeof word, slot_text(id, slots));
        } else {
            copy_text(word, sizeof word, k_vocab[id]);
        }
        if (append_word(out, cap, &used, word, first) != 0) return -1;
        first = 0;
    }
    if (used == 0) return -1;
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 'a' + 'A');
    if (out[used - 1u] != '.') {
        if (used + 1 >= cap) return -1;
        out[used++] = '.';
        out[used] = '\0';
    }
    return 0;
}

static int train_leaf(unsigned seed) {
    int ctxs[64][CS_CTX];
    int tgts[64];
    int ids[16];
    int np = 0, s, n, i, ep;
    size_t nsent = sizeof k_corpus / sizeof k_corpus[0];
    cce_wordlm *m;
    int probe[CS_GEN_MAX + 2];
    int pn;
    unsigned dummy = 0;

    for (s = 0; s < (int)nsent; s++) {
        n = tokenize_sent(k_corpus[s], ids, 16);
        if (n < CS_CTX + 1) return -1;
        for (i = CS_CTX; i < n; i++) {
            if (np >= 64) return -1;
            ctxs[np][0] = ids[i - 2];
            ctxs[np][1] = ids[i - 1];
            tgts[np] = ids[i];
            np++;
        }
    }
    m = cce_wordlm_create(CS_V, CS_D, CS_CTX, CS_HID, seed);
    if (m == NULL) return -1;
    for (ep = 0; ep < CS_EPOCHS; ep++) {
        for (i = 0; i < np; i++)
            (void)cce_wordlm_train_step(m, ctxs[i], tgts[i], 0.05f);
    }
    if (g_leaf != NULL) cce_wordlm_free(g_leaf);
    g_leaf = m;
    pn = generate_tokens(probe, CS_GEN_MAX + 2, &dummy);
    if (pn < 3 || !tokens_have(probe, pn, CS_VAL)) {
        cce_wordlm_free(g_leaf);
        g_leaf = NULL;
        return -1;
    }
    return 0;
}

int cnet_c_speak_init(void) {
    static const unsigned seeds[] = {7u, 11u, 13u, 17u, 19u};
    size_t i;
    if (g_ready && g_leaf != NULL) return 0;
    g_ready = 0;
    for (i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        if (train_leaf(seeds[i]) == 0) {
            g_ready = 1;
            return 0;
        }
    }
    return -1;
}

void cnet_c_speak_shutdown(void) {
    if (g_leaf != NULL) {
        cce_wordlm_free(g_leaf);
        g_leaf = NULL;
    }
    g_ready = 0;
}

const char *cnet_c_speak_leaf(void) { return CNET_CSPEAK_LEAF; }

int cnet_c_speak_may_voice(const char *source, int never_voice_llm) {
    if (source == NULL || source[0] == '\0') return 0;
    if (strcmp(source, "LOCAL") == 0 || strcmp(source, "CNET") == 0 ||
        strcmp(source, "SELF") == 0)
        return 1;
    if (strcmp(source, "LLM") == 0 || strcmp(source, "TEACHER") == 0 ||
        strcmp(source, "RESIDUAL") == 0)
        return never_voice_llm ? 0 : 1;
    return 0;
}

static void clear_result(CnetCSpeakResult *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
}

static int refuse(CnetCSpeakResult *out, const char *why) {
    if (out == NULL) return -1;
    clear_result(out);
    copy_text(out->refusal, sizeof out->refusal, why);
    return 1;
}

int cnet_c_speak_wrap(const CnetCSpeakSlots *slots, CnetCSpeakResult *out) {
    int toks[CS_GEN_MAX + 2];
    int n;
    unsigned leaf_calls = 0;

    if (out == NULL) return -1;
    clear_result(out);
    if (slots == NULL || !slots->bound || slots->value[0] == '\0')
        return refuse(out, "no_bind");
    if (cnet_c_speak_init() != 0) return refuse(out, "leaf_unready");

    n = generate_tokens(toks, CS_GEN_MAX + 2, &leaf_calls);
    out->leaf_calls = leaf_calls;
    out->residual_calls = 0;
    if (n < 3 || !tokens_have(toks, n, CS_VAL))
        return refuse(out, "leaf_no_slot");
    if (render_wrap(toks, n, slots, out->spoken, sizeof out->spoken) != 0) {
        out->spoken[0] = '\0';
        return refuse(out, "render");
    }
    if (strstr(out->spoken, slots->value) == NULL) {
        out->spoken[0] = '\0';
        return refuse(out, "value_missing");
    }
    out->wrapped = 1;
    out->claimed_cert = 1;
    out->may_voice = 1;
    return 0;
}

int cnet_c_speak_after_lookup(const CnetChatLookupTurn *hop,
                              CnetCSpeakResult *out) {
    CnetCSpeakSlots slots;
    if (out == NULL) return -1;
    clear_result(out);
    if (hop == NULL || !hop->answered || !hop->report.bound ||
        hop->report.value[0] == '\0')
        return refuse(out, hop && hop->refusal[0] ? hop->refusal : "no_bind");
    memset(&slots, 0, sizeof slots);
    copy_text(slots.value, sizeof slots.value, hop->report.value);
    copy_text(slots.host, sizeof slots.host, hop->report.host);
    copy_text(slots.contract, sizeof slots.contract, CNET_LOOKUP_CONTRACT);
    copy_text(slots.sha256, sizeof slots.sha256, hop->report.sha256);
    slots.bound = 1;
    if (cnet_c_speak_wrap(&slots, out) != 0) return 1;
    out->residual_calls = hop->residual_calls;
    if (out->residual_calls != 0) {
        out->spoken[0] = '\0';
        out->wrapped = 0;
        out->claimed_cert = 0;
        copy_text(out->refusal, sizeof out->refusal, "residual_mouth");
        return 1;
    }
    return 0;
}

int cnet_c_speak_after_capsule(const char *value, const char *contract,
                               CnetCSpeakResult *out) {
    CnetCSpeakSlots slots;
    if (out == NULL) return -1;
    memset(&slots, 0, sizeof slots);
    copy_text(slots.value, sizeof slots.value, value);
    copy_text(slots.contract, sizeof slots.contract, contract);
    slots.bound = (slots.value[0] != '\0');
    return cnet_c_speak_wrap(&slots, out);
}

int cnet_c_speak_cd_ask_step(const CnetCSpeakSlots *bound,
                             const char *teacher_draft, const char *source,
                             int never_voice_llm, CnetCSpeakResult *out) {
    int rc;
    (void)teacher_draft;
    if (out == NULL) return -1;
    clear_result(out);
    out->may_voice = cnet_c_speak_may_voice(source, never_voice_llm);
    if (bound != NULL && bound->bound && bound->value[0] != '\0') {
        rc = cnet_c_speak_wrap(bound, out);
        if (rc == 0) {
            /* Bound A wrap is CNET speech even if a teacher draft exists. */
            out->may_voice = 1;
            if (teacher_draft != NULL && teacher_draft[0] != '\0' &&
                strcmp(out->spoken, teacher_draft) == 0) {
                out->spoken[0] = '\0';
                out->wrapped = 0;
                out->claimed_cert = 0;
                copy_text(out->refusal, sizeof out->refusal, "teacher_collision");
                return 1;
            }
        }
        return rc;
    }
    /* Nothing bound: refuse. Do not present a number/fact as certified.
       Teacher drafts stay logged; they are not spoken. */
    (void)refuse(out, "no_bind");
    out->may_voice = cnet_c_speak_may_voice(source, never_voice_llm);
    if (strcmp(source ? source : "", "LLM") == 0 && never_voice_llm)
        out->may_voice = 0;
    return 1;
}

int cnet_c_speak_slot_like(const char *answer) {
    size_t n, i, spaces = 0;
    if (answer == NULL || answer[0] == '\0') return 0;
    n = strlen(answer);
    if (n > 80) return 0;
    for (i = 0; i < n; i++) {
        if (answer[i] == '\n' || answer[i] == '.') return 0;
        if (isspace((unsigned char)answer[i])) spaces++;
    }
    return spaces <= 3;
}
