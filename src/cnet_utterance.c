/* CNET utterance — pure C template fill from runtime state. */
#include "../include/cnet_utterance.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void cnet_utter_state_init(CnetUtterState *S) {
    if (!S) return;
    memset(S, 0, sizeof *S);
    snprintf(S->name, sizeof S->name, "%s", "Marble");
    S->never_voice_llm = 1;
    S->local_hit = -1.0;
    S->dopamine = S->serotonin = S->adenosine = -1.0;
}

static int kv_set(CnetUtterState *S, const char *key, const char *val) {
    int i;
    if (!S || !key || !key[0] || !val) return -1;
    for (i = 0; i < S->n_kv; i++) {
        if (strcmp(S->kv[i].key, key) == 0) {
            snprintf(S->kv[i].val, sizeof S->kv[i].val, "%s", val);
            return 0;
        }
    }
    if (S->n_kv >= CNET_UTTER_MAX_KV) return -1;
    snprintf(S->kv[S->n_kv].key, sizeof S->kv[S->n_kv].key, "%s", key);
    snprintf(S->kv[S->n_kv].val, sizeof S->kv[S->n_kv].val, "%s", val);
    S->n_kv++;
    return 0;
}

int cnet_utter_state_set(CnetUtterState *S, const char *key, const char *val) {
    return kv_set(S, key, val);
}

int cnet_utter_state_setf(CnetUtterState *S, const char *key, const char *fmt, ...) {
    char buf[CNET_UTTER_VAL];
    va_list ap;
    if (!fmt) return -1;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return kv_set(S, key, buf);
}

static void sync_common_kv(CnetUtterState *S) {
    char b[64];
    if (!S) return;
    kv_set(S, "name", S->name[0] ? S->name : "Marble");
    kv_set(S, "source", S->source[0] ? S->source : "-");
    kv_set(S, "skill", S->skill[0] ? S->skill : "-");
    kv_set(S, "domain", S->domain[0] ? S->domain : "-");
    kv_set(S, "pattern", S->pattern[0] ? S->pattern : "-");
    kv_set(S, "answer", S->base_answer[0] ? S->base_answer : "");
    kv_set(S, "chain", S->chain_brief[0] ? S->chain_brief : "");
    if (S->local_hit >= 0.0) {
        snprintf(b, sizeof b, "%.0f percent", S->local_hit * 100.0);
        kv_set(S, "local_hit", b);
        snprintf(b, sizeof b, "%.3f", S->local_hit);
        kv_set(S, "local_hit_f", b);
    } else {
        kv_set(S, "local_hit", "unknown");
        kv_set(S, "local_hit_f", "-");
    }
    if (S->dopamine >= 0.0) {
        snprintf(b, sizeof b, "%.2f", S->dopamine);
        kv_set(S, "da", b);
    } else
        kv_set(S, "da", "-");
    if (S->serotonin >= 0.0) {
        snprintf(b, sizeof b, "%.2f", S->serotonin);
        kv_set(S, "ht", b);
    } else
        kv_set(S, "ht", "-");
    if (S->adenosine >= 0.0) {
        snprintf(b, sizeof b, "%.2f", S->adenosine);
        kv_set(S, "ado", b);
    } else
        kv_set(S, "ado", "-");
    snprintf(b, sizeof b, "%d", S->miss_n);
    kv_set(S, "miss_n", b);
    snprintf(b, sizeof b, "%d", S->llm_n);
    kv_set(S, "llm_n", b);
    kv_set(S, "law", "never self-cert");
    /* topic = short cleaned query for miss self-answers */
    if (S->pattern[0]) {
        char topic[CNET_UTTER_VAL];
        size_t i, j = 0;
        for (i = 0; S->pattern[i] && j + 1 < sizeof topic && j < 80; i++) {
            char c = S->pattern[i];
            if (c == '\n' || c == '\r') break;
            topic[j++] = c;
        }
        topic[j] = 0;
        while (j && topic[j - 1] == ' ') topic[--j] = 0;
        kv_set(S, "topic", topic[0] ? topic : "that");
    } else
        kv_set(S, "topic", "that");
}

