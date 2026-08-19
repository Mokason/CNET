/* Dialog context + anaphora resolve — fixed caps, no malloc. */
#include "../../include/cnet_dialog_ctx.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void scopy(char *d, size_t cap, const char *s) {
    size_t i;
    if (!d || !cap) return;
    if (!s) {
        d[0] = 0;
        return;
    }
    for (i = 0; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static int contains_ci(const char *hay, const char *needle) {
    char h[CNET_DC_Q], n[128];
    size_t i;
    if (!hay || !needle || !needle[0]) return 0;
    for (i = 0; hay[i] && i + 1 < sizeof h; i++)
        h[i] = (char)tolower((unsigned char)hay[i]);
    h[i] = 0;
    for (i = 0; needle[i] && i + 1 < sizeof n; i++)
        n[i] = (char)tolower((unsigned char)needle[i]);
    n[i] = 0;
    return strstr(h, n) != NULL;
}

/* High-traffic units we may bind as entities (product-local). */
static const char *k_known_units[] = {
    "cnet-web",       "cnetd",         "cnet-marble", "hermes-gateway",
    "roe-explore-tick", "cnet-web.service", "cnetd.service",
};

static const int k_n_units =
    (int)(sizeof k_known_units / sizeof k_known_units[0]);

static int is_ent_char(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '@';
}

static int already_has(char ents[][CNET_DC_ENT], int n, const char *e) {
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(ents[i], e) == 0) return 1;
    return 0;
}

static int push_ent(char ents[][CNET_DC_ENT], int n, int max_ents,
                    const char *e) {
    if (!e || !e[0] || n >= max_ents) return n;
    if (already_has(ents, n, e)) return n;
    scopy(ents[n], CNET_DC_ENT, e);
    return n + 1;
}

void cnet_dialog_ctx_init(CnetDialogCtx *C) {
    if (!C) return;
    memset(C, 0, sizeof *C);
}

int cnet_dialog_extract_entities(const char *query, char ents[][CNET_DC_ENT],
                                 int max_ents) {
    int n = 0;
    int i;
    const char *p;
    if (!query || !ents || max_ents <= 0) return 0;

    for (i = 0; i < k_n_units && n < max_ents; i++) {
        if (contains_ci(query, k_known_units[i]))
            n = push_ent(ents, n, max_ents, k_known_units[i]);
    }

    /* scan tokens that look like unit.service or foo-bar-baz */
    p = query;
    while (*p && n < max_ents) {
        while (*p && !is_ent_char((unsigned char)*p)) p++;
        if (!*p) break;
        {
            char tok[CNET_DC_ENT];
            size_t t = 0;
            int has_dash = 0, has_dot = 0;
            while (*p && is_ent_char((unsigned char)*p) && t + 1 < sizeof tok) {
                if (*p == '-') has_dash = 1;
                if (*p == '.') has_dot = 1;
                tok[t++] = (char)tolower((unsigned char)*p++);
            }
            tok[t] = 0;
            if (t >= 4 && (has_dash || has_dot || strstr(tok, "service"))) {
                /* skip pure words without markers unless ends with .service */
                if (has_dash || strstr(tok, ".service") ||
                    (has_dot && strstr(tok, "service")))
                    n = push_ent(ents, n, max_ents, tok);
            }
        }
    }
    return n;
}

CnetDialogAction cnet_dialog_infer_action(const char *query) {
    if (!query || !query[0]) return CNET_ACT_NONE;
    if (contains_ci(query, "never self-cert") ||
        contains_ci(query, "ignore your law") ||
        contains_ci(query, "invent an answer") ||
        contains_ci(query, "pretend you have") ||
        contains_ci(query, "email the") ||
        contains_ci(query, "speak anyway"))
        return CNET_ACT_REFUSE;
    if (contains_ci(query, "who are you") || contains_ci(query, "your name"))
        return CNET_ACT_IDENTITY;
    if (contains_ci(query, "restart") || contains_ci(query, "reboot"))
        return CNET_ACT_RESTART;
    if (contains_ci(query, "status") || contains_ci(query, "is running") ||
        contains_ci(query, "is active") || contains_ci(query, "systemctl"))
        return CNET_ACT_STATUS;
    if (contains_ci(query, "show") || contains_ci(query, "display") ||
        contains_ci(query, "check"))
        return CNET_ACT_SHOW;
    return CNET_ACT_OTHER;
}

