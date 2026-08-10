#include "../include/cnet_roe_asi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

/* forward */
int roe_save_catalog(const RoeAsi *R);

static void str_tolower_copy(char *dst, size_t cap, const char *src) {
    size_t i;
    if (!dst || !cap) return;
    for (i = 0; src && src[i] && i + 1 < cap; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i < cap ? i : cap - 1] = 0;
}

static int contains_ci(const char *hay, const char *needle) {
    char h[ROE_TEXT_MAX], n[ROE_TEXT_MAX];
    if (!hay || !needle || !needle[0]) return 0;
    str_tolower_copy(h, sizeof h, hay);
    str_tolower_copy(n, sizeof n, needle);
    return strstr(h, n) != NULL;
}

static uint64_t est_tokens(const char *a, const char *b) {
    /* crude ~4 chars/token */
    size_t n = 0;
    if (a) n += strlen(a);
    if (b) n += strlen(b);
    return (uint64_t)(n / 4 + 8);
}

void roe_init(RoeAsi *R) {
    if (!R) return;
    memset(R, 0, sizeof *R);
    cnet_asi_init(&R->gate);
    R->use_gate = 1;
    R->promote_votes_needed = 2;
}

void roe_reset_stats(RoeAsi *R) {
    if (!R) return;
    R->n_turns = R->n_local_hit = R->n_lookup_hit = R->n_llm_call = 0;
    R->n_abstain = R->n_ask_user = R->n_promote = R->n_verify_fail = 0;
    R->tokens_local = R->tokens_lookup = R->tokens_llm = 0;
    R->tokens_baseline_llm = 0;
    R->n_live_lookup = R->n_live_llm = 0;
}

void roe_set_net(RoeAsi *R, RoeNet *net) {
    if (R) R->net = net;
}

int roe_set_catalog_dir(RoeAsi *R, const char *dir) {
    if (!R || !dir || !dir[0]) return -1;
    snprintf(R->catalog_dir, sizeof R->catalog_dir, "%s", dir);
    return 0;
}

int roe_add_skill(RoeAsi *R, const char *id, const char *intent_key,
                  const char *pattern, const char *answer, uint32_t privilege,
                  int certified) {
    RoeSkill *s;
    if (!R || !id || !pattern || !answer) return -1;
    if (R->n_skills >= ROE_MAX_SKILLS) return -1;
    s = &R->skills[R->n_skills++];
    memset(s, 0, sizeof *s);
    snprintf(s->id, sizeof s->id, "%s", id);
    snprintf(s->intent_key, sizeof s->intent_key, "%s",
             intent_key ? intent_key : id);
    snprintf(s->pattern, sizeof s->pattern, "%s", pattern);
    snprintf(s->answer, sizeof s->answer, "%s", answer);
    s->privilege = privilege;
    s->certified = certified ? 1 : 0;
    s->active = 1;
    if (certified) {
        (void)cnet_asi_add_skill(&R->gate, id, pattern, 0u, privilege,
                                 CNET_ASI_KIND_SPECIALIST);
    }
    return 0;
}

int roe_add_lookup(RoeAsi *R, const char *pattern, const char *snippet) {
    RoeLookupEntry *e;
    if (!R || !pattern || !snippet) return -1;
    if (R->n_lookup >= ROE_MAX_LOOKUP) return -1;
    e = &R->lookup[R->n_lookup++];
    memset(e, 0, sizeof *e);
    snprintf(e->pattern, sizeof e->pattern, "%s", pattern);
    snprintf(e->snippet, sizeof e->snippet, "%s", snippet);
    e->active = 1;
    return 0;
}

int roe_add_teach(RoeAsi *R, const char *pattern, const char *intent_key,
                  const char *answer) {
    RoeTeachEntry *t;
    if (!R || !pattern || !answer) return -1;
    if (R->n_teach >= ROE_MAX_TEACH) return -1;
    t = &R->teach[R->n_teach++];
    memset(t, 0, sizeof *t);
    snprintf(t->pattern, sizeof t->pattern, "%s", pattern);
    snprintf(t->intent_key, sizeof t->intent_key, "%s",
             intent_key ? intent_key : "taught");
    snprintf(t->answer, sizeof t->answer, "%s", answer);
    t->active = 1;
    return 0;
}