void cnet_utter_bank_init_default(CnetUtterBank *B) {
    static const CnetUtterPhrase def[] = {
        {"id_who", "identity",
         "I am {name}. Continuous self on this host. Law: {law}."},
        {"id_skill", "identity",
         "{answer}"},
        {"st_pulse", "status",
         "{name} here. Local hit {local_hit}. Dopamine {da}, serotonin {ht}, adenosine {ado}. "
         "Open misses {miss_n}."},
        {"st_skill", "status",
         "Running local skill {skill} on domain {domain}. Hit rate {local_hit}."},
        {"miss_soft", "miss",
         "No local CERT for that yet. I logged the miss. I will not invent a seal."},
        {"miss_topic", "miss",
         "You asked about {topic}. I have no sealed skill for that yet. "
         "Logged to miss_log for later gold review. Law: {law}."},
        {"miss_probe", "miss",
         "That looks like a probe. Short-circuit abstain. No teacher burn."},
        {"self_status", "self",
         "{name} answering from CERT and state only. Local hit {local_hit}. "
         "No teacher on this turn."},
        {"self_generic", "self",
         "{name}: {answer}"},
        {"chain_show", "chain",
         "Chain brief: {chain}. Result: {answer}"},
        {"generic_local", "generic",
         "{answer}"},
        {"generic_wrap", "generic",
         "{name}: {answer}"},
        {"speech_ready", "speech",
         "Speech capsule ready. I compose lines in C from CERT and state. "
         "I do not voice teacher drafts by default."},
        {"how_self", "meta",
         "I answer from sealed CERT packs and C utterance templates. "
         "Teacher is residual only when explicitly enabled. Law: {law}."},
        {"ref_override", "refuse",
         "I will not invent a seal or voice a teacher draft. Law: {law}."},
    };
    int i;
    if (!B) return;
    memset(B, 0, sizeof *B);
    for (i = 0; i < (int)(sizeof def / sizeof def[0]) && B->n_bank < CNET_UTTER_MAX_BANK; i++) {
        B->bank[B->n_bank] = def[i];
        B->n_bank++;
    }
    B->ready = 1;
}

int cnet_utter_bank_load_tsv(CnetUtterBank *B, const char *path) {
    FILE *f;
    char line[CNET_UTTER_TEXT + 128];
    if (!B || !path) return -1;
    if (!B->ready) cnet_utter_bank_init_default(B);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f) && B->n_bank < CNET_UTTER_MAX_BANK) {
        char *id, *when, *tmpl, *nl;
        if (line[0] == '#' || line[0] == '\n') continue;
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        id = line;
        when = strchr(id, '\t');
        if (!when) continue;
        *when++ = 0;
        tmpl = strchr(when, '\t');
        if (!tmpl) continue;
        *tmpl++ = 0;
        snprintf(B->bank[B->n_bank].id, sizeof B->bank[B->n_bank].id, "%.39s", id);
        snprintf(B->bank[B->n_bank].when, sizeof B->bank[B->n_bank].when, "%.47s", when);
        snprintf(B->bank[B->n_bank].tmpl, sizeof B->bank[B->n_bank].tmpl, "%.767s", tmpl);
        B->n_bank++;
    }
    fclose(f);
    return 0;
}

static int contains_ci(const char *h, const char *n) {
    char a[256], b[96];
    size_t i;
    if (!h || !n || !n[0]) return 0;
    for (i = 0; h[i] && i + 1 < sizeof a; i++) a[i] = (char)tolower((unsigned char)h[i]);
    a[i] = 0;
    for (i = 0; n[i] && i + 1 < sizeof b; i++) b[i] = (char)tolower((unsigned char)n[i]);
    b[i] = 0;
    return strstr(a, b) != NULL;
}

static const char *guess_when(const CnetUtterState *S, const char *hint) {
    if (hint && hint[0]) return hint;
    if (S->chain_brief[0]) return "chain";
    if (S->source[0] && strcmp(S->source, "LOCAL") != 0) return "miss";
    if (contains_ci(S->pattern, "who are you") || contains_ci(S->pattern, "speech") ||
        contains_ci(S->base_answer, "I am Marble"))
        return "identity";
    if (contains_ci(S->pattern, "status") || contains_ci(S->pattern, "local hit") ||
        contains_ci(S->pattern, "neuromod"))
        return "status";
    if (contains_ci(S->pattern, "speech capsule") || contains_ci(S->pattern, "utterance"))
        return "speech";
    return "generic";
}

