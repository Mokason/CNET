#include "cnet_rlm.h"

#include "cnet_core_serve.h"
#include "cnet_ember.h"
#include "cnet_live_miss.h"
#include "cnet_lookup.h"
#include "cnet_skill_lane.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int push_step(CnetRlmResult *out, const CnetHemiResult *h, const char *note);
static void set_final(CnetRlmResult *out, const CnetHemiResult *h);

void cnet_rlm_policy_default(CnetRlmPolicy *p) {
    if (p == NULL) return;
    memset(p, 0, sizeof *p);
    cnet_hemi_policy_default(&p->core);
    p->max_steps = 3;
    p->use_capsule_loop = 1;
    p->allow_ember_draft = 0; /* leftover mouths killed unless ops opt-in */
}

void cnet_rlm_session_init(CnetRlmSession *s) {
    if (s == NULL) return;
    memset(s, 0, sizeof *s);
    s->open = 1;
}

void cnet_rlm_session_clear(CnetRlmSession *s) {
    if (s == NULL) return;
    memset(s, 0, sizeof *s);
}

void cnet_rlm_session_remember(CnetRlmSession *s, const char *skill,
                               const char *value) {
    int i;
    if (s == NULL || !s->open) return;
    if (skill == NULL || skill[0] == '\0') return;
    if (s->n >= CNET_RLM_SESSION_HOPS) {
        for (i = 1; i < CNET_RLM_SESSION_HOPS; i++) {
            memcpy(s->skill[i - 1], s->skill[i], sizeof s->skill[0]);
            memcpy(s->value[i - 1], s->value[i], sizeof s->value[0]);
        }
        s->n = CNET_RLM_SESSION_HOPS - 1;
    }
    copy_text(s->skill[s->n], sizeof s->skill[0], skill);
    copy_text(s->value[s->n], sizeof s->value[0], value ? value : "");
    s->n++;
}

/* Resolve "again" / "same" from session CERT. Bare skill names are NOT
   recalled — that falsely CERTs a stale value when the user meant a fresh
   invoke. */
static int try_session_recall(const char *turn, CnetRlmSession *session,
                              CnetRlmResult *out) {
    CnetHemiResult hr;
    const char *skill;
    const char *val;
    if (session == NULL || !session->open || session->n <= 0 || turn == NULL)
        return -2;
    skill = session->skill[session->n - 1];
    val = session->value[session->n - 1];
    if (skill[0] == '\0' || val[0] == '\0') return -2;
    if (strcmp(turn, "again") != 0 && strcmp(turn, "same") != 0 &&
        strcmp(turn, "same again") != 0)
        return -2;
    memset(&hr, 0, sizeof hr);
    hr.via_core = 1;
    hr.bound = 1;
    hr.claimed_cert = 1;
    hr.hemi = CNET_HEMI_CORE;
    hr.plane = CNET_CORE_PLANE_CERT;
    hr.source = CNET_HEMI_SRC_SKILL_EXACT;
    hr.may_voice = 1;
    hr.intent = CNET_CORE_INTENT_LOGIC;
    copy_text(hr.skill, sizeof hr.skill, skill);
    copy_text(hr.value, sizeof hr.value, val);
    copy_text(hr.spoken, sizeof hr.spoken, val);
    push_step(out, &hr, "session");
    set_final(out, &hr);
    out->session_used = 1;
    return 0;
}