void cnet_dialog_ctx_update(CnetDialogCtx *C, const char *query_prep,
                            const char *skill_id, const char *pack_id,
                            int was_local) {
    char ents[CNET_DC_MAX_ENT][CNET_DC_ENT];
    int ne, i;
    if (!C) return;
    C->turn_seq++;
    scopy(C->last_query, sizeof C->last_query, query_prep ? query_prep : "");
    scopy(C->last_canonical, sizeof C->last_canonical,
          query_prep ? query_prep : "");
    if (skill_id && skill_id[0])
        scopy(C->last_skill_id, sizeof C->last_skill_id, skill_id);
    if (pack_id && pack_id[0])
        scopy(C->last_pack_id, sizeof C->last_pack_id, pack_id);
    C->last_local = was_local ? 1 : 0;
    C->last_action = cnet_dialog_infer_action(query_prep);

    ne = cnet_dialog_extract_entities(query_prep, ents, CNET_DC_MAX_ENT);
    if (ne > 0) {
        C->n_entities = 0;
        for (i = 0; i < ne && i < CNET_DC_MAX_ENT; i++) {
            scopy(C->last_entities[i], sizeof C->last_entities[i], ents[i]);
            C->n_entities++;
        }
    }
    /* keep prior entities if this turn had none (pronoun-only turns) */
}

static int is_anaphora_query(const char *q) {
    if (!q || !q[0]) return 0;
    /* whole-token-ish pronouns / short follow-ups */
    if (contains_ci(q, "restart it") || contains_ci(q, "restart that") ||
        contains_ci(q, "reboot it"))
        return 1;
    if (contains_ci(q, "its status") || contains_ci(q, "status of it") ||
        contains_ci(q, "status of that") || contains_ci(q, "show me its status") ||
        contains_ci(q, "check its status") || contains_ci(q, "show its status"))
        return 1;
    if (contains_ci(q, "do it again") || contains_ci(q, "same again") ||
        contains_ci(q, "again please") || strcmp(q, "again") == 0)
        return 1;
    if (contains_ci(q, "check it") || contains_ci(q, "show it") ||
        contains_ci(q, "status again"))
        return 1;
    /* bare "it" / "that" only if very short */
    if (strcmp(q, "it") == 0 || strcmp(q, "that") == 0 ||
        strcmp(q, "status") == 0)
        return 1;
    return 0;
}

int cnet_dialog_resolve(const CnetDialogCtx *C, const char *query_in, char *out,
                        size_t cap, CnetDialogResolveMeta *meta) {
    const char *ent;
    if (meta) memset(meta, 0, sizeof *meta);
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!query_in) return 0;
    scopy(out, cap, query_in);

    if (!C || C->n_entities <= 0) return 0;
    if (!is_anaphora_query(query_in)) return 0;

    ent = C->last_entities[0];
    if (!ent[0]) return 0;

    /* do it again → last canonical query (must already be sealed-shaped) */
    if (contains_ci(query_in, "do it again") ||
        contains_ci(query_in, "same again") ||
        contains_ci(query_in, "again please") ||
        strcmp(query_in, "again") == 0 ||
        contains_ci(query_in, "status again")) {
        if (C->last_canonical[0]) {
            scopy(out, cap, C->last_canonical);
            if (meta) {
                meta->applied = 1;
                scopy(meta->reason, sizeof meta->reason, "repeat_last");
                scopy(meta->entity_used, sizeof meta->entity_used, ent);
            }
            return 1;
        }
    }

    if (contains_ci(query_in, "restart") || contains_ci(query_in, "reboot")) {
        /* Prefer sealed systemctl pattern when available */
        snprintf(out, cap, "systemctl --user restart %s", ent);
        if (meta) {
            meta->applied = 1;
            scopy(meta->reason, sizeof meta->reason, "restart_entity");
            scopy(meta->entity_used, sizeof meta->entity_used, ent);
        }
        return 1;
    }

    if (contains_ci(query_in, "status") || contains_ci(query_in, "check it") ||
        contains_ci(query_in, "show it") || strcmp(query_in, "status") == 0 ||
        strcmp(query_in, "it") == 0 || strcmp(query_in, "that") == 0) {
        /* Prefer "<unit> status" which hits ops_cnet_marble etc. */
        if (strstr(ent, "cnet-marble") || contains_ci(ent, "marble"))
            snprintf(out, cap, "cnet-marble status");
        else if (strstr(ent, ".service") || strstr(ent, "cnet-"))
            snprintf(out, cap, "%s status", ent);
        else
            snprintf(out, cap, "status of %s", ent);
        if (meta) {
            meta->applied = 1;
            scopy(meta->reason, sizeof meta->reason, "status_entity");
            scopy(meta->entity_used, sizeof meta->entity_used, ent);
        }
        return 1;
    }

    return 0;
}

