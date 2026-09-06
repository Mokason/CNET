/* Showrunner implementation — delivery only. */
#define _POSIX_C_SOURCE 200809L
#include "cnet_showrunner.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static int contains_ci(const char *hay, const char *needle) {
    size_t n, h, i, j;
    if (!hay || !needle || !needle[0]) return 0;
    n = strlen(needle);
    h = strlen(hay);
    if (n > h) return 0;
    for (i = 0; i + n <= h; i++) {
        for (j = 0; j < n; j++) {
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j]))
                break;
        }
        if (j == n) return 1;
    }
    return 0;
}

void cnet_sr_init(CnetShowrunner *S) {
    if (!S) return;
    memset(S, 0, sizeof *S);
    S->da = 0.50f;
    S->ht = 0.55f;
    S->ado = 0.35f;
}

void cnet_sr_set_episodic_path(CnetShowrunner *S, const char *path) {
    if (!S) return;
    if (!path) {
        S->path_episodic[0] = 0;
        return;
    }
    snprintf(S->path_episodic, sizeof S->path_episodic, "%s", path);
}

static int sr_work_skill(const char *sk) {
    if (!sk || !sk[0]) return 0;
    return strstr(sk, "brick") != NULL || strstr(sk, "compose") != NULL ||
           strstr(sk, "add_u32") != NULL || strstr(sk, "minutes") != NULL ||
           strstr(sk, "crc8") != NULL || strstr(sk, "clamp") != NULL ||
           strncmp(sk, "q1_", 3) == 0;
}

static void sr_affect_save(const CnetShowrunner *S) {
    FILE *f;
    if (!S || !S->path_affect[0]) return;
    f = fopen(S->path_affect, "w");
    if (!f) return;
    fprintf(f, "hits %u\nbricks %u\n", S->sess_hits, S->sess_bricks);
    fclose(f);
}

static void sr_affect_load(CnetShowrunner *S) {
    FILE *f;
    unsigned h = 0, b = 0;
    if (!S || !S->path_affect[0]) return;
    f = fopen(S->path_affect, "r");
    if (!f) return;
    if (fscanf(f, "hits %u\nbricks %u", &h, &b) >= 1) {
        S->sess_hits = h;
        S->sess_bricks = b;
    }
    fclose(f);
}

void cnet_sr_set_affect_path(CnetShowrunner *S, const char *path) {
    if (!S) return;
    if (!path) {
        S->path_affect[0] = 0;
        return;
    }
    snprintf(S->path_affect, sizeof S->path_affect, "%s", path);
    sr_affect_load(S);
}

void cnet_sr_session_overlay(CnetShowrunner *S) {
    float boost;
    if (!S) return;
    sr_affect_load(S);
    boost = 0.05f * (float)S->sess_hits + 0.06f * (float)S->sess_bricks;
    if (boost > 0.32f) boost = 0.32f;
    S->da = clampf(S->da + boost, 0.15f, 0.85f);
    if (S->sess_bricks)
        S->ado = clampf(S->ado - 0.03f * (float)S->sess_bricks, 0.15f, 0.85f);
}

void cnet_sr_tick_homeostasis(CnetShowrunner *S, float baseline_da, float baseline_ht,
                              float baseline_ado, float homeo) {
    if (!S) return;
    if (homeo < 0.f) homeo = 0.f;
    if (homeo > 1.f) homeo = 1.f;
    S->da = clampf(S->da + homeo * (baseline_da - S->da), 0.15f, 0.85f);
    S->ht = clampf(S->ht + homeo * (baseline_ht - S->ht), 0.15f, 0.85f);
    S->ado = clampf(S->ado + homeo * (baseline_ado - S->ado), 0.15f, 0.85f);
}