static int try_ember_draft(const char *turn, CnetRlmResult *out) {
    static CnetEmber g_ember;
    static int g_ember_tried;
    CnetEmberDraftReport er;
    CnetHemiResult hr;
    char draft[CNET_RLM_TEXT];
    int rc;
    if (!g_ember_tried) {
        g_ember_tried = 1;
        if (cnet_ember_open(&g_ember, NULL) != 0) g_ember.open = 0;
    }
    if (!g_ember.open) return -2;
    memset(&er, 0, sizeof er);
    draft[0] = '\0';
    rc = cnet_ember_draft(&g_ember, turn, draft, sizeof draft, &er);
    memset(&hr, 0, sizeof hr);
    hr.via_core = 1;
    hr.intent = out->intent;
    hr.residual_calls = 1;
    hr.open_chat = 0;
    hr.claimed_cert = 0;
    hr.may_voice = 0;
    hr.plane = CNET_CORE_PLANE_NONE;
    hr.bound = 0;
    copy_text(hr.skill, sizeof hr.skill, "cnet_ember");
    if (rc == 0 && draft[0]) {
        /* Draft text is residual — never CERT, never voiced as sealed. */
        copy_text(hr.value, sizeof hr.value, draft);
        copy_text(hr.spoken, sizeof hr.spoken, draft);
        copy_text(hr.refusal, sizeof hr.refusal, "ember_draft_not_cert");
        push_step(out, &hr, "ember");
        set_final(out, &hr);
        out->ember_draft = 1;
        copy_text(out->summary, sizeof out->summary, draft);
        (void)cnet_ember_note_miss("ember_draft", turn);
        return 1; /* abstain from CERT; draft in summary/value */
    }
    copy_text(hr.refusal, sizeof hr.refusal, "ember_abstain");
    copy_text(hr.spoken, sizeof hr.spoken, "ember_abstain");
    push_step(out, &hr, "ember");
    set_final(out, &hr);
    return 1;
}

int cnet_rlm_is_chain_turn(const char *turn) {
    const char *p;
    if (turn == NULL || turn[0] == '\0') return 0;
    if (strstr(turn, " | ") != NULL) return 1;
    for (p = turn; *p; ++p) {
        if ((p == turn || isspace((unsigned char)p[-1])) &&
            (p[0] == 't' || p[0] == 'T') && (p[1] == 'h' || p[1] == 'H') &&
            (p[2] == 'e' || p[2] == 'E') && (p[3] == 'n' || p[3] == 'N') &&
            (p[4] == '\0' || isspace((unsigned char)p[4])))
            return 1;
    }
    return 0;
}

static void clear_rlm(CnetRlmResult *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
}

static void hemi_from_capsule(const CnetCapsuleLoopResult *loop,
                              CnetHemiResult *h) {
    memset(h, 0, sizeof *h);
    h->via_core = 1;
    h->intent = CNET_CORE_INTENT_LOGIC;
    if (loop == NULL) {
        h->source = CNET_HEMI_SRC_ABSTAIN;
        return;
    }
    copy_text(h->skill, sizeof h->skill, loop->skill);
    copy_text(h->value, sizeof h->value, loop->value);
    copy_text(h->spoken, sizeof h->spoken, loop->spoken);
    copy_text(h->refusal, sizeof h->refusal, loop->refusal);
    h->bound = loop->bound;
    h->claimed_cert = loop->claimed_cert;
    h->residual_calls = 0;
    h->teacher_calls = 0;
    h->open_chat = 0;
    if (loop->bound && loop->claimed_cert) {
        h->hemi = CNET_HEMI_CORE;
        h->plane = CNET_CORE_PLANE_CERT;
        h->source = CNET_HEMI_SRC_SKILL_EXACT;
        h->may_voice = 1;
    } else {
        h->hemi = CNET_HEMI_NONE;
        h->plane = CNET_CORE_PLANE_NONE;
        h->source = CNET_HEMI_SRC_ABSTAIN;
        h->may_voice = 0;
        if (h->refusal[0] == '\0')
            copy_text(h->refusal, sizeof h->refusal, "capsule_abstain");
    }
}

static void hemi_from_call(const CnetCapsuleCall *c, CnetHemiResult *h) {
    memset(h, 0, sizeof *h);
    h->via_core = 1;
    h->intent = CNET_CORE_INTENT_LOGIC;
    h->residual_calls = 0;
    h->teacher_calls = 0;
    h->open_chat = 0;
    if (c == NULL) {
        h->source = CNET_HEMI_SRC_ABSTAIN;
        return;
    }
    copy_text(h->skill, sizeof h->skill, c->name);
    copy_text(h->value, sizeof h->value, c->value);
    copy_text(h->refusal, sizeof h->refusal, c->refusal);
    if (c->kind == CNET_CAPSULE_CALL_EXACT && c->bound && c->claimed_cert) {
        h->bound = 1;
        h->claimed_cert = 1;
        h->hemi = CNET_HEMI_CORE;
        h->plane = CNET_CORE_PLANE_CERT;
        h->source = CNET_HEMI_SRC_SKILL_EXACT;
        h->may_voice = 1;
        copy_text(h->spoken, sizeof h->spoken, c->value);
    } else {
        h->source = CNET_HEMI_SRC_ABSTAIN;
        if (h->refusal[0] == '\0')
            copy_text(h->refusal, sizeof h->refusal, "capsule_abstain");
        copy_text(h->spoken, sizeof h->spoken, h->refusal);
    }
}