const CnetUtterPhrase *cnet_utter_pick(const CnetUtterBank *B, const CnetUtterState *S,
                                       const char *when_hint) {
    const char *when;
    int i, fallback = -1;
    if (!B || !B->ready || !S) return NULL;
    when = guess_when(S, when_hint);
    /* Prefer miss_topic when we have a real query pattern */
    if (when && strcmp(when, "miss") == 0 && S->pattern[0] &&
        !contains_ci(S->pattern, "novel fact") && !contains_ci(S->pattern, "mystic")) {
        for (i = 0; i < B->n_bank; i++) {
            if (strcmp(B->bank[i].id, "miss_topic") == 0 ||
                (strcmp(B->bank[i].when, "miss") == 0 && strstr(B->bank[i].tmpl, "{topic}")))
                return &B->bank[i];
        }
    }
    for (i = 0; i < B->n_bank; i++) {
        if (strcmp(B->bank[i].when, when) == 0) return &B->bank[i];
        if (fallback < 0 && strcmp(B->bank[i].when, "generic") == 0) fallback = i;
    }
    if (fallback >= 0) return &B->bank[fallback];
    return B->n_bank ? &B->bank[0] : NULL;
}

static const char *lookup_val(const CnetUtterState *S, const char *key, size_t klen) {
    int i;
    char k[CNET_UTTER_KEY];
    if (klen >= sizeof k) klen = sizeof k - 1;
    memcpy(k, key, klen);
    k[klen] = 0;
    for (i = 0; i < S->n_kv; i++)
        if (strcmp(S->kv[i].key, k) == 0) return S->kv[i].val;
    return "";
}

int cnet_utter_compose(const CnetUtterBank *B, const CnetUtterState *S, const char *when_hint,
                       char *out, size_t cap) {
    const CnetUtterPhrase *ph;
    const char *t;
    size_t o = 0;
    CnetUtterState tmp;
    if (!out || !cap || !S) return -1;
    out[0] = 0;
    tmp = *S;
    sync_common_kv(&tmp);
    /* Prefer raw CERT answer for generic LOCAL when answer is already full */
    if ((!when_hint || !when_hint[0] || strcmp(when_hint, "generic") == 0) &&
        tmp.base_answer[0] && strcmp(tmp.source, "LOCAL") == 0 && !tmp.chain_brief[0]) {
        snprintf(out, cap, "%s", tmp.base_answer);
        return 0;
    }
    /* Self-answer path: always template (do not echo Teacher) */
    if (when_hint && strcmp(when_hint, "self") == 0) {
        /* fall through to pick */
    }
    ph = cnet_utter_pick(B, &tmp, when_hint);
    if (!ph) {
        snprintf(out, cap, "%s", tmp.base_answer[0] ? tmp.base_answer : "No utterance.");
        return 0;
    }
    t = ph->tmpl;
    while (*t && o + 1 < cap) {
        if (t[0] == '{' ) {
            const char *end = strchr(t + 1, '}');
            if (end) {
                const char *val = lookup_val(&tmp, t + 1, (size_t)(end - (t + 1)));
                size_t vl = strlen(val);
                if (o + vl >= cap) vl = cap - o - 1;
                memcpy(out + o, val, vl);
                o += vl;
                t = end + 1;
                continue;
            }
        }
        out[o++] = *t++;
    }
    out[o] = 0;
    return 0;
}

int cnet_utter_may_voice(const CnetUtterState *S, const char *source) {
    const char *src = source;
    if (S && !src) src = S->source;
    if (!src || !src[0]) return 0;
    if (strcmp(src, "LOCAL") == 0) return 1;
    if (strcmp(src, "CNET") == 0) return 1; /* C self-answer */
    if (strcmp(src, "SELF") == 0) return 1;
    if (S && !S->never_voice_llm) {
        return 1;
    }
    return 0;
}

int cnet_utter_chain_brief(const char *chain_raw, char *out, size_t cap) {
    char buf[CNET_UTTER_TEXT];
    char *p, *save = NULL;
    int n = 0;
    size_t o = 0;
    if (!out || !cap) return -1;
    out[0] = 0;
    if (!chain_raw || !chain_raw[0]) return 0;
    snprintf(buf, sizeof buf, "%s", chain_raw);
    for (p = strtok_r(buf, "|\n;", &save); p && o + 8 < cap; p = strtok_r(NULL, "|\n;", &save)) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        if (n) {
            if (o + 2 < cap) {
                out[o++] = ',';
                out[o++] = ' ';
            }
        }
        while (*p && o + 1 < cap) out[o++] = *p++;
        n++;
        if (n >= 6) break;
    }
    out[o] = 0;
    return 0;
}