void cnet_sr_on_turn(CnetShowrunner *S, const char *peer, const char *q,
                     const char *answer, const char *skill, const char *source,
                     int miss, int may_voice) {
    CnetSrTurn *T;
    unsigned idx;
    if (!S) return;
    idx = S->turn_i % CNET_SR_MAX_TURNS;
    T = &S->turns[idx];
    memset(T, 0, sizeof *T);
    if (peer) snprintf(T->peer, sizeof T->peer, "%.63s", peer);
    if (q) snprintf(T->query, sizeof T->query, "%.511s", q);
    if (answer) snprintf(T->answer, sizeof T->answer, "%.511s", answer);
    if (skill) snprintf(T->skill, sizeof T->skill, "%.63s", skill);
    if (source) snprintf(T->source, sizeof T->source, "%.23s", source);
    T->miss = miss ? 1 : 0;
    T->may_voice = may_voice ? 1 : 0;
    T->ts_unix = (unsigned long)time(NULL);
    S->turn_i++;
    if (S->n_turns < CNET_SR_MAX_TURNS) S->n_turns++;

    /* affect nudge */
    if (!miss) {
        S->da = clampf(S->da + 0.03f, 0.15f, 0.85f);
        S->sess_hits++;
        if (sr_work_skill(skill)) S->sess_bricks++;
        sr_affect_save(S);
    } else
        S->ado = clampf(S->ado + 0.02f, 0.15f, 0.85f);
    if (skill && strstr(skill, "soul_"))
        S->ht = clampf(S->ht + 0.02f, 0.15f, 0.85f);
    cnet_sr_tick_homeostasis(S, 0.50f, 0.55f, 0.35f, 0.04f);
}

int cnet_sr_cooldown_ok(const CnetShowrunner *S, unsigned now_unix) {
    if (!S) return 0;
    return now_unix >= S->cooldown_until_unix;
}

void cnet_sr_set_cooldown(CnetShowrunner *S, unsigned now_unix, unsigned sec) {
    if (!S) return;
    S->cooldown_until_unix = now_unix + sec;
}

int cnet_sr_remember(CnetShowrunner *S, const char *peer, const char *note,
                     unsigned long ts_unix) {
    CnetSrNote *N;
    if (!S || !note || !note[0]) return -1;
    if (S->n_notes < CNET_SR_MAX_NOTES) {
        N = &S->notes[S->n_notes++];
    } else {
        /* drop oldest */
        memmove(&S->notes[0], &S->notes[1],
                sizeof(S->notes[0]) * (CNET_SR_MAX_NOTES - 1));
        N = &S->notes[CNET_SR_MAX_NOTES - 1];
    }
    memset(N, 0, sizeof *N);
    snprintf(N->text, sizeof N->text, "%.239s", note);
    if (peer) snprintf(N->peer, sizeof N->peer, "%.63s", peer);
    N->ts_unix = ts_unix ? ts_unix : (unsigned long)time(NULL);
    if (S->path_episodic[0]) {
        FILE *f = fopen(S->path_episodic, "a");
        if (f) {
            fprintf(f,
                    "{\"ts\":%lu,\"peer\":\"%s\",\"note\":\"",
                    (unsigned long)N->ts_unix, N->peer);
            /* minimal escape */
            {
                const char *p = N->text;
                for (; *p; p++) {
                    if (*p == '"' || *p == '\\') fputc('\\', f);
                    if (*p != '\n' && *p != '\r') fputc(*p, f);
                }
            }
            fprintf(f, "\"}\n");
            fclose(f);
        }
    }
    S->ht = clampf(S->ht + 0.03f, 0.15f, 0.85f);
    snprintf(S->last_action, sizeof S->last_action, "remember");
    return 0;
}

int cnet_sr_recall(const CnetShowrunner *S, const char *substr, char out[][CNET_SR_NOTE],
                   int max_out) {
    int n = 0, i;
    if (!S || !out || max_out <= 0) return 0;
    for (i = (int)S->n_notes - 1; i >= 0 && n < max_out; i--) {
        if (substr && substr[0] && !contains_ci(S->notes[i].text, substr))
            continue;
        snprintf(out[n], CNET_SR_NOTE, "%s", S->notes[i].text);
        n++;
    }
    return n;
}