static void hemi_from_serve(const CnetServeResult *sr, const char *tag,
                            CnetHemiResult *h) {
    memset(h, 0, sizeof *h);
    h->via_core = 1;
    h->intent = CNET_CORE_INTENT_LOGIC;
    h->residual_calls = 0;
    h->teacher_calls = 0;
    h->open_chat = 0;
    copy_text(h->skill, sizeof h->skill, tag ? tag : (sr ? sr->brick : ""));
    if (sr && sr->proved && sr->claimed_cert) {
        char nb[8];
        snprintf(nb, sizeof nb, "%u", sr->out_nibble);
        h->bound = 1;
        h->claimed_cert = 1;
        h->hemi = CNET_HEMI_CORE;
        h->plane = CNET_CORE_PLANE_CERT;
        h->source = CNET_HEMI_SRC_SKILL_EXACT;
        h->may_voice = 1;
        copy_text(h->value, sizeof h->value, nb);
        copy_text(h->spoken, sizeof h->spoken,
                  sr->spoken[0] ? sr->spoken : nb);
    } else {
        h->source = CNET_HEMI_SRC_ABSTAIN;
        copy_text(h->refusal, sizeof h->refusal,
                  (sr && sr->refusal[0]) ? sr->refusal : "outside_table_abstain");
        copy_text(h->spoken, sizeof h->spoken, h->refusal);
    }
}

static int push_step(CnetRlmResult *out, const CnetHemiResult *h,
                     const char *note) {
    CnetRlmStep *s;
    if (out == NULL || out->n_steps >= CNET_RLM_MAX_STEPS) return -1;
    s = &out->steps[out->n_steps++];
    memset(s, 0, sizeof *s);
    if (h) s->step = *h;
    copy_text(s->note, sizeof s->note, note ? note : "step");
    return 0;
}

static void set_final(CnetRlmResult *out, const CnetHemiResult *h) {
    if (out == NULL || h == NULL) return;
    out->final = *h;
    out->final.via_core = 1;
    if (h->spoken[0])
        copy_text(out->summary, sizeof out->summary, h->spoken);
    else if (h->value[0])
        copy_text(out->summary, sizeof out->summary, h->value);
    else if (h->refusal[0])
        copy_text(out->summary, sizeof out->summary, h->refusal);
}

/* Autonomy: RLM never evolves. Typed miss harvest feeds unattended evolve.
   Telemetry row stays auto_cert=false; typed rows may complete domains later.
   brick_abstain already appended hop-level typed rows — skip full-turn harvest
   to avoid duplicate spam. */
static void rlm_note_miss(const char *reason, const char *turn) {
    const char *p = getenv("CNET_MISS_LOG");
    FILE *f;
    char qesc[96];
    size_t i, j = 0;
    if (p == NULL || p[0] == '\0') return;
    f = fopen(p, "a");
    if (f == NULL) return;
    qesc[0] = '\0';
    if (turn) {
        for (i = 0; turn[i] && j + 1 < sizeof qesc; ++i) {
            char c = turn[i];
            if (c == '"' || c == '\\' || c == '\n' || c == '\r') c = ' ';
            qesc[j++] = c;
        }
        qesc[j] = '\0';
    }
    fprintf(f,
            "{\"via\":\"cnet_rlm\",\"reason\":\"%s\",\"q\":\"%s\","
            "\"claimed_cert\":0,\"auto_cert\":false,\"learnable\":true}\n",
            reason ? reason : "abstain", qesc);
    fclose(f);
    if (reason && (strcmp(reason, "brick_abstain") == 0 ||
                   strcmp(reason, "brick_bank_miss") == 0))
        return;
    (void)cnet_live_miss_harvest_turn(p, turn);
}