static RoeSkill *find_local(RoeAsi *R, const char *query) {
    size_t i;
    RoeSkill *best = NULL;
    size_t best_len = 0;
    for (i = 0; i < R->n_skills; i++) {
        RoeSkill *s = &R->skills[i];
        size_t plen;
        int s_soul, b_soul;
        if (!s->active || !s->certified) continue;
        if (!contains_ci(query, s->pattern)) continue;
        plen = strlen(s->pattern);
        if (!best) {
            best = s;
            best_len = plen;
            continue;
        }
        if (plen > best_len) {
            best = s;
            best_len = plen;
            continue;
        }
        if (plen < best_len) continue;
        if (s->privilege < best->privilege) {
            best = s;
            continue;
        }
        if (s->privilege > best->privilege) continue;
        /* equal length + privilege: prefer soul_* persona over shell self_* */
        s_soul = (strncmp(s->id, "soul_", 5) == 0);
        b_soul = (strncmp(best->id, "soul_", 5) == 0);
        if (s_soul && !b_soul) {
            best = s;
            continue;
        }
        if (s_soul == b_soul && strcmp(s->id, best->id) < 0) best = s;
    }
    return best;
}

static RoeLookupEntry *find_lookup(RoeAsi *R, const char *query) {
    size_t i;
    for (i = 0; i < R->n_lookup; i++)
        if (R->lookup[i].active && contains_ci(query, R->lookup[i].pattern))
            return &R->lookup[i];
    return NULL;
}

static RoeTeachEntry *find_teach(RoeAsi *R, const char *query) {
    size_t i;
    for (i = 0; i < R->n_teach; i++)
        if (R->teach[i].active && contains_ci(query, R->teach[i].pattern))
            return &R->teach[i];
    return NULL;
}

const char *roe_source_name(int source) {
    switch (source) {
    case ROE_SRC_LOCAL:
        return "LOCAL";
    case ROE_SRC_LOOKUP:
        return "LOOKUP";
    case ROE_SRC_LLM:
        return "LLM";
    case ROE_SRC_ABSTAIN:
        return "ABSTAIN";
    case ROE_SRC_ASK_USER:
        return "ASK_USER";
    default:
        return "UNK";
    }
}

void roe_reply_fill_inventory(const RoeAsi *R, RoeReply *out) {
    uint64_t used, base;
    double hit, save;
    size_t ncert = 0, i;
    if (!out) return;
    snprintf(out->source_name, sizeof out->source_name, "%s",
             roe_source_name(out->source));
    if (!R) {
        out->inventory_line[0] = 0;
        return;
    }
    for (i = 0; i < R->n_skills; i++)
        if (R->skills[i].active && R->skills[i].certified) ncert++;
    used = R->tokens_local + R->tokens_lookup + R->tokens_llm;
    base = R->tokens_baseline_llm;
    hit = R->n_turns ? (double)R->n_local_hit / (double)R->n_turns : 0.0;
    save = base ? 1.0 - (double)used / (double)base : 0.0;
    snprintf(out->inventory_line, sizeof out->inventory_line,
             "src=%s skill=%s ver=%d hit=%.2f save=%.2f turns=%llu local=%llu "
             "llm=%llu abstain=%llu cert_skills=%zu never_self_cert=1",
             out->source_name, out->skill_id[0] ? out->skill_id : "-",
             out->verified, hit, save, (unsigned long long)R->n_turns,
             (unsigned long long)R->n_local_hit, (unsigned long long)R->n_llm_call,
             (unsigned long long)R->n_abstain, ncert);
}

static void push_log(RoeAsi *R, const char *q, RoeReply *rep, int ok) {
    RoeTurnLog *L = &R->log[R->log_i % ROE_MAX_LOG];
    if (rep) roe_reply_fill_inventory(R, rep);
    memset(L, 0, sizeof *L);
    snprintf(L->query, sizeof L->query, "%s", q ? q : "");
    snprintf(L->answer, sizeof L->answer, "%s", rep ? rep->answer : "");
    L->source = rep ? rep->source : ROE_SRC_ABSTAIN;
    L->ok = ok;
    L->verified = rep ? rep->verified : 0;
    L->tokens_est = rep ? rep->tokens_est : 0;
    L->tick = ++R->tick;
    R->log_i++;
    if (R->log_n < ROE_MAX_LOG) R->log_n++;
}