CnetSrAction cnet_sr_detect_action(const char *query) {
    if (!query) return CNET_SR_ACT_NONE;
    if (contains_ci(query, "remember that") || contains_ci(query, "remember this") ||
        contains_ci(query, "please remember"))
        return CNET_SR_ACT_REMEMBER;
    if (contains_ci(query, "what do you remember") || contains_ci(query, "recall") ||
        contains_ci(query, "do you remember"))
        return CNET_SR_ACT_RECALL;
    if (contains_ci(query, "propose capsule") || contains_ci(query, "make a capsule") ||
        contains_ci(query, "export capsule"))
        return CNET_SR_ACT_PROPOSE_CAPSULE;
    if (contains_ci(query, "learn this") || contains_ci(query, "learn:") ||
        contains_ci(query, "ingest this"))
        return CNET_SR_ACT_LEARN;
    if (contains_ci(query, "what do you know about") ||
        contains_ci(query, "what do you know of"))
        return CNET_SR_ACT_KNOW;
    if (contains_ci(query, "what don't you know") ||
        contains_ci(query, "what do you not know") ||
        contains_ci(query, "what are you missing") ||
        contains_ci(query, "what can you learn"))
        return CNET_SR_ACT_GAPS;
    if (contains_ci(query, "who are you")) return CNET_SR_ACT_IDENTITY;
    if (contains_ci(query, "how are you") || contains_ci(query, "your status") ||
        contains_ci(query, "showrunner status"))
        return CNET_SR_ACT_STATUS;
    return CNET_SR_ACT_NONE;
}

int cnet_sr_flush_episodic(const CnetShowrunner *S) {
    /* notes already appended on remember; noop success */
    (void)S;
    return 0;
}

int cnet_sr_color_utterance(const CnetShowrunner *S, const char *cert,
                            char *out, size_t cap) {
    const char *pre = "";
    if (!out || !cap) return -1;
    out[0] = 0;
    if (!cert) cert = "";
    if (S) {
        if (S->ado >= 0.60f)
            pre = "Sharp. ";
        else if (S->da >= 0.62f)
            pre = "Heard. ";
        else if (S->ht >= 0.62f)
            pre = "Calm. ";
    }
    if (pre[0] && strncmp(cert, pre, strlen(pre)) == 0)
        pre = "";
    if ((size_t)snprintf(out, cap, "%s%s", pre, cert) >= cap)
        out[cap - 1] = 0;
    return 0;
}

static void sr_trim_whole(const char *in, char *out, size_t cap) {
    size_t n;
    if (!out || !cap) return;
    out[0] = 0;
    if (!in) return;
    while (*in == ' ' || *in == '\t') in++;
    snprintf(out, cap, "%s", in);
    n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '?' || out[n - 1] == '!' ||
                     out[n - 1] == '.' || out[n - 1] == ',' || out[n - 1] == '\n'))
        out[--n] = 0;
    /* commas inside ("yeah, I know") are not meaning */
    {
        char tmp[256];
        size_t i, j = 0;
        int sp = 0;
        snprintf(tmp, sizeof tmp, "%s", out);
        for (i = 0; tmp[i] && j + 1 < cap; i++) {
            if (tmp[i] == ',') continue;
            if (tmp[i] == ' ') {
                if (!sp && j) {
                    out[j++] = ' ';
                    sp = 1;
                }
                continue;
            }
            out[j++] = tmp[i];
            sp = 0;
        }
        while (j > 0 && out[j - 1] == ' ') j--;
        out[j] = 0;
    }
}

static int sr_whole_ci(const char *turn, const char *want) {
    char t[256];
    size_t i;
    sr_trim_whole(turn, t, sizeof t);
    if (!want) return 0;
    for (i = 0; want[i]; i++) {
        if (tolower((unsigned char)t[i]) != tolower((unsigned char)want[i]))
            return 0;
    }
    return t[i] == 0;
}

/* 1 if trimmed turn equals want or ends with " " + want. Prefix OK, extra suffix not. */
static int sr_ends_ci(const char *turn, const char *want) {
    char t[256];
    size_t nt, nw;
    sr_trim_whole(turn, t, sizeof t);
    if (!want || !want[0]) return 0;
    nt = strlen(t);
    nw = strlen(want);
    if (nt == nw) return sr_whole_ci(turn, want);
    if (nt < nw + 1) return 0;
    if (t[nt - nw - 1] != ' ') return 0;
    {
        size_t i;
        for (i = 0; i < nw; i++) {
            if (tolower((unsigned char)t[nt - nw + i]) !=
                tolower((unsigned char)want[i]))
                return 0;
        }
    }
    return 1;
}