static void budget_fail(CnetRlmResult *out, const char *turn) {
    CnetHemiResult hr;
    if (out == NULL) return;
    memset(&hr, 0, sizeof hr);
    hr.via_core = 1;
    hr.source = CNET_HEMI_SRC_ABSTAIN;
    copy_text(hr.refusal, sizeof hr.refusal, "rlm_budget");
    copy_text(hr.spoken, sizeof hr.spoken, "rlm_budget");
    set_final(out, &hr);
    (void)push_step(out, &hr, "budget");
    rlm_note_miss("rlm_budget", turn);
}

static void blank_word_ci(char *text, const char *word) {
    char *cursor;
    size_t n, i;
    if (text == NULL || word == NULL || word[0] == '\0') return;
    n = strlen(word);
    for (cursor = text; *cursor != '\0'; ++cursor) {
        int left, right, match = 1;
        if (cursor > text && isalnum((unsigned char)cursor[-1])) continue;
        for (i = 0; i < n; ++i) {
            if (cursor[i] == '\0' ||
                tolower((unsigned char)cursor[i]) !=
                    tolower((unsigned char)word[i])) {
                match = 0;
                break;
            }
        }
        if (!match) continue;
        right = !isalnum((unsigned char)cursor[n]);
        left = (cursor == text) || !isalnum((unsigned char)cursor[-1]);
        if (left && right) {
            for (i = 0; i < n; ++i) cursor[i] = ' ';
            cursor += n - 1u;
        }
    }
}

static void consume_named(char *text, const char *canonical) {
    if (text == NULL || canonical == NULL) return;
    blank_word_ci(text, canonical);
    if (strcmp(canonical, "increment_mod256") == 0)
        blank_word_ci(text, "increment");
    else if (strcmp(canonical, "crc8_atm") == 0)
        blank_word_ci(text, "crc8");
    else if (strcmp(canonical, CNET_LOOKUP_CONTRACT) == 0)
        blank_word_ci(text, "lookup");
}

typedef struct {
    char tag[32];
    unsigned n;
    int auto_n;
} RlmBrickHop;

static int brick_in_bank(const CnetServeBank *b, const char *tag) {
    int i;
    if (b == NULL || tag == NULL || tag[0] == '\0') return 0;
    for (i = 0; i < b->n; ++i)
        if (b->bricks[i].live && strcmp(b->bricks[i].tag, tag) == 0) return 1;
    return 0;
}

static int parse_brick_hops(const char *turn, RlmBrickHop *hops, int maxn) {
    const char *p = turn;
    int n = 0;
    if (turn == NULL || hops == NULL || maxn <= 0) return 0;
    while (n < maxn) {
        RlmBrickHop *h = &hops[n];
        size_t ti = 0;
        unsigned v = 0;
        int saw = 0;
        memset(h, 0, sizeof *h);
        while (*p == ' ' || *p == '\t' || *p == '|') p++;
        if (*p == '\0') break;
        if (!(isalpha((unsigned char)*p) || *p == '_')) return 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ':' && *p != '|' &&
               ti + 1 < sizeof h->tag)
            h->tag[ti++] = *p++;
        h->tag[ti] = '\0';
        if (h->tag[0] == '\0') return 0;
        if (*p == ':') {
            p++;
            if ((p[0] == 'a' || p[0] == 'A') && (p[1] == 'u' || p[1] == 'U') &&
                (p[2] == 't' || p[2] == 'T') && (p[3] == 'o' || p[3] == 'O') &&
                (p[4] == '\0' || !isalnum((unsigned char)p[4]))) {
                h->auto_n = 1;
                p += 4;
            } else if (isdigit((unsigned char)*p)) {
                while (isdigit((unsigned char)*p)) {
                    saw = 1;
                    v = v * 10u + (unsigned)(*p - '0');
                    if (v > 15u) return 0;
                    p++;
                }
                h->n = v;
            } else
                return 0;
        } else {
            while (*p == ' ' || *p == '\t') p++;
            if (isdigit((unsigned char)*p)) {
                while (isdigit((unsigned char)*p)) {
                    saw = 1;
                    v = v * 10u + (unsigned)(*p - '0');
                    if (v > 15u) return 0;
                    p++;
                }
                h->n = v;
            } else {
                h->auto_n = 1;
            }
        }
        (void)saw;
        n++;
        while (*p == ' ' || *p == '\t') p++;
        if ((p[0] == 't' || p[0] == 'T') && (p[1] == 'h' || p[1] == 'H') &&
            (p[2] == 'e' || p[2] == 'E') && (p[3] == 'n' || p[3] == 'N') &&
            (p[4] == '\0' || isspace((unsigned char)p[4]))) {
            p += 4;
            continue;
        }
        if (*p == '|') continue;
        if (*p == '\0') break;
        /* leftover prose — not a brick chain */
        if (n == 1 && !cnet_rlm_is_chain_turn(turn)) break;
        if (*p) {
            /* another hop must start with a tag or we stop */
            if (!(isalpha((unsigned char)*p) || *p == '_')) break;
        }
    }
    return n;
}