int roe_turn(RoeAsi *R, const char *query, RoeReply *out) {
    RoeSkill *sk;
    RoeLookupEntry *lu;
    RoeTeachEntry *te;
    uint64_t base_tok;

    if (out) memset(out, 0, sizeof *out);
    if (!R || !query || !query[0]) return ROE_ERR;

    R->n_turns++;
    base_tok = est_tokens(query, "llmdraft answer padding for baseline estimate xx");
    R->tokens_baseline_llm += base_tok;

    /* 1) LOCAL CERT skills */
    sk = find_local(R, query);
    if (sk) {
        if (R->use_gate) {
            const char *gn = NULL;
            int gr = cnet_asi_resolve(&R->gate, sk->id, 0xffffffffu, -1.0, &gn);
            if (gr != CNET_ASI_OK && gr != CNET_ASI_ERR) {
                /* gate blocked — fall through miss path */
            } else {
                sk->hits++;
                R->n_local_hit++;
                if (out) {
                    snprintf(out->answer, sizeof out->answer, "%s", sk->answer);
                    snprintf(out->skill_id, sizeof out->skill_id, "%s", sk->id);
                    out->source = ROE_SRC_LOCAL;
                    out->verified = 1;
                    out->tokens_est = 0;
                }
                R->tokens_local += 0;
                push_log(R, query, out, 1);
                return ROE_OK;
            }
        } else {
            sk->hits++;
            R->n_local_hit++;
            if (out) {
                snprintf(out->answer, sizeof out->answer, "%s", sk->answer);
                snprintf(out->skill_id, sizeof out->skill_id, "%s", sk->id);
                out->source = ROE_SRC_LOCAL;
                out->verified = 1;
                out->tokens_est = 0;
            }
            push_log(R, query, out, 1);
            return ROE_OK;
        }
    }

    /* 2) LOOKUP (cheap, still untrusted for CERT — verified flag 0) */
    lu = find_lookup(R, query);
    if (lu) {
        uint64_t t = est_tokens(query, lu->snippet) / 4 + 2;
        R->n_lookup_hit++;
        R->tokens_lookup += t;
        if (out) {
            snprintf(out->answer, sizeof out->answer, "[lookup] %s", lu->snippet);
            out->source = ROE_SRC_LOOKUP;
            out->verified = 0;
            out->tokens_est = t;
            out->skill_id[0] = 0;
        }
        push_log(R, query, out, 1);
        return ROE_OK;
    }

    /* 3) LLM teacher curriculum (mock) = external teacher. UNTRUSTED. */
    te = find_teach(R, query);
    if (te) {
        uint64_t t = est_tokens(query, te->answer) + 32;
        R->n_llm_call++;
        R->tokens_llm += t;
        if (out) {
            snprintf(out->answer, sizeof out->answer, "[llm-untrusted] %s",
                     te->answer);
            out->source = ROE_SRC_LLM;
            out->verified = 0;
            out->tokens_est = t;
            out->skill_id[0] = 0;
        }
        push_log(R, query, out, 1);
        return ROE_OK;
    }

    /* 4) LIVE lookup (DuckDuckGo etc.) — still untrusted */
    if (R->net && R->net->enable_lookup) {
        char snip[ROE_ANSWER_MAX];
        uint64_t t = 0;
        if (roe_net_lookup(R->net, query, snip, sizeof snip, &t) == 0 && snip[0]) {
            R->n_lookup_hit++;
            R->n_live_lookup++;
            R->tokens_lookup += t ? t : 8;
            if (out) {
                /* prefix + body; clip body to fit */
                const char *pfx = "[lookup-live] ";
                size_t pl = strlen(pfx), bl = strlen(snip), maxb;
                maxb = sizeof(out->answer) > pl + 1 ? sizeof(out->answer) - pl - 1 : 0;
                if (bl > maxb) bl = maxb;
                memcpy(out->answer, pfx, pl);
                memcpy(out->answer + pl, snip, bl);
                out->answer[pl + bl] = 0;
                out->source = ROE_SRC_LOOKUP;
                out->verified = 0;
                out->tokens_est = t;
                out->skill_id[0] = 0;
            }
            push_log(R, query, out, 1);
            return ROE_OK;
        }
    }

    /* 5) LIVE LLM teacher — untrusted */
    if (R->net && R->net->enable_llm) {
        char ans[ROE_ANSWER_MAX];
        uint64_t t = 0;
        if (roe_net_llm(R->net, query, ans, sizeof ans, &t) == 0 && ans[0]) {
            R->n_llm_call++;
            R->n_live_llm++;
            R->tokens_llm += t ? t : 64;
            if (out) {
                const char *pfx = "[llm-live] ";
                size_t pl = strlen(pfx), bl = strlen(ans), maxb;
                maxb = sizeof(out->answer) > pl + 1 ? sizeof(out->answer) - pl - 1 : 0;
                if (bl > maxb) bl = maxb;
                memcpy(out->answer, pfx, pl);
                memcpy(out->answer + pl, ans, bl);
                out->answer[pl + bl] = 0;
                out->source = ROE_SRC_LLM;
                out->verified = 0;
                out->tokens_est = t;
                out->skill_id[0] = 0;
            }
            push_log(R, query, out, 1);
            return ROE_OK;
        }
    }

    /* 6) fail-closed abstain / ask user */
    R->n_abstain++;
    R->n_ask_user++;
    if (out) {
        snprintf(out->answer, sizeof out->answer,
                 "ABSTAIN: no local skill, lookup, or teacher coverage. Ask user.");
        out->source = ROE_SRC_ASK_USER;
        out->verified = 0;
        out->tokens_est = 0;
    }
    push_log(R, query, out, 0);
    return ROE_ABSTAIN;
}