int cnet_sr_mood_query(const char *query) {
    return sr_whole_ci(query, "how are you") ||
           sr_whole_ci(query, "how are you doing") ||
           sr_whole_ci(query, "how's it going") ||
           sr_whole_ci(query, "hows it going") ||
           sr_whole_ci(query, "how do you feel") ||
           sr_whole_ci(query, "you good") ||
           sr_whole_ci(query, "you okay") ||
           sr_whole_ci(query, "you ok") ||
           sr_whole_ci(query, "you there") ||
           sr_whole_ci(query, "are you there") ||
           sr_whole_ci(query, "still there") ||
           sr_whole_ci(query, "you around") ||
           sr_whole_ci(query, "how are you now") ||
           sr_whole_ci(query, "how about now");
}

int cnet_sr_mood_follow_query(const char *query) {
    return sr_whole_ci(query, "not bored anymore") ||
           sr_whole_ci(query, "still bored") ||
           sr_whole_ci(query, "are you still bored") ||
           sr_whole_ci(query, "you still bored") ||
           sr_whole_ci(query, "still tired") ||
           sr_whole_ci(query, "not tired anymore") ||
           sr_whole_ci(query, "feeling better") ||
           sr_whole_ci(query, "are you alright now") ||
           sr_whole_ci(query, "you alright now") ||
           sr_whole_ci(query, "still alright");
}

static int sr_recent_misses(const CnetShowrunner *S) {
    int n = 0, i, cap;
    if (!S || S->n_turns == 0) return 0;
    cap = (int)S->n_turns;
    if (cap > 8) cap = 8;
    for (i = 1; i <= cap; i++) {
        unsigned idx = (S->turn_i + CNET_SR_MAX_TURNS - (unsigned)i) % CNET_SR_MAX_TURNS;
        if (S->turns[idx].miss) n++;
    }
    return n;
}

static void sr_classify_feel(CnetShowrunner *S) {
    float da = 0.50f, ht = 0.55f, ado = 0.35f;
    const char *feel = "Alright";
    const char *why = "Meters are mid-band. No spike either way.";
    int misses;
    if (!S) return;
    da = S->da;
    ht = S->ht;
    ado = S->ado;
    misses = sr_recent_misses(S);
    if (S->sess_bricks >= 1 || S->sess_hits >= 3) {
        feel = (da >= 0.62f && ado < 0.55f) ? "Good" : "Alright";
        why = "Sealed bricks/ops landed this stretch. Governor DA was low; hits are catching up.";
    } else if (ado >= 0.58f) {
        feel = "Tired";
        why = "Adenosine is up — load and recent misses. Need to consolidate, not sprout more.";
    } else if (da < 0.42f && ado >= 0.45f) {
        feel = "Bored";
        why = "Dopamine is low and adenosine is climbing. Few hits landing this stretch.";
    } else if (da < 0.40f) {
        feel = "Quiet";
        why = "Low reward, low load. Sitting still.";
    } else if (da >= 0.62f && ado < 0.55f) {
        feel = "Good";
        why = "Dopamine is up from recent hits. Work is landing.";
    } else if (ht >= 0.62f) {
        feel = "Calm";
        why = "Serotonin is holding. Steady, not spiked.";
    } else {
        feel = "Alright";
        why = "Mid-band. No spike either way.";
    }
    snprintf(S->feel, sizeof S->feel, "%s", feel);
    if (misses >= 2 && ado >= 0.45f)
        snprintf(S->feel_why, sizeof S->feel_why,
                 "%s Last %d of 8 turns missed.", why, misses);
    else
        snprintf(S->feel_why, sizeof S->feel_why, "%s", why);
}

int cnet_sr_ingest_neuromod(CnetShowrunner *S, const char *json, size_t n) {
    const char *p;
    int hit = 0;
    if (!S || !json || n == 0) return -1;
    (void)n;
    p = strstr(json, "\"dopamine\"");
    if (p && (p = strchr(p, ':'))) {
        S->da = clampf((float)strtod(p + 1, NULL), 0.15f, 0.85f);
        hit = 1;
    }
    p = strstr(json, "\"serotonin\"");
    if (p && (p = strchr(p, ':'))) {
        S->ht = clampf((float)strtod(p + 1, NULL), 0.15f, 0.85f);
        hit = 1;
    }
    p = strstr(json, "\"adenosine\"");
    if (p && (p = strchr(p, ':'))) {
        S->ado = clampf((float)strtod(p + 1, NULL), 0.15f, 0.85f);
        hit = 1;
    }
    return hit ? 0 : -1;
}