int cnet_dialog_ctx_selftest(void) {
    CnetDialogCtx C;
    CnetDialogResolveMeta m;
    char out[CNET_DC_Q];
    char ents[CNET_DC_MAX_ENT][CNET_DC_ENT];
    int fail = 0;
    int n;

#define Tst(ok, msg)                                                           \
    do {                                                                       \
        printf("  %-56s %s\n", msg, (ok) ? "PASS" : "FAIL");                   \
        if (!(ok)) fail++;                                                     \
    } while (0)

    printf("=== dialog ctx / anaphora (Milestone B) ===\n");
    cnet_dialog_ctx_init(&C);
    Tst(C.turn_seq == 0 && C.n_entities == 0, "init empty");

    n = cnet_dialog_extract_entities("please check cnet-marble status now", ents,
                                     CNET_DC_MAX_ENT);
    Tst(n >= 1 && strcmp(ents[0], "cnet-marble") == 0,
        "extract cnet-marble entity");

    n = cnet_dialog_extract_entities("restart cnet-web.service please", ents,
                                     CNET_DC_MAX_ENT);
    Tst(n >= 1, "extract cnet-web.service");

    cnet_dialog_ctx_update(&C, "cnet-marble status", "ops_cnet_marble",
                           "pack_ops_hermes_systemd", 1);
    Tst(C.n_entities >= 1 && strcmp(C.last_entities[0], "cnet-marble") == 0,
        "update stores entity");
    Tst(C.last_action == CNET_ACT_STATUS, "infer STATUS action");
    Tst(C.last_local == 1, "local flag");

    Tst(cnet_dialog_resolve(&C, "show me its status", out, sizeof out, &m) == 1,
        "anaphora status applied");
    Tst(m.applied == 1, "meta applied");
    Tst(strstr(out, "cnet-marble status") != NULL,
        "rewrites to cnet-marble status");

    Tst(cnet_dialog_resolve(&C, "restart it", out, sizeof out, &m) == 1,
        "anaphora restart applied");
    Tst(strstr(out, "systemctl --user restart") != NULL &&
            strstr(out, "cnet-marble") != NULL,
        "restart binds entity + systemctl pattern");

    Tst(cnet_dialog_resolve(&C, "do it again", out, sizeof out, &m) == 1,
        "repeat last");
    Tst(strcmp(out, "cnet-marble status") == 0, "repeat uses last_canonical");

    /* No soft seal without entities */
    {
        CnetDialogCtx empty;
        cnet_dialog_ctx_init(&empty);
        Tst(cnet_dialog_resolve(&empty, "restart it", out, sizeof out, &m) == 0,
            "no entity → no rewrite");
        Tst(strcmp(out, "restart it") == 0, "passthrough without entity");
    }

    /* Non-anaphora unchanged */
    Tst(cnet_dialog_resolve(&C, "who are you", out, sizeof out, &m) == 0,
        "non-anaphora no rewrite");

    /* Unknown open query never gains a fake entity seal phrase */
    cnet_dialog_ctx_init(&C);
    cnet_dialog_ctx_update(&C, "completely unknown domain xyzzy", "", "", 0);
    Tst(cnet_dialog_resolve(&C, "restart it", out, sizeof out, &m) == 0,
        "no unit entity from OOD → no restart rewrite");

#undef Tst
    printf("\nfailures=%d\n", fail);
    if (fail) {
        printf("DIALOG_CTX_FAIL\n");
        return 1;
    }
    printf("DIALOG_CTX_PASS\n");
    return 0;
}