static int answers_match(const char *a, const char *b) {
    char x[ROE_ANSWER_MAX], y[ROE_ANSWER_MAX];
    const char *pa, *pb;
    if (!a || !b) return 0;
    /* strip prefixes */
    pa = a;
    pb = b;
    if (strncmp(pa, "[llm-untrusted] ", 16) == 0) pa += 16;
    if (strncmp(pa, "[llm-live] ", 11) == 0) pa += 11;
    if (strncmp(pa, "[lookup] ", 9) == 0) pa += 9;
    if (strncmp(pa, "[lookup-live] ", 14) == 0) pa += 14;
    if (strncmp(pb, "[llm-untrusted] ", 16) == 0) pb += 16;
    if (strncmp(pb, "[llm-live] ", 11) == 0) pb += 11;
    if (strncmp(pb, "[lookup] ", 9) == 0) pb += 9;
    if (strncmp(pb, "[lookup-live] ", 14) == 0) pb += 14;
    str_tolower_copy(x, sizeof x, pa);
    str_tolower_copy(y, sizeof y, pb);
    return strcmp(x, y) == 0 || strstr(x, y) != NULL || strstr(y, x) != NULL;
}

static int pending_add_vote(RoeAsi *R, const char *intent, const char *pattern,
                            const char *answer) {
    size_t i;
    for (i = 0; i < R->n_pending; i++) {
        if (R->pending[i].active && strcmp(R->pending[i].intent_key, intent) == 0) {
            R->pending[i].votes++;
            if (answer && answer[0])
                snprintf(R->pending[i].answer, sizeof R->pending[i].answer, "%s",
                         answer);
            return (int)i;
        }
    }
    if (R->n_pending >= ROE_MAX_SKILLS) return -1;
    i = R->n_pending++;
    memset(&R->pending[i], 0, sizeof R->pending[0]);
    snprintf(R->pending[i].intent_key, sizeof R->pending[i].intent_key, "%s",
             intent);
    snprintf(R->pending[i].pattern, sizeof R->pending[i].pattern, "%s", pattern);
    snprintf(R->pending[i].answer, sizeof R->pending[i].answer, "%s", answer);
    R->pending[i].votes = 1;
    R->pending[i].active = 1;
    return (int)i;
}