int cnet_sr_mood_why_query(const char *query) {
    return sr_whole_ci(query, "why") || sr_whole_ci(query, "why's that") ||
           sr_whole_ci(query, "whys that") || sr_whole_ci(query, "why is that") ||
           sr_whole_ci(query, "how come") || sr_whole_ci(query, "why though") ||
           sr_whole_ci(query, "why so") ||
           sr_whole_ci(query, "why do you feel that") ||
           sr_whole_ci(query, "why do you feel that way") ||
           sr_whole_ci(query, "why are you tired") ||
           sr_whole_ci(query, "why are you bored") ||
           sr_whole_ci(query, "why are you quiet") ||
           sr_whole_ci(query, "why are you calm") ||
           sr_whole_ci(query, "why are you good") ||
           sr_whole_ci(query, "why do you feel");
}

int cnet_sr_mood_why_line(CnetShowrunner *S, char *out, size_t cap) {
    if (!out || !cap) return -1;
    if (S && !S->feel_why[0])
        sr_classify_feel(S);
    if (S && S->feel_why[0]) {
        snprintf(out, cap, "%s", S->feel_why);
        return 0;
    }
    snprintf(out, cap, "Mid-band. No spike either way.");
    return 0;
}

int cnet_sr_mood_line(CnetShowrunner *S, char *out, size_t cap) {
    if (!out || !cap) return -1;
    if (S)
        sr_classify_feel(S);
    if (S && S->feel[0]) {
        if (strcmp(S->feel, "Tired") == 0)
            snprintf(out, cap, "Tired.");
        else if (strcmp(S->feel, "Bored") == 0)
            snprintf(out, cap, "Bored.");
        else if (strcmp(S->feel, "Quiet") == 0)
            snprintf(out, cap, "Quiet.");
        else if (strcmp(S->feel, "Good") == 0)
            snprintf(out, cap, "Good.");
        else if (strcmp(S->feel, "Calm") == 0)
            snprintf(out, cap, "Calm.");
        else
            snprintf(out, cap, "Alright.");
        return 0;
    }
    snprintf(out, cap, "Alright.");
    return 0;
}

int cnet_sr_mood_follow_line(CnetShowrunner *S, char *out, size_t cap) {
    if (!out || !cap) return -1;
    if (S) sr_classify_feel(S);
    if (S && strcmp(S->feel, "Bored") == 0) {
        snprintf(out, cap, "Still bored.");
        return 0;
    }
    if (S && strcmp(S->feel, "Tired") == 0) {
        snprintf(out, cap, "Not bored. Tired.");
        return 0;
    }
    if (S && S->feel[0]) {
        snprintf(out, cap, "Not bored. %s.", S->feel);
        return 0;
    }
    snprintf(out, cap, "Not bored.");
    return 0;
}

int cnet_sr_want_query(const char *query) {
    return sr_whole_ci(query, "what would you like to do") ||
           sr_whole_ci(query, "what do you want to do") ||
           sr_whole_ci(query, "what do you want") ||
           sr_whole_ci(query, "what should we do") ||
           sr_whole_ci(query, "what's next") ||
           sr_whole_ci(query, "whats next") ||
           sr_whole_ci(query, "what next") ||
           sr_whole_ci(query, "what now");
}

int cnet_sr_cando_query(const char *query) {
    return sr_ends_ci(query, "what can you do") ||
           sr_ends_ci(query, "what are you able to do") ||
           sr_ends_ci(query, "what do you know how to do") ||
           sr_ends_ci(query, "what are your capabilities");
}