static int try_brick_hops(const char *turn, int *steps_left, CnetRlmResult *out) {
    CnetServeBank *bank;
    RlmBrickHop hops[CNET_RLM_MAX_STEPS];
    int n_hops, i;
    unsigned prior = 0;
    int have_prior = 0;
    CnetHemiResult last;
    memset(&last, 0, sizeof last);

    n_hops = parse_brick_hops(turn, hops, CNET_RLM_MAX_STEPS);
    /* Multi-hop brick-shaped turns must not fall through to soft core/OPEN_CHAT
       when the bank is empty or the first tag is missing — that is a CERT miss,
       not a creative fill. */
    if (n_hops >= 2 || (n_hops == 1 && cnet_rlm_is_chain_turn(turn))) {
        bank = cnet_serve_global();
        if (bank == NULL || bank->n <= 0 || !brick_in_bank(bank, hops[0].tag) ||
            hops[0].auto_n) {
            CnetHemiResult hr;
            memset(&hr, 0, sizeof hr);
            hr.via_core = 1;
            hr.source = CNET_HEMI_SRC_ABSTAIN;
            hr.intent = out ? out->intent : CNET_CORE_INTENT_LOGIC;
            copy_text(hr.skill, sizeof hr.skill, hops[0].tag);
            copy_text(hr.refusal, sizeof hr.refusal, "outside_table_abstain");
            copy_text(hr.spoken, sizeof hr.spoken, "outside_table_abstain");
            if (out) {
                push_step(out, &hr, "brick");
                set_final(out, &hr);
            }
            {
                const char *mp = getenv("CNET_MISS_LOG");
                if (mp && mp[0]) {
                    int hi;
                    for (hi = 0; hi < n_hops; ++hi) {
                        if (hops[hi].auto_n) continue;
                        (void)cnet_live_miss_append(mp, hops[hi].tag,
                                                    hops[hi].n & 15u, 0, 0);
                    }
                }
            }
            rlm_note_miss("brick_bank_miss", turn);
            return 1;
        }
    } else {
        bank = cnet_serve_global();
        if (bank == NULL || bank->n <= 0) return -2;
        if (n_hops < 1) return -2;
        if (!brick_in_bank(bank, hops[0].tag)) return -2;
        if (hops[0].auto_n) return -2;
    }

    for (i = 0; i < n_hops; ++i) {
        CnetServeResult sr;
        CnetHemiResult hr;
        char piece[48];
        unsigned nib;
        if (*steps_left <= 0) {
            budget_fail(out, turn);
            return 1;
        }
        nib = hops[i].auto_n ? (have_prior ? prior : hops[i].n) : hops[i].n;
        snprintf(piece, sizeof piece, "%.31s %u", hops[i].tag, nib & 15u);
        memset(&sr, 0, sizeof sr);
        if (cnet_serve_result(bank, piece, &sr) != 0 || !sr.proved) {
            hemi_from_serve(&sr, hops[i].tag, &hr);
            hr.intent = out->intent;
            push_step(out, &hr, "brick");
            set_final(out, &hr);
            {
                const char *mp = getenv("CNET_MISS_LOG");
                if (mp && mp[0])
                    (void)cnet_live_miss_append(mp, hops[i].tag, nib & 15u, 0, 0);
            }
            rlm_note_miss("brick_abstain", turn);
            return 1;
        }
        hemi_from_serve(&sr, hops[i].tag, &hr);
        hr.intent = out->intent;
        push_step(out, &hr, "brick");
        {
            const char *mp = getenv("CNET_MISS_LOG");
            if (mp && mp[0])
                (void)cnet_live_miss_append(mp, hops[i].tag, nib & 15u, 1,
                                            sr.out_nibble & 15u);
        }
        last = hr;
        prior = sr.out_nibble;
        have_prior = 1;
        (*steps_left)--;
    }
    set_final(out, &last);
    return 0;
}