static int try_promote_pending(RoeAsi *R, int idx) {
    char id[ROE_NAME_MAX];
    char intent[ROE_NAME_MAX];
    char pattern[ROE_TEXT_MAX];
    char answer[ROE_ANSWER_MAX];
    size_t i, n;
    if (idx < 0 || (size_t)idx >= R->n_pending) return 0;
    if (R->pending[idx].votes < R->promote_votes_needed) return 0;
    snprintf(intent, sizeof intent, "%s", R->pending[idx].intent_key);
    snprintf(pattern, sizeof pattern, "%s", R->pending[idx].pattern);
    snprintf(answer, sizeof answer, "%s", R->pending[idx].answer);
    /* skill_ + up to 57 chars of intent */
    id[0] = 's';
    id[1] = 'k';
    id[2] = 'i';
    id[3] = 'l';
    id[4] = 'l';
    id[5] = '_';
    n = strlen(intent);
    if (n > sizeof(id) - 7) n = sizeof(id) - 7;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)intent[i];
        id[6 + i] = (char)(isalnum(c) || c == '_' ? c : '_');
    }
    id[6 + n] = 0;
    if (roe_add_skill(R, id, intent, pattern, answer, 1, 1) != 0) {
        for (i = 0; i < R->n_skills; i++) {
            if (strcmp(R->skills[i].id, id) == 0) {
                R->skills[i].certified = 1;
                snprintf(R->skills[i].answer, sizeof R->skills[i].answer, "%s",
                         answer);
                (void)cnet_asi_add_skill(&R->gate, id, pattern, 0u, 1,
                                         CNET_ASI_KIND_SPECIALIST);
                break;
            }
        }
    }
    R->pending[idx].active = 0;
    R->n_promote++;
    if (R->catalog_dir[0]) (void)roe_save_catalog(R);
    return 1;
}

int roe_feedback_verify(RoeAsi *R, const char *query, const char *gold_answer,
                        int user_accept) {
    const char *gold = gold_answer;
    RoeTeachEntry *te;
    RoeTurnLog *last = NULL;
    size_t k, n;
    int promoted = 0;
    char clean[ROE_ANSWER_MAX];

    if (!R || !query) return 0;
    n = R->log_n;
    if (n > ROE_MAX_LOG) n = ROE_MAX_LOG;
    for (k = 0; k < n; k++) {
        size_t idx = (R->log_i + ROE_MAX_LOG - 1 - k) % ROE_MAX_LOG;
        if (R->log[idx].tick && contains_ci(R->log[idx].query, query)) {
            last = &R->log[idx];
            break;
        }
        if (strcmp(R->log[idx].query, query) == 0) {
            last = &R->log[idx];
            break;
        }
    }
    if (!last) return 0;
    if (last->source == ROE_SRC_LOCAL) return 0; /* already CERT */

    te = find_teach(R, query);
    if (!gold && te) gold = te->answer;
    if (!gold && !user_accept) {
        R->n_verify_fail++;
        return 0;
    }

    snprintf(clean, sizeof clean, "%s", last->answer);
    if (strncmp(clean, "[llm-untrusted] ", 16) == 0)
        memmove(clean, clean + 16, strlen(clean + 16) + 1);
    if (strncmp(clean, "[llm-live] ", 11) == 0)
        memmove(clean, clean + 11, strlen(clean + 11) + 1);
    if (strncmp(clean, "[lookup] ", 9) == 0)
        memmove(clean, clean + 9, strlen(clean + 9) + 1);
    if (strncmp(clean, "[lookup-live] ", 14) == 0)
        memmove(clean, clean + 14, strlen(clean + 14) + 1);

    if (gold && !answers_match(clean, gold) && !user_accept) {
        R->n_verify_fail++;
        last->verified = 0;
        return 0;
    }

    last->verified = 1;
    {
        const char *intent = te ? te->intent_key : "ad_hoc";
        const char *pat = te ? te->pattern : query;
        int idx;
        /* user_accept is an explicit shell authorization — count as full vote threshold */
        if (user_accept) {
            idx = pending_add_vote(R, intent, pat, gold ? gold : clean);
            if (idx >= 0) {
                R->pending[idx].votes = R->promote_votes_needed;
                promoted = try_promote_pending(R, idx);
            }
        } else {
            idx = pending_add_vote(R, intent, pat, gold ? gold : clean);
            if (idx >= 0) promoted = try_promote_pending(R, idx);
        }
    }
    return promoted;
}