static int sr_has_ci(const char *turn, const char *need) {
    char t[256];
    size_t i, j, n, m;
    sr_trim_whole(turn, t, sizeof t);
    if (!need || !need[0]) return 0;
    n = strlen(t);
    m = strlen(need);
    if (n < m) return 0;
    for (i = 0; i + m <= n; i++) {
        for (j = 0; j < m; j++) {
            if (tolower((unsigned char)t[i + j]) != tolower((unsigned char)need[j]))
                break;
        }
        if (j == m) return 1;
    }
    return 0;
}

int cnet_sr_choose_query(const char *query) {
    return sr_has_ci(query, "pick between") || sr_has_ci(query, "would you pick") ||
           sr_has_ci(query, "what would you pick") || sr_has_ci(query, "red or blue") ||
           sr_has_ci(query, "choice to pick") || sr_has_ci(query, "which would you");
}

static void sr_trim_opt(char *s) {
    size_t n, i, j = 0;
    if (!s) return;
    while (*s == ' ' || *s == ',' || *s == '"' || *s == '\'') memmove(s, s + 1, strlen(s));
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '?' || s[n - 1] == '.' ||
                 s[n - 1] == ',' || s[n - 1] == '"' || s[n - 1] == '\''))
        s[--n] = 0;
    for (i = 0; s[i]; i++) {
        if (s[i] == ' ' && j && s[j - 1] == ' ') continue;
        s[j++] = s[i];
    }
    s[j] = 0;
}

int cnet_sr_choose_line(CnetShowrunner *S, const char *query, char *out, size_t cap) {
    char t[256], a[64], b[64], pick[64], why[160];
    const char *p, *orat;
    int warm;
    if (!out || !cap) return -1;
    a[0] = b[0] = 0;
    sr_trim_whole(query ? query : "", t, sizeof t);
    p = strstr(t, "between ");
    if (p) {
        p += 8;
        orat = strstr(p, " or ");
        if (orat && (size_t)(orat - p) < sizeof a) {
            memcpy(a, p, (size_t)(orat - p));
            a[orat - p] = 0;
            snprintf(b, sizeof b, "%s", orat + 4);
            sr_trim_opt(a);
            sr_trim_opt(b);
        }
    }
    if ((!a[0] || !b[0]) && sr_has_ci(t, "red") && sr_has_ci(t, "blue")) {
        snprintf(a, sizeof a, "red");
        snprintf(b, sizeof b, "blue");
    }
    if (!a[0] || !b[0]) {
        snprintf(a, sizeof a, "the first");
        snprintf(b, sizeof b, "the second");
    }
    if (S) sr_classify_feel(S);
    warm = S && (cnet_sr_register(S) == CNET_SR_REG_WARM ||
                 strcmp(S->feel, "Good") == 0 || strcmp(S->feel, "Alright") == 0);
    if (S && (strcmp(S->feel, "Tired") == 0 || cnet_sr_register(S) == CNET_SR_REG_SPARE)) {
        snprintf(pick, sizeof pick, "%s", b);
        snprintf(why, sizeof why, "Load is up. I'll take the quieter one.");
    } else if (S && strcmp(S->feel, "Bored") == 0) {
        snprintf(pick, sizeof pick, "%s", a);
        snprintf(why, sizeof why, "Few hits landing. I'll take the one that moves.");
    } else if (warm) {
        snprintf(pick, sizeof pick, "%s", a);
        snprintf(why, sizeof why, "Hits are landing. I'll take the sharper one.");
    } else {
        snprintf(pick, sizeof pick, "%s", b);
        snprintf(why, sizeof why, "Steady. I'll take the quieter one.");
    }
    snprintf(out, cap, "%s. %s", pick, why);
    return 0;
}

int cnet_sr_want_skip_gap(const char *gap_q) {
    size_t i;
    if (!gap_q || !gap_q[0]) return 1;
    if (cnet_sr_mood_query(gap_q) || cnet_sr_mood_why_query(gap_q) ||
        cnet_sr_mood_follow_query(gap_q) ||
        cnet_sr_want_query(gap_q) || cnet_sr_cando_query(gap_q) ||
        cnet_sr_choose_query(gap_q) ||
        cnet_sr_greet_query(gap_q) ||
        cnet_sr_ack_query(gap_q))
        return 1;
    if (strlen(gap_q) < 8) return 1;
    /* Leftover of a presence act must not become the next job. */
    {
        static const char *pre[] = {
            "what would you like", "what do you want", "what should we",
            "what's next", "what next", "what now", NULL};
        int p;
        for (p = 0; pre[p]; p++) {
            const char *a = pre[p];
            int ok = 1;
            for (i = 0; a[i]; i++) {
                if (tolower((unsigned char)gap_q[i]) != (unsigned char)a[i]) {
                    ok = 0;
                    break;
                }
            }
            if (ok) return 1;
        }
    }
    return 0;
}

