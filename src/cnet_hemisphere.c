#include "cnet_hemisphere.h"

#include "cnet_brain_mirror.h"
#include "cnet_held_model.h"
#include "cnet_ood_skill.h"

#include <ctype.h>
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

static void clear_hemi(CnetHemiResult *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    out->via_core = 0;
    out->intent = CNET_CORE_INTENT_UNKNOWN;
    out->plane = CNET_CORE_PLANE_NONE;
    out->hemi = CNET_HEMI_NONE;
    out->source = CNET_HEMI_SRC_NONE;
}

static void mark_via_core(CnetHemiResult *out) {
    if (out) out->via_core = 1;
}

static int has_word_ci(const char *text, const char *word) {
    const char *cursor;
    size_t n, i;
    if (text == NULL || word == NULL || word[0] == '\0') return 0;
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
        if (left && right) return 1;
    }
    return 0;
}

static int has_substr_ci(const char *text, const char *needle) {
    size_t i, j, nt, nn;
    if (text == NULL || needle == NULL || needle[0] == '\0') return 0;
    nt = strlen(text);
    nn = strlen(needle);
    if (nn > nt) return 0;
    for (i = 0; i + nn <= nt; ++i) {
        for (j = 0; j < nn; ++j) {
            if (tolower((unsigned char)text[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nn) return 1;
    }
    return 0;
}

/* CORE discern — cheap heuristic. Not a model. "what is what". */
CnetCoreIntent cnet_core_discern(const char *turn) {
    int logic = 0, creative = 0;
    static const char *logic_w[] = {
        "plus", "minus", "times", "multiply", "divide", "compute", "calculate",
        "crc8", "crc", "increment", "mod256", "hash", "exact", "verify",
        "lookup", "year", "add", "subtract", "sum", "product", "modulo",
        "wiki", "fact", "true", "false", "equals", NULL};
    static const char *creative_w[] = {
        "poem", "haiku", "story", "imagine", "invent", "joke", "write",
        "draft", "brainstorm", "creative", "fiction", "song", "lyrics",
        "metaphor", "riddle", "roleplay", "pretend", "dream", "paint",
        "narrate", "essay", "opine", "rhyme", NULL};
    size_t i;

    if (turn == NULL || turn[0] == '\0') return CNET_CORE_INTENT_UNKNOWN;

    for (i = 0; logic_w[i]; ++i)
        if (has_word_ci(turn, logic_w[i]) || has_substr_ci(turn, logic_w[i]))
            logic = 1;
    for (i = 0; creative_w[i]; ++i)
        if (has_word_ci(turn, creative_w[i]) ||
            has_substr_ci(turn, creative_w[i]))
            creative = 1;

    /* digit op digit → logic */
    {
        const char *p = turn;
        int saw_digit = 0, saw_op = 0;
        for (; *p; ++p) {
            if (isdigit((unsigned char)*p)) saw_digit = 1;
            if (*p == '+' || *p == '*' || *p == '=' ||
                (*p == '-' && saw_digit))
                saw_op = 1;
        }
        if (saw_digit && saw_op) logic = 1;
    }

    if (logic && creative) return CNET_CORE_INTENT_MIXED;
    if (logic) return CNET_CORE_INTENT_LOGIC;
    if (creative) return CNET_CORE_INTENT_CREATIVE;
    return CNET_CORE_INTENT_UNKNOWN;
}

const char *cnet_core_intent_name(CnetCoreIntent i) {
    switch (i) {
    case CNET_CORE_INTENT_LOGIC:
        return "LOGIC";
    case CNET_CORE_INTENT_CREATIVE:
        return "CREATIVE";
    case CNET_CORE_INTENT_MIXED:
        return "MIXED";
    default:
        return "UNKNOWN";
    }
}

static void apply_plane_tags(CnetHemiResult *out, int open_chat_may_voice,
                             int never_voice_llm) {
    if (out == NULL) return;
    mark_via_core(out);
    if (!out->bound) {
        out->plane = CNET_CORE_PLANE_NONE;
        out->open_chat = 0;
        return;
    }
    if (out->hemi == CNET_HEMI_RESIDUAL ||
        out->source == CNET_HEMI_SRC_HELD_LLM) {
        out->plane = CNET_CORE_PLANE_OPEN_CHAT;
        out->open_chat = 1;
        out->claimed_cert = 0;
        out->hemi = CNET_HEMI_RESIDUAL;
        if (open_chat_may_voice)
            out->may_voice = 1;
        else
            out->may_voice = never_voice_llm ? 0 : 1;
        return;
    }
    if (out->hemi == CNET_HEMI_CORE) {
        out->plane = CNET_CORE_PLANE_CERT;
        out->open_chat = 0;
        return;
    }
    out->plane = CNET_CORE_PLANE_NONE;
    out->open_chat = 0;
}

void cnet_hemi_policy_default(CnetHemiPolicy *p) {
    const char *e;
    if (p == NULL) return;
    memset(p, 0, sizeof *p);
    p->never_voice_llm = 1;
    p->residual_enabled = 0; /* leftover OPEN_CHAT answers killed */
    p->open_chat_enabled = 0;
    p->allow_wiki = 1;
    p->open_chat_may_voice = 0;
    p->logic_open_chat_fallback = 0;
    /* Env can re-enable only for explicit experiments — product default is OFF */
    e = getenv("CNET_NEVER_VOICE_LLM");
    if (e && e[0] == '0') p->never_voice_llm = 0;
    e = getenv("CNET_CORE_OPEN_CHAT");
    if (e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y')) {
        p->open_chat_enabled = 1;
        p->residual_enabled = 1;
    }
    e = getenv("CNET_HEMI_RESIDUAL");
    if (e && e[0] == '1') {
        p->residual_enabled = 1;
        p->open_chat_enabled = 1;
    }
    e = getenv("CNET_HEMI_WIKI");
    if (e && e[0] == '0') p->allow_wiki = 0;
    e = getenv("CNET_OPEN_CHAT_MAY_VOICE");
    if (e && e[0] == '1') p->open_chat_may_voice = 1;
    e = getenv("CNET_LOGIC_OPEN_CHAT_FALLBACK");
    if (e && e[0] == '1') p->logic_open_chat_fallback = 1;
}

const char *cnet_hemi_name(CnetHemiId id) {
    switch (id) {
    case CNET_HEMI_CORE:
        return "CERT";
    case CNET_HEMI_RESIDUAL:
        return "OPEN_CHAT";
    default:
        return "NONE";
    }
}

const char *cnet_core_plane_name(CnetCorePlane p) {
    switch (p) {
    case CNET_CORE_PLANE_CERT:
        return "CERT";
    case CNET_CORE_PLANE_OPEN_CHAT:
        return "OPEN_CHAT";
    default:
        return "NONE";
    }
}

const char *cnet_hemi_source_name(CnetHemiSource s) {
    switch (s) {
    case CNET_HEMI_SRC_SKILL_EXACT:
        return "skill_exact";
    case CNET_HEMI_SRC_LOOKUP:
        return "lookup";
    case CNET_HEMI_SRC_OOD_MATH:
        return "ood_math";
    case CNET_HEMI_SRC_OOD_WIKI:
        return "ood_wiki";
    case CNET_HEMI_SRC_HELD_LLM:
        return "open_chat";
    case CNET_HEMI_SRC_ABSTAIN:
        return "abstain";
    default:
        return "none";
    }
}

static int is_math_skill(const char *skill) {
    if (skill == NULL) return 0;
    return strcmp(skill, CNET_OOD_ADD) == 0 || strcmp(skill, CNET_OOD_SUB) == 0 ||
           strcmp(skill, CNET_OOD_MUL) == 0;
}

static int is_wiki_skill(const char *skill) {
    return skill && strcmp(skill, CNET_OOD_WIKI) == 0;
}

static int is_held_skill(const char *skill) {
    return skill && strcmp(skill, CNET_HELD_CONTRACT) == 0;
}

static int lane_is_open_chat(const CnetSkillLaneResult *lane) {
    if (lane == NULL) return 0;
    if (lane->kind == CNET_SKILL_LANE_HELD) return 1;
    if (is_held_skill(lane->skill)) return 1;
    if (lane->residual_calls != 0 || lane->teacher_calls != 0) return 1;
    return 0;
}

void cnet_hemi_classify_lane(const CnetSkillLaneResult *lane, int never_voice_llm,
                             CnetHemiResult *out) {
    clear_hemi(out);
    if (lane == NULL || out == NULL) return;

    copy_text(out->skill, sizeof out->skill, lane->skill);
    copy_text(out->value, sizeof out->value, lane->value);
    copy_text(out->spoken, sizeof out->spoken, lane->spoken);
    copy_text(out->refusal, sizeof out->refusal, lane->refusal);
    out->bound = lane->bound;
    out->residual_calls = lane->residual_calls;
    out->teacher_calls = lane->teacher_calls;
    mark_via_core(out);

    if (!lane->bound) {
        out->hemi = CNET_HEMI_NONE;
        out->plane = CNET_CORE_PLANE_NONE;
        out->source = CNET_HEMI_SRC_ABSTAIN;
        out->claimed_cert = 0;
        out->may_voice = 0;
        out->open_chat = 0;
        if (out->refusal[0] == '\0')
            copy_text(out->refusal, sizeof out->refusal, "abstain");
        return;
    }

    if (lane_is_open_chat(lane)) {
        out->hemi = CNET_HEMI_RESIDUAL;
        out->plane = CNET_CORE_PLANE_OPEN_CHAT;
        out->open_chat = 1;
        out->source = CNET_HEMI_SRC_HELD_LLM;
        out->claimed_cert = 0;
        out->may_voice = never_voice_llm ? 0 : 1;
        return;
    }

    out->hemi = CNET_HEMI_CORE;
    out->plane = CNET_CORE_PLANE_CERT;
    out->open_chat = 0;
    if (is_math_skill(lane->skill))
        out->source = CNET_HEMI_SRC_OOD_MATH;
    else if (is_wiki_skill(lane->skill))
        out->source = CNET_HEMI_SRC_OOD_WIKI;
    else if (lane->skill[0] && strstr(lane->skill, "lookup") != NULL)
        out->source = CNET_HEMI_SRC_LOOKUP;
    else
        out->source = CNET_HEMI_SRC_SKILL_EXACT;
    out->claimed_cert = lane->claimed_cert ? 1 : 0;
    if (never_voice_llm)
        out->may_voice =
            (out->claimed_cert || lane->kind == CNET_SKILL_LANE_EXACT) ? 1 : 0;
    else
        out->may_voice = 1;
}

int cnet_hemi_may_voice(const CnetHemiResult *r, int never_voice_llm) {
    if (r == NULL || !r->bound) return 0;
    if (r->plane == CNET_CORE_PLANE_OPEN_CHAT || r->hemi == CNET_HEMI_RESIDUAL)
        return never_voice_llm ? 0 : 1;
    if (r->plane != CNET_CORE_PLANE_CERT && r->hemi != CNET_HEMI_CORE) return 0;
    if (never_voice_llm) return r->claimed_cert ? 1 : 0;
    return 1;
}

static int accept_cert_lane(const CnetSkillLaneResult *lane, int never_voice_llm,
                            CnetHemiResult *out) {
    if (lane == NULL || !lane->bound) return 1;
    if (lane_is_open_chat(lane)) return 1;
    cnet_hemi_classify_lane(lane, never_voice_llm, out);
    return 0;
}

int cnet_hemi_ask_core(const char *turn, const CnetHemiPolicy *policy,
                       CnetHemiResult *out) {
    CnetHemiPolicy pol;
    CnetSkillLaneResult lane;
    CnetCoreIntent intent;

    if (out == NULL) return -1;
    clear_hemi(out);
    mark_via_core(out);
    intent = cnet_core_discern(turn);
    out->intent = intent;
    if (policy)
        pol = *policy;
    else
        cnet_hemi_policy_default(&pol);

    if (turn == NULL || turn[0] == '\0') {
        out->source = CNET_HEMI_SRC_ABSTAIN;
        out->plane = CNET_CORE_PLANE_NONE;
        copy_text(out->refusal, sizeof out->refusal, "empty_turn");
        return 1;
    }

    memset(&lane, 0, sizeof lane);
    if (cnet_skill_lane_cd_ask(turn, NULL, &lane) == 0 &&
        accept_cert_lane(&lane, pol.never_voice_llm, out) == 0) {
        out->intent = intent;
        apply_plane_tags(out, pol.open_chat_may_voice, pol.never_voice_llm);
        (void)cnet_brain_mirror_core(out);
        return 0;
    }

    memset(&lane, 0, sizeof lane);
    if (cnet_ood_try_add(turn, &lane) == 0 &&
        accept_cert_lane(&lane, pol.never_voice_llm, out) == 0) {
        out->intent = intent;
        apply_plane_tags(out, pol.open_chat_may_voice, pol.never_voice_llm);
        (void)cnet_brain_mirror_core(out);
        return 0;
    }

#if CNET_HAVE_CURL
    if (pol.allow_wiki) {
        memset(&lane, 0, sizeof lane);
        if (cnet_ood_try_wiki(turn, 0, &lane) == 0 &&
            accept_cert_lane(&lane, pol.never_voice_llm, out) == 0) {
            out->intent = intent;
            apply_plane_tags(out, pol.open_chat_may_voice, pol.never_voice_llm);
            (void)cnet_brain_mirror_core(out);
            return 0;
        }
    }
#else
    (void)pol.allow_wiki;
#endif

    out->via_core = 1;
    out->intent = intent;
    out->hemi = CNET_HEMI_CORE;
    out->plane = CNET_CORE_PLANE_NONE;
    out->source = CNET_HEMI_SRC_ABSTAIN;
    out->bound = 0;
    out->claimed_cert = 0;
    out->may_voice = 0;
    out->open_chat = 0;
    copy_text(out->refusal, sizeof out->refusal, "core_miss");
    return 1;
}


int cnet_hemi_ask(const char *turn, const CnetHemiPolicy *policy,
                  CnetHemiResult *out) {
    CnetHemiPolicy pol;
    int rc;
    int open_ok;
    CnetCoreIntent intent;

    if (out == NULL) return -1;
    if (policy)
        pol = *policy;
    else
        cnet_hemi_policy_default(&pol);

    if (pol.open_chat_enabled && !pol.residual_enabled) pol.residual_enabled = 1;
    if (pol.residual_enabled && !pol.open_chat_enabled) pol.open_chat_enabled = 1;
    open_ok = pol.residual_enabled || pol.open_chat_enabled;
    intent = cnet_core_discern(turn);

    /* Always try CERT first — logic-strong plane, even on creative phrasing. */
    rc = cnet_hemi_ask_core(turn, &pol, out);
    if (rc == 0) {
        out->intent = intent;
        return 0;
    }
    if (rc < 0) return rc;
    out->intent = intent;

    /* CORE middle ground: after CERT miss, OPEN_CHAT may draft (claimed_cert=0).
     * This is not a bypass of never-self-CERT — it IS the creativity plane. */
    if (open_ok) {
        int allow = 0;
        if (intent == CNET_CORE_INTENT_CREATIVE || intent == CNET_CORE_INTENT_MIXED ||
            intent == CNET_CORE_INTENT_UNKNOWN)
            allow = 1;
        if (intent == CNET_CORE_INTENT_LOGIC && pol.logic_open_chat_fallback)
            allow = 1;
        if (allow) {
            char draft[CNET_HELD_TEXT];
            draft[0] = 0;
            if (cnet_held_model_ask(turn, draft, sizeof draft) == 0 && draft[0]) {
                clear_hemi(out);
                mark_via_core(out);
                out->intent = intent;
                out->hemi = CNET_HEMI_RESIDUAL;
                out->plane = CNET_CORE_PLANE_OPEN_CHAT;
                out->open_chat = 1;
                out->source = CNET_HEMI_SRC_HELD_LLM;
                out->bound = 1;
                out->claimed_cert = 0; /* NEVER self-CERT */
                out->may_voice = pol.open_chat_may_voice ? 1 : (pol.never_voice_llm ? 0 : 1);
                out->residual_calls = 1;
                copy_text(out->skill, sizeof out->skill, "core_open_chat");
                copy_text(out->value, sizeof out->value, draft);
                copy_text(out->spoken, sizeof out->spoken, draft);
                apply_plane_tags(out, pol.open_chat_may_voice, pol.never_voice_llm);
                /* do not brain_mirror open chat as CERT */
                return 0;
            }
        }
    }

    out->via_core = 1;
    out->intent = intent;
    out->hemi = CNET_HEMI_NONE;
    out->plane = CNET_CORE_PLANE_NONE;
    out->source = CNET_HEMI_SRC_ABSTAIN;
    out->bound = 0;
    out->claimed_cert = 0;
    out->may_voice = 0;
    out->open_chat = 0;
    if (intent == CNET_CORE_INTENT_LOGIC)
        copy_text(out->refusal, sizeof out->refusal, "logic_miss_no_creative_fill");
    else
        copy_text(out->refusal, sizeof out->refusal, "open_chat_unavailable");
    return 1;
}


int cnet_core_ask(const char *turn, const CnetHemiPolicy *policy,
                  CnetHemiResult *out) {
    return cnet_hemi_ask(turn, policy, out);
}

int cnet_core_ask_cert(const char *turn, const CnetHemiPolicy *policy,
                       CnetHemiResult *out) {
    return cnet_hemi_ask_core(turn, policy, out);
}