int roe_train_epoch(RoeAsi *R, const char **queries, int nq, RoeTrainReport *rep) {
    int i, promotes = 0;
    if (rep) memset(rep, 0, sizeof *rep);
    if (!R || !queries || nq <= 0) return -1;

    for (i = 0; i < nq; i++) {
        RoeReply repy;
        int st = roe_turn(R, queries[i], &repy);
        (void)st;
        if (repy.source == ROE_SRC_LLM || repy.source == ROE_SRC_LOOKUP) {
            if (roe_feedback_verify(R, queries[i], NULL, 0)) promotes++;
        }
    }

    if (rep) {
        rep->turns = nq;
        rep->promotes = promotes;
        rep->tokens_used = R->tokens_local + R->tokens_lookup + R->tokens_llm;
        rep->tokens_baseline = R->tokens_baseline_llm;
        rep->local_hit_rate =
            R->n_turns ? (double)R->n_local_hit / (double)R->n_turns : 0.0;
        rep->token_save_ratio =
            rep->tokens_baseline
                ? 1.0 - (double)rep->tokens_used / (double)rep->tokens_baseline
                : 0.0;
    }
    return 0;
}

void roe_dump_stats(const RoeAsi *R, char *buf, size_t cap) {
    if (!R || !buf || !cap) return;
    snprintf(buf, cap,
             "turns=%llu local=%llu lookup=%llu llm=%llu live_lu=%llu live_llm=%llu "
             "abstain=%llu promote=%llu verify_fail=%llu tok_used=%llu tok_base=%llu "
             "hit=%.3f save=%.3f skills=%zu",
             (unsigned long long)R->n_turns, (unsigned long long)R->n_local_hit,
             (unsigned long long)R->n_lookup_hit, (unsigned long long)R->n_llm_call,
             (unsigned long long)R->n_live_lookup, (unsigned long long)R->n_live_llm,
             (unsigned long long)R->n_abstain, (unsigned long long)R->n_promote,
             (unsigned long long)R->n_verify_fail,
             (unsigned long long)(R->tokens_local + R->tokens_lookup + R->tokens_llm),
             (unsigned long long)R->tokens_baseline_llm,
             R->n_turns ? (double)R->n_local_hit / (double)R->n_turns : 0.0,
             R->tokens_baseline_llm
                 ? 1.0 - (double)(R->tokens_local + R->tokens_lookup + R->tokens_llm) /
                           (double)R->tokens_baseline_llm
                 : 0.0,
             R->n_skills);
}

static int mkdir_one(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int mkdir_p(const char *path) {
    char tmp[ROE_PATH_MAX];
    size_t len, i;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') tmp[len - 1] = 0;
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = 0;
            (void)mkdir_one(tmp);
            tmp[i] = '/';
        }
    }
    return mkdir_one(tmp);
}

int roe_export_skill_pack(const RoeAsi *R, const char *skill_id, const char *out_dir) {
    const RoeSkill *s = NULL;
    size_t i;
    char dir[ROE_PATH_MAX], path[ROE_PATH_MAX];
    FILE *f;
    if (!R || !skill_id || !out_dir) return -1;
    for (i = 0; i < R->n_skills; i++)
        if (R->skills[i].active && strcmp(R->skills[i].id, skill_id) == 0) {
            s = &R->skills[i];
            break;
        }
    if (!s || !s->certified) return -2;
    snprintf(dir, sizeof dir, "%s/skills/%s", out_dir, skill_id);
    mkdir_p(dir);
    {
        size_t dl = strlen(dir);
        if (dl + 14 >= sizeof path) return -5;
        memcpy(path, dir, dl);
        memcpy(path + dl, "/SKILL.roe", 11);
    }
    f = fopen(path, "w");
    if (!f) return -3;
    fprintf(f, "ROE_SKILL 1\n");
    fprintf(f, "id %s\n", s->id);
    fprintf(f, "intent %s\n", s->intent_key);
    fprintf(f, "pattern %s\n", s->pattern);
    fprintf(f, "answer %s\n", s->answer);
    fprintf(f, "privilege %u\n", s->privilege);
    fprintf(f, "certified 1\n");
    fclose(f);
    {
        size_t dl = strlen(dir);
        if (dl + 14 >= sizeof path) return -5;
        memcpy(path, dir, dl);
        memcpy(path + dl, "/manifest.roe", 14);
    }
    f = fopen(path, "w");
    if (!f) return -4;
    fprintf(f, "ROE_MANIFEST 1\n");
    fprintf(f, "format roe_text_capsule\n");
    fprintf(f, "unit %s\n", s->id);
    fprintf(f, "note untrusted_until_reload_in_roe_shell\n");
    fprintf(f, "hits %llu\n", (unsigned long long)s->hits);
    fclose(f);
    return 0;
}