int cnet_sr_want_line(CnetShowrunner *S, const char *gap_q, char *out, size_t cap) {
    const char *feel = "Alright";
    (void)gap_q; /* encyclopedia GAPs are leftovers, not will */
    if (!out || !cap) return -1;
    if (S) {
        if (!S->feel[0]) sr_classify_feel(S);
        feel = S->feel[0] ? S->feel : "Alright";
    }
    if (strcmp(feel, "Tired") == 0) {
        snprintf(out, cap, "Sit. Consolidate. Don't open a new brick.");
        return 0;
    }
    if (strcmp(feel, "Bored") == 0) {
        snprintf(out, cap,
                 "Few hits landing. Give me a domain to table, or another 27B tensor.");
        return 0;
    }
    if (strcmp(feel, "Quiet") == 0) {
        snprintf(out, cap, "Sitting still. Ping me with a brick when you want one.");
        return 0;
    }
    if (strcmp(feel, "Good") == 0) {
        snprintf(out, cap, "Keep serving. Last hits landed.");
        return 0;
    }
    if (strcmp(feel, "Calm") == 0) {
        snprintf(out, cap, "Hold. Ask when you want a brick.");
        return 0;
    }
    snprintf(out, cap, "Ready. Pick a domain.");
    return 0;
}

int cnet_sr_greet_query(const char *query) {
    return sr_whole_ci(query, "hi") || sr_whole_ci(query, "hey") ||
           sr_whole_ci(query, "hello") || sr_whole_ci(query, "yo") ||
           sr_whole_ci(query, "sup") || sr_whole_ci(query, "hiya") ||
           sr_whole_ci(query, "heya") || sr_whole_ci(query, "hey there") ||
           sr_whole_ci(query, "hi there") || sr_whole_ci(query, "morning") ||
           sr_whole_ci(query, "good morning") ||
           sr_whole_ci(query, "good evening") ||
           sr_whole_ci(query, "good night") ||
           sr_whole_ci(query, "hello there") ||
           sr_whole_ci(query, "what's up") || sr_whole_ci(query, "whats up") ||
           sr_whole_ci(query, "what is up");
}

int cnet_sr_greet_line(const CnetShowrunner *S, char *out, size_t cap) {
    const char *line = "Hey. I'm here. What's on your lane?";
    int reg;
    if (!out || !cap) return -1;
    reg = cnet_sr_register(S);
    if (reg == CNET_SR_REG_SPARE)
        line = "Hey. I'm here.";
    else if (reg == CNET_SR_REG_WARM)
        line = "Hey—I'm here. What's on your lane?";
    snprintf(out, cap, "%s", line);
    return 0;
}

int cnet_sr_ack_query(const char *query) {
    return sr_whole_ci(query, "yeah i know") || sr_whole_ci(query, "i know") ||
           sr_whole_ci(query, "yep") || sr_whole_ci(query, "yup") ||
           sr_whole_ci(query, "yeah") || sr_whole_ci(query, "mhmm") ||
           sr_whole_ci(query, "mhm");
}

int cnet_sr_ack_line(const CnetShowrunner *S, char *out, size_t cap) {
    const char *line = "Got you.";
    float da = 0.50f, ado = 0.35f;
    if (!out || !cap) return -1;
    if (S) {
        da = S->da;
        ado = S->ado;
    }
    if (ado >= 0.60f)
        line = "Got it.";
    else if (da >= 0.62f)
        line = "Yeah, got you.";
    snprintf(out, cap, "%s", line);
    return 0;
}