int cnet_rlm_ask(const char *turn, const CnetRlmPolicy *policy,
                 CnetRlmResult *out) {
    return cnet_rlm_ask_session(turn, policy, NULL, out);
}

int cnet_rlm_ask_session(const char *turn, const CnetRlmPolicy *policy,
                         CnetRlmSession *session, CnetRlmResult *out) {
    CnetRlmPolicy pol;
    CnetHemiResult hr;
    int steps_left;
    int n_subj;
    int brc;
    int src;

    if (out == NULL) return -1;
    clear_rlm(out);
    out->via_rlm = 1;
    if (policy)
        pol = *policy;
    else
        cnet_rlm_policy_default(&pol);
    if (pol.max_steps < 1) pol.max_steps = 1;
    if (pol.max_steps > CNET_RLM_MAX_STEPS) pol.max_steps = CNET_RLM_MAX_STEPS;

    if (turn == NULL || turn[0] == '\0') {
        out->intent = CNET_CORE_INTENT_UNKNOWN;
        out->final.via_core = 1;
        out->final.source = CNET_HEMI_SRC_ABSTAIN;
        copy_text(out->final.refusal, sizeof out->final.refusal, "empty_turn");
        copy_text(out->summary, sizeof out->summary, "empty_turn");
        push_step(out, &out->final, "empty");
        return 1;
    }

    out->intent = cnet_core_discern(turn);
    steps_left = pol.max_steps;

    /* Multi-turn CERT recall (session memory only — never residual). */
    src = try_session_recall(turn, session, out);
    if (src == 0) {
        if (session)
            copy_text(session->last_turn, sizeof session->last_turn, turn);
        return 0;
    }

    /* CERT brick hops (scenario world) hosted by RLM. First tag must already
       sit in the serve bank — RLM does not admit or evolve. */
    brc = try_brick_hops(turn, &steps_left, out);
    if (brc == 0) {
        if (session && out->final.claimed_cert)
            cnet_rlm_session_remember(session, out->final.skill, out->final.value);
        if (session)
            copy_text(session->last_turn, sizeof session->last_turn, turn);
        return 0;
    }
    if (brc == 1) return 1;

    /* Multi-skill CERT path: bill one RLM step per capsule call. */
    n_subj = cnet_capsule_loop_count_subjects(turn);
    if (pol.use_capsule_loop && n_subj >= 2 && steps_left > 0) {
        CnetCapsuleLoopResult loop;
        unsigned i;
        int leftover;
        memset(&loop, 0, sizeof loop);
        if (cnet_capsule_loop_cd_ask(turn, NULL, &loop) == 0) {
            leftover = n_subj;
            for (i = 0; i < loop.n_calls; ++i) {
                if (steps_left <= 0) break;
                hemi_from_call(&loop.calls[i], &hr);
                hr.intent = out->intent;
                push_step(out, &hr, "capsule");
                steps_left--;
                if (loop.calls[i].kind == CNET_CAPSULE_CALL_EXACT &&
                    loop.calls[i].claimed_cert)
                    leftover--;
            }
            if (leftover > 0) {
                char remaining[CNET_RLM_TEXT];
                copy_text(remaining, sizeof remaining, turn);
                for (i = 0; i < loop.n_calls; ++i)
                    if (loop.calls[i].kind == CNET_CAPSULE_CALL_EXACT)
                        consume_named(remaining, loop.calls[i].name);
                while (leftover > 0 && steps_left > 0) {
                    memset(&hr, 0, sizeof hr);
                    if (cnet_hemi_ask_core(remaining, &pol.core, &hr) < 0)
                        return -1;
                    hr.intent = out->intent;
                    hr.residual_calls = 0;
                    hr.open_chat = 0;
                    if (hr.plane == CNET_CORE_PLANE_OPEN_CHAT) {
                        /* leftover hop open-chat: keep as draft, not CERT */
                        hr.claimed_cert = 0;
                        hr.open_chat = 1;
                        hr.bound = 1;
                        if (!hr.spoken[0] && hr.value[0])
                            copy_text(hr.spoken, sizeof hr.spoken, hr.value);
                    }
                    push_step(out, &hr, "core");
                    steps_left--;
                    if (hr.bound && hr.plane == CNET_CORE_PLANE_CERT) {
                        leftover--;
                        consume_named(remaining, hr.skill);
                    } else {
                        set_final(out, &hr);
                        rlm_note_miss("core_leftover_abstain", turn);
                        goto maybe_ember;
                    }
                }
            }
            if (leftover > 0) {
                budget_fail(out, turn);
                return 1;
            }
            if (out->n_steps > 0) {
                const CnetHemiResult *last = &out->steps[out->n_steps - 1].step;
                if (last->bound && last->plane == CNET_CORE_PLANE_CERT) {
                    set_final(out, last);
                    if (session)
                        cnet_rlm_session_remember(session, last->skill,
                                                  last->value);
                    return 0;
                }
            }
            hemi_from_capsule(&loop, &hr);
            hr.intent = out->intent;
            if (hr.bound && hr.plane == CNET_CORE_PLANE_CERT) {
                set_final(out, &hr);
                if (session)
                    cnet_rlm_session_remember(session, hr.skill, hr.value);
                return 0;
            }
            /* capsule abstain → fall through to single core_ask once */
        }
    }

    /* Recursive CORE re-entry budget (usually 1 decisive core_ask). */
    while (steps_left > 0) {
        memset(&hr, 0, sizeof hr);
        if (cnet_core_ask(turn, &pol.core, &hr) < 0) return -1;
        hr.intent = out->intent;
        push_step(out, &hr, "core");
        steps_left--;

        if (hr.bound && hr.plane == CNET_CORE_PLANE_CERT) {
            set_final(out, &hr);
            if (session)
                cnet_rlm_session_remember(session, hr.skill, hr.value);
            if (session)
                copy_text(session->last_turn, sizeof session->last_turn, turn);
            return 0;
        }
        /* OPEN_CHAT is CORE creativity plane — speak it, never claim CERT. */
        if (hr.bound && hr.plane == CNET_CORE_PLANE_OPEN_CHAT) {
            hr.claimed_cert = 0;
            hr.open_chat = 1;
            if (!hr.spoken[0] && hr.value[0])
                copy_text(hr.spoken, sizeof hr.spoken, hr.value);
            out->steps[out->n_steps - 1].step = hr;
            set_final(out, &hr);
            rlm_note_miss("core_open_chat", turn);
            return 0;
        }

        /* Abstain: no further magic recursion into LLM for pure logic */
        if (out->intent == CNET_CORE_INTENT_LOGIC &&
            !pol.core.logic_open_chat_fallback) {
            set_final(out, &hr);
            goto maybe_ember;
        }

        set_final(out, &hr);
        goto maybe_ember;
    }

    budget_fail(out, turn);
    return 1;

maybe_ember:
    /* Optional Ember residual draft — never CERT. Opt-in only. */
    if (pol.allow_ember_draft && out->intent != CNET_CORE_INTENT_LOGIC) {
        int er = try_ember_draft(turn, out);
        if (er == 0 || er == 1) return er;
    }
    if (!out->final.refusal[0] && !out->final.bound)
        rlm_note_miss("rlm_abstain", turn);
    return 1;
}
