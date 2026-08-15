#include "cnet_skill_lane.h"

#include "cnet_c_speak.h"
#include "cnet_lookup.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Explicit skill-name dispatch table. Hard switch. Not compete WordLM.
   Residual/teacher symbols are not invoked on this lane. */

typedef enum {
    SKILL_NONE = 0,
    SKILL_INCREMENT,
    SKILL_CRC8,
    SKILL_LOOKUP
} SkillId;

typedef struct {
    SkillId id;
    const char *name;
    const char *subjects[4];
} SkillRow;

static const SkillRow k_table[] = {
    {SKILL_INCREMENT, "increment_mod256", {"increment_mod256", "increment", NULL, NULL}},
    {SKILL_CRC8, "crc8_atm", {"crc8_atm", "crc8", NULL, NULL}},
    {SKILL_LOOKUP, CNET_LOOKUP_CONTRACT, {CNET_LOOKUP_CONTRACT, "lookup", NULL, NULL}},
};

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

static void clear_result(CnetSkillLaneResult *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
}

static int refuse(CnetSkillLaneResult *out, const char *why) {
    if (out == NULL) return -1;
    clear_result(out);
    copy_text(out->refusal, sizeof out->refusal, why);
    out->kind = CNET_SKILL_LANE_ABSTAIN;
    out->claimed_cert = 0;
    out->residual_calls = 0;
    out->teacher_calls = 0;
    return 0;
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

static int has_url_scheme(const char *text) {
    if (text == NULL) return 0;
    return strstr(text, "https://") != NULL || strstr(text, "http://") != NULL;
}

static SkillId id_of_name(const char *name) {
    size_t i;
    if (name == NULL || name[0] == '\0') return SKILL_NONE;
    for (i = 0; i < sizeof k_table / sizeof k_table[0]; ++i)
        if (strcmp(k_table[i].name, name) == 0) return k_table[i].id;
    return SKILL_NONE;
}

static unsigned crc8_atm(unsigned byte) {
    unsigned crc = byte & 255u;
    int bit;
    for (bit = 0; bit < 8; ++bit)
        crc = (crc & 0x80u) ? ((crc << 1) ^ 0x07u) & 255u
                            : (crc << 1) & 255u;
    return crc;
}

static int parse_u8(const char *turn, unsigned *out) {
    const char *p;
    if (turn == NULL || out == NULL) return 0;
    for (p = turn; *p != '\0'; ++p) {
        int left;
        if (!isdigit((unsigned char)*p)) continue;
        left = (p == turn) || !isalnum((unsigned char)p[-1]);
        if (!left) continue;
        {
            unsigned long x = 0;
            const char *q = p;
            while (isdigit((unsigned char)*q)) {
                x = x * 10ul + (unsigned long)(*q - '0');
                if (x > 255ul) return 0;
                ++q;
            }
            if (isalnum((unsigned char)*q)) continue;
            *out = (unsigned)x;
            return 1;
        }
    }
    return 0;
}

/* Exact commit: A value is the answer. Residual/teacher stay 0. */
static int exact_commit(CnetSkillLaneResult *out, const char *skill,
                        const char *value) {
    CnetCSpeakResult wrap;
    if (out == NULL || skill == NULL || value == NULL || value[0] == '\0')
        return -1;
    copy_text(out->skill, sizeof out->skill, skill);
    copy_text(out->value, sizeof out->value, value);
    out->bound = 1;
    out->kind = CNET_SKILL_LANE_EXACT;
    out->residual_calls = 0;
    out->teacher_calls = 0;
    out->refusal[0] = '\0';
    /* Optional C-wrap. Wrap failure still speaks the A value. */
    memset(&wrap, 0, sizeof wrap);
    if (cnet_c_speak_after_capsule(value, skill, &wrap) == 0 &&
        wrap.residual_calls == 0 && wrap.spoken[0] != '\0' &&
        strstr(wrap.spoken, value) != NULL) {
        copy_text(out->spoken, sizeof out->spoken, wrap.spoken);
    } else {
        copy_text(out->spoken, sizeof out->spoken, value);
    }
    out->claimed_cert = 1;
    return 0;
}

int cnet_skill_lane_route(const char *turn, char *skill, size_t cap) {
    size_t i, s;
    if (skill == NULL || cap == 0) return -1;
    skill[0] = '\0';
    if (turn == NULL || turn[0] == '\0') return 1;
    if (has_url_scheme(turn)) {
        copy_text(skill, cap, CNET_LOOKUP_CONTRACT);
        return 0;
    }
    for (i = 0; i < sizeof k_table / sizeof k_table[0]; ++i) {
        for (s = 0; s < 4 && k_table[i].subjects[s] != NULL; ++s) {
            if (has_word_ci(turn, k_table[i].subjects[s])) {
                copy_text(skill, cap, k_table[i].name);
                return 0;
            }
        }
    }
    return 1;
}

int cnet_skill_lane_bind_exact(const char *skill, unsigned operand,
                               CnetSkillLaneResult *out) {
    char value[16];
    unsigned result;
    SkillId id;
    if (out == NULL) return -1;
    clear_result(out);
    if (skill == NULL || skill[0] == '\0' || operand > 255u)
        return refuse(out, "no_bind");
    id = id_of_name(skill);
    if (id == SKILL_INCREMENT)
        result = (operand + 1u) & 255u;
    else if (id == SKILL_CRC8)
        result = crc8_atm(operand);
    else
        return refuse(out, "no_bind");
    snprintf(value, sizeof value, "%u", result);
    return exact_commit(out, skill, value);
}

int cnet_skill_lane_bind_fixture(const char *skill, const char *value,
                                 int verified, CnetSkillLaneResult *out) {
    if (out == NULL) return -1;
    clear_result(out);
    if (skill == NULL || skill[0] == '\0' || value == NULL || value[0] == '\0' ||
        !verified)
        return refuse(out, "no_bind");
    if (id_of_name(skill) == SKILL_NONE) return refuse(out, "ood_no_skill");
    return exact_commit(out, skill, value);
}

int cnet_skill_lane_bind_lookup(const CnetChatLookupTurn *hop,
                                CnetSkillLaneResult *out) {
    CnetCSpeakResult wrap;
    if (out == NULL) return -1;
    clear_result(out);
    if (hop == NULL || !hop->answered || !hop->report.bound ||
        hop->report.value[0] == '\0')
        return refuse(out, hop && hop->refusal[0] ? hop->refusal : "no_bind");
    if (hop->residual_calls != 0) {
        refuse(out, "residual_mouth");
        copy_text(out->skill, sizeof out->skill, CNET_LOOKUP_CONTRACT);
        return 0;
    }
    copy_text(out->skill, sizeof out->skill, CNET_LOOKUP_CONTRACT);
    copy_text(out->value, sizeof out->value, hop->report.value);
    out->bound = 1;
    out->kind = CNET_SKILL_LANE_EXACT;
    out->residual_calls = 0;
    out->teacher_calls = 0;
    memset(&wrap, 0, sizeof wrap);
    if (cnet_c_speak_after_lookup(hop, &wrap) == 0 &&
        wrap.residual_calls == 0 && wrap.spoken[0] != '\0' &&
        strstr(wrap.spoken, hop->report.value) != NULL) {
        copy_text(out->spoken, sizeof out->spoken, wrap.spoken);
    } else if (hop->spoken[0] != '\0') {
        copy_text(out->spoken, sizeof out->spoken, hop->spoken);
    } else {
        copy_text(out->spoken, sizeof out->spoken, hop->report.value);
    }
    out->claimed_cert = 1;
    return 0;
}

int cnet_skill_lane_turn(const char *turn, CnetSkillLaneResult *out) {
    return cnet_skill_lane_cd_ask(turn, NULL, out);
}

int cnet_skill_lane_cd_ask(const char *turn, const CnetChatLookupTurn *hop,
                           CnetSkillLaneResult *out) {
    char skill[CNET_SKILL_LANE_NAME];
    unsigned operand = 0;
    SkillId id;
    int routed;

    if (out == NULL) return -1;
    clear_result(out);

    /* Exact lookup hop, if already bound, wins. Never escalate. */
    if (hop != NULL && hop->answered && hop->report.bound &&
        hop->report.value[0] != '\0' && hop->residual_calls == 0)
        return cnet_skill_lane_bind_lookup(hop, out);
    if (hop != NULL && hop->residual_calls != 0) {
        refuse(out, "residual_mouth");
        copy_text(out->skill, sizeof out->skill, CNET_LOOKUP_CONTRACT);
        return 0;
    }

    routed = cnet_skill_lane_route(turn, skill, sizeof skill);
    if (routed != 0 || skill[0] == '\0')
        return refuse(out, "ood_no_skill");

    id = id_of_name(skill);
    if (id == SKILL_LOOKUP) {
        /* Named lookup without a bound hop: abstain. Do not call teacher. */
        refuse(out, "no_bind");
        copy_text(out->skill, sizeof out->skill, skill);
        return 0;
    }
    if ((id == SKILL_INCREMENT || id == SKILL_CRC8) && parse_u8(turn, &operand))
        return cnet_skill_lane_bind_exact(skill, operand, out);

    refuse(out, "no_bind");
    copy_text(out->skill, sizeof out->skill, skill);
    return 0;
}