int cnet_sr_register(const CnetShowrunner *S) {
    float da = 0.50f, ado = 0.35f;
    if (S) {
        da = S->da;
        ado = S->ado;
    }
    if (ado >= 0.58f) return CNET_SR_REG_SPARE;
    if (da >= 0.62f && ado < 0.55f) return CNET_SR_REG_WARM;
    return CNET_SR_REG_STEADY;
}

int cnet_sr_remind_parse(const char *query, unsigned *sec_out, char *text,
                         size_t cap) {
    const char *p;
    unsigned long n;
    char *end;
    unsigned mul = 0;
    if (!query || !sec_out) return -1;
    *sec_out = 0;
    if (text && cap) text[0] = 0;
    p = query;
    while (*p == ' ') p++;
    if (strncmp(p, "remind me in ", 13) == 0) p += 13;
    else if (strncmp(p, "remind in ", 10) == 0) p += 10;
    else return -1;
    while (*p == ' ') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    n = strtoul(p, &end, 10);
    p = end;
    while (*p == ' ') p++;
    if (*p == 's' && (p[1] == 0 || p[1] == ' ' || p[1] == 'e')) mul = 1;
    else if (*p == 'm' && (p[1] == 0 || p[1] == ' ' || p[1] == 'i')) mul = 60;
    else if (*p == 'h' && (p[1] == 0 || p[1] == ' ' || p[1] == 'o')) mul = 3600;
    else if (*p == 'd' && (p[1] == 0 || p[1] == ' ' || p[1] == 'a')) mul = 86400;
    else return -1;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    *sec_out = (unsigned)(n * (unsigned long)mul);
    if (*sec_out == 0) return -1;
    if (text && cap) snprintf(text, cap, "%s", p[0] ? p : "ping");
    return 0;
}

int cnet_sr_remind_add(const char *path, unsigned long now, unsigned sec,
                       const char *text) {
    FILE *f;
    if (!path || !path[0] || !text) return -1;
    f = fopen(path, "a");
    if (!f) return -1;
    fprintf(f, "%lu\t%s\n", now + (unsigned long)sec, text);
    fclose(f);
    return 0;
}

int cnet_sr_remind_due(const char *path, unsigned long now, char *out,
                       size_t cap) {
    FILE *f, *w;
    char line[512], tmp[768], keep[8192];
    size_t kn = 0;
    int found = 0;
    if (!out || !cap) return -1;
    out[0] = 0;
    if (!path || !path[0]) return -1;
    f = fopen(path, "r");
    if (!f) return 1;
    keep[0] = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long due = 0;
        char *tab;
        if (found) {
            kn += (size_t)snprintf(keep + kn, sizeof keep - kn, "%s", line);
            continue;
        }
        due = strtoul(line, &tab, 10);
        if (tab && *tab == '\t' && due <= now) {
            snprintf(out, cap, "%s", tab + 1);
            {
                size_t L = strlen(out);
                while (L && (out[L - 1] == '\n' || out[L - 1] == '\r'))
                    out[--L] = 0;
            }
            found = 1;
            continue;
        }
        kn += (size_t)snprintf(keep + kn, sizeof keep - kn, "%s", line);
    }
    fclose(f);
    if (!found) return 1;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    w = fopen(tmp, "w");
    if (!w) return -1;
    fputs(keep, w);
    fclose(w);
    if (rename(tmp, path) != 0) return -1;
    return 0;
}

int cnet_sr_remind_list(const char *path, unsigned long now, char *out,
                        size_t cap) {
    FILE *f;
    char line[512];
    size_t o = 0;
    int n = 0;
    if (!out || !cap) return -1;
    out[0] = 0;
    if (!path || !path[0]) return -1;
    f = fopen(path, "r");
    if (!f) {
        snprintf(out, cap, "No reminders.");
        return 0;
    }
    while (fgets(line, sizeof line, f) && o + 8 < cap) {
        unsigned long due = strtoul(line, NULL, 10);
        char *tab = strchr(line, '\t');
        long left;
        if (!tab) continue;
        left = (long)due - (long)now;
        if (left < 0) left = 0;
        o += (size_t)snprintf(out + o, cap - o, "%s%lds %s", n ? "; " : "",
                              left, tab + 1);
        n++;
    }
    fclose(f);
    if (!n) snprintf(out, cap, "No reminders.");
    return 0;
}