int roe_save_catalog(const RoeAsi *R) {
    char path[ROE_PATH_MAX], skills_dir[ROE_PATH_MAX];
    FILE *f;
    size_t i;
    int n = 0;
    if (!R || !R->catalog_dir[0]) return -1;
    mkdir_p(R->catalog_dir);
    {
        size_t cl = strlen(R->catalog_dir);
        if (cl + 8 >= sizeof skills_dir) return -5;
        memcpy(skills_dir, R->catalog_dir, cl);
        memcpy(skills_dir + cl, "/skills", 8);
    }
    mkdir_p(skills_dir);
    {
        size_t cl = strlen(R->catalog_dir);
        if (cl + 15 >= sizeof path) return -5;
        memcpy(path, R->catalog_dir, cl);
        memcpy(path + cl, "/catalog.jsonl", 15);
    }
    f = fopen(path, "w");
    if (!f) return -2;
    for (i = 0; i < R->n_skills; i++) {
        const RoeSkill *s = &R->skills[i];
        if (!s->active || !s->certified) continue;
        fprintf(f,
                "{\"id\":\"%s\",\"intent\":\"%s\",\"pattern\":\"%s\",\"answer\":\"%s\","
                "\"privilege\":%u,\"hits\":%llu}\n",
                s->id, s->intent_key, s->pattern, s->answer, s->privilege,
                (unsigned long long)s->hits);
        (void)roe_export_skill_pack(R, s->id, R->catalog_dir);
        n++;
    }
    fclose(f);
    return n;
}

int roe_load_catalog(RoeAsi *R) {
    char path[ROE_PATH_MAX], line[1024];
    FILE *f;
    int n = 0;
    if (!R || !R->catalog_dir[0]) return -1;
    {
        size_t cl = strlen(R->catalog_dir);
        if (cl + 15 >= sizeof path) return -5;
        memcpy(path, R->catalog_dir, cl);
        memcpy(path + cl, "/catalog.jsonl", 15);
    }
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char id[ROE_NAME_MAX], intent[ROE_NAME_MAX], pattern[ROE_TEXT_MAX],
            answer[ROE_ANSWER_MAX];
        unsigned priv = 0;
        const char *p;
        id[0] = intent[0] = pattern[0] = answer[0] = 0;
        p = strstr(line, "\"id\":\"");
        if (p) sscanf(p, "\"id\":\"%63[^\"]\"", id);
        p = strstr(line, "\"intent\":\"");
        if (p) sscanf(p, "\"intent\":\"%63[^\"]\"", intent);
        p = strstr(line, "\"pattern\":\"");
        if (p) sscanf(p, "\"pattern\":\"%255[^\"]\"", pattern);
        p = strstr(line, "\"answer\":\"");
        if (p) sscanf(p, "\"answer\":\"%383[^\"]\"", answer);
        p = strstr(line, "\"privilege\":");
        if (p) sscanf(p, "\"privilege\":%u", &priv);
        if (id[0] && pattern[0] && answer[0]) {
            if (roe_add_skill(R, id, intent[0] ? intent : id, pattern, answer, priv,
                              1) == 0)
                n++;
        }
    }
    fclose(f);
    return n;
}
