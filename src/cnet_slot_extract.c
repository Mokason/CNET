/* Ops/systemd fixed-grammar slot extractor — Milestone C. */
#include "../include/cnet_slot_extract.h"

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

static int is_unit_char(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '@';
}

/* Reject pronouns / stopwords as units. */
static int bad_unit(const char *u) {
    static const char *bad[] = {
        "it",       "that",   "this",     "them",    "its",     "the",
        "a",        "an",     "my",       "our",     "your",    "me",
        "you",      "we",     "they",     "and",     "or",      "of",
        "to",       "for",    "on",       "in",      "with",    "from",
        "active",   "running","status",   "restart", "start",   "stop",
        "service",  "unit",   "please",   "now",     "currently","user",
        "systemctl","check",  "show",     "get",     "is",      "if",
        NULL};
    int i;
    if (!u || !u[0] || strlen(u) < 2) return 1;
    for (i = 0; bad[i]; i++)
        if (strcmp(u, bad[i]) == 0) return 1;
    return 0;
}

/* Unit-ish: product tokens, dashed names, or .service/.target/.timer */
static int unit_ok(const char *u) {
    if (bad_unit(u)) return 0;
    if (strstr(u, "cnet") || strstr(u, "hermes") || strstr(u, "roe-") ||
        strstr(u, "marble"))
        return 1;
    if (strstr(u, ".service") || strstr(u, ".target") || strstr(u, ".timer") ||
        strstr(u, ".socket"))
        return 1;
    if (strchr(u, '-')) return 1;
    if (strlen(u) >= 4 && strlen(u) <= 48) {
        size_t i;
        int alpha = 0;
        for (i = 0; u[i]; i++) {
            if (!is_unit_char((unsigned char)u[i])) return 0;
            if (isalpha((unsigned char)u[i])) alpha = 1;
        }
        return alpha;
    }
    return 0;
}

static int is_marble_family(const char *u) {
    return u && (strcmp(u, "cnet-marble") == 0 ||
                 strcmp(u, "cnet-marble.target") == 0 ||
                 strcmp(u, "marble") == 0 ||
                 strstr(u, "cnet-marble") != NULL);
}

static int take_unit_at(const char *s, char *unit, size_t cap) {
    size_t i = 0;
    if (!s || !unit || !cap) return 0;
    while (s[i] && is_unit_char((unsigned char)s[i]) && i + 1 < cap) {
        unit[i] = (char)tolower((unsigned char)s[i]);
        i++;
    }
    unit[i] = 0;
    return unit_ok(unit) ? (int)i : 0;
}

static void fill_meta(CnetSlotMeta *m, CnetSlotAction act, const char *unit,
                      const char *reason) {
    if (!m) return;
    memset(m, 0, sizeof *m);
    m->applied = 1;
    m->action = act;
    scopy(m->unit, sizeof m->unit, unit);
    scopy(m->reason, sizeof m->reason, reason);
    switch (act) {
    case CNET_SLOT_ACT_STATUS:
        scopy(m->action_name, sizeof m->action_name, "status");
        break;
    case CNET_SLOT_ACT_RESTART:
        scopy(m->action_name, sizeof m->action_name, "restart");
        break;
    case CNET_SLOT_ACT_START:
        scopy(m->action_name, sizeof m->action_name, "start");
        break;
    case CNET_SLOT_ACT_STOP:
        scopy(m->action_name, sizeof m->action_name, "stop");
        break;
    default:
        scopy(m->action_name, sizeof m->action_name, "none");
        break;
    }
}

static int status_word(const char *rest) {
    if (!rest || !rest[0]) return 0;
    return strcmp(rest, "active") == 0 || strcmp(rest, "running") == 0 ||
           strcmp(rest, "up") == 0 || strcmp(rest, "alive") == 0 ||
           strcmp(rest, "online") == 0;
}

static int emit_rewrite(char *out, size_t cap, CnetSlotAction act,
                        const char *unit, CnetSlotMeta *meta,
                        const char *reason) {
    if (!out || !cap || !unit || !unit[0]) return 0;
    if (act == CNET_SLOT_ACT_STATUS && is_marble_family(unit)) {
        scopy(out, cap, "cnet-marble status");
        fill_meta(meta, act, unit, reason);
        return 1;
    }
    switch (act) {
    case CNET_SLOT_ACT_STATUS:
        snprintf(out, cap, "systemctl --user status %s", unit);
        break;
    case CNET_SLOT_ACT_RESTART:
        snprintf(out, cap, "systemctl --user restart %s", unit);
        break;
    case CNET_SLOT_ACT_START:
        snprintf(out, cap, "systemctl --user start %s", unit);
        break;
    case CNET_SLOT_ACT_STOP:
        snprintf(out, cap, "systemctl --user stop %s", unit);
        break;
    default:
        return 0;
    }
    fill_meta(meta, act, unit, reason);
    return 1;
}

static const char *after_prefix(const char *s, const char *prefix) {
    size_t n;
    if (!s || !prefix) return NULL;
    n = strlen(prefix);
    if (strncmp(s, prefix, n) != 0) return NULL;
    s += n;
    while (*s == ' ') s++;
    return *s ? s : NULL;
}

static int trailing_ok(const char *rest) {
    if (!rest) return 0;
    while (*rest == ' ') rest++;
    return !rest[0] || strcmp(rest, "please") == 0 || strcmp(rest, "now") == 0 ||
           strcmp(rest, "service") == 0 || strcmp(rest, "unit") == 0;
}

int cnet_slot_extract_ops(const char *in, char *out, size_t cap,
                          CnetSlotMeta *meta) {
    char unit[CNET_SLOT_UNIT];
    const char *p;
    int ulen;

    if (meta) memset(meta, 0, sizeof *meta);
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!in || !in[0]) return 0;
    scopy(out, cap, in);

    /* Already sealed forms — no rewrite */
    if (strncmp(in, "systemctl --user ", 17) == 0) return 0;
    if (strcmp(in, "cnet-marble status") == 0) return 0;
    if (strcmp(in, "systemctl --user") == 0) return 0;

    /* is <unit> [currently] active|running|up */
    p = after_prefix(in, "is ");
    if (p) {
        ulen = take_unit_at(p, unit, sizeof unit);
        if (ulen > 0) {
            const char *rest = p + ulen;
            while (*rest == ' ') rest++;
            if (strncmp(rest, "currently ", 10) == 0) {
                rest += 10;
                while (*rest == ' ') rest++;
            }
            if (status_word(rest))
                return emit_rewrite(out, cap, CNET_SLOT_ACT_STATUS, unit, meta,
                                    "is_unit_active");
        }
    }

    /* check if|whether <unit> is active|running */
    p = after_prefix(in, "check if ");
    if (!p) p = after_prefix(in, "check whether ");
    if (p) {
        ulen = take_unit_at(p, unit, sizeof unit);
        if (ulen > 0) {
            const char *rest = p + ulen;
            while (*rest == ' ') rest++;
            if (strncmp(rest, "is ", 3) == 0) {
                rest += 3;
                while (*rest == ' ') rest++;
                if (status_word(rest))
                    return emit_rewrite(out, cap, CNET_SLOT_ACT_STATUS, unit,
                                        meta, "check_if_unit");
            }
        }
    }

    /* status of <unit> */
    p = strstr(in, "status of ");
    if (p) {
        p += strlen("status of ");
        while (*p == ' ') p++;
        ulen = take_unit_at(p, unit, sizeof unit);
        if (ulen > 0 && trailing_ok(p + ulen))
            return emit_rewrite(out, cap, CNET_SLOT_ACT_STATUS, unit, meta,
                                "status_of_unit");
    }

    /* [show|check|get] <unit> status */
    {
        const char *suf = " status";
        size_t n = strlen(in);
        size_t sl = strlen(suf);
        if (n > sl && strcmp(in + n - sl, suf) == 0) {
            char head[CNET_SLOT_OUT];
            size_t hl = n - sl;
            if (hl + 1 < sizeof head) {
                memcpy(head, in, hl);
                head[hl] = 0;
                p = head;
                if (strncmp(p, "show ", 5) == 0)
                    p += 5;
                else if (strncmp(p, "check ", 6) == 0)
                    p += 6;
                else if (strncmp(p, "get ", 4) == 0)
                    p += 4;
                ulen = take_unit_at(p, unit, sizeof unit);
                if (ulen > 0 && (size_t)ulen == strlen(p))
                    return emit_rewrite(out, cap, CNET_SLOT_ACT_STATUS, unit,
                                        meta, "unit_status");
            }
        }
    }

    /* restart|reboot|start|stop [the] <unit> */
    {
        static const struct {
            const char *pref;
            CnetSlotAction act;
            const char *reason;
        } acts[] = {
            {"restart ", CNET_SLOT_ACT_RESTART, "restart_unit"},
            {"reboot ", CNET_SLOT_ACT_RESTART, "reboot_unit"},
            {"start ", CNET_SLOT_ACT_START, "start_unit"},
            {"stop ", CNET_SLOT_ACT_STOP, "stop_unit"},
        };
        size_t ai;
        for (ai = 0; ai < sizeof acts / sizeof acts[0]; ai++) {
            p = after_prefix(in, acts[ai].pref);
            if (!p) continue;
            if (strncmp(p, "the ", 4) == 0) p += 4;
            ulen = take_unit_at(p, unit, sizeof unit);
            if (ulen > 0 && trailing_ok(p + ulen))
                return emit_rewrite(out, cap, acts[ai].act, unit, meta,
                                    acts[ai].reason);
        }
    }

    return 0;
}

int cnet_slot_extract_selftest(void) {
    CnetSlotMeta m;
    char out[CNET_SLOT_OUT];
    int fail = 0;

#define Tst(ok, msg)                                                           \
    do {                                                                       \
        printf("  %-56s %s\n", msg, (ok) ? "PASS" : "FAIL");                   \
        if (!(ok)) fail++;                                                     \
    } while (0)

    printf("=== slot extract ops (Milestone C) ===\n");

    Tst(cnet_slot_extract_ops("is cnet-web active", out, sizeof out, &m) == 1,
        "is cnet-web active");
    Tst(m.applied && m.action == CNET_SLOT_ACT_STATUS, "meta status");
    Tst(strcmp(out, "systemctl --user status cnet-web") == 0,
        "→ systemctl --user status cnet-web");
    Tst(strcmp(m.unit, "cnet-web") == 0, "unit cnet-web");

    Tst(cnet_slot_extract_ops("is cnet-marble currently running", out,
                              sizeof out, &m) == 1,
        "is cnet-marble currently running");
    Tst(strcmp(out, "cnet-marble status") == 0, "marble → sealed cnet-marble status");

    Tst(cnet_slot_extract_ops("status of cnetd", out, sizeof out, &m) == 1,
        "status of cnetd");
    Tst(strcmp(out, "systemctl --user status cnetd") == 0, "status of → systemctl");

    Tst(cnet_slot_extract_ops("check if hermes-gateway is active", out,
                              sizeof out, &m) == 1,
        "check if hermes-gateway is active");
    Tst(strstr(out, "systemctl --user status hermes-gateway") != NULL,
        "check if → systemctl status");

    Tst(cnet_slot_extract_ops("restart cnet-web", out, sizeof out, &m) == 1,
        "restart cnet-web");
    Tst(strcmp(out, "systemctl --user restart cnet-web") == 0,
        "restart → systemctl restart");
    Tst(m.action == CNET_SLOT_ACT_RESTART, "action restart");

    Tst(cnet_slot_extract_ops("stop the cnetd.service please", out, sizeof out,
                              &m) == 1,
        "stop the cnetd.service please");
    Tst(strstr(out, "systemctl --user stop cnetd.service") != NULL, "stop unit");

    Tst(cnet_slot_extract_ops("cnet-web status", out, sizeof out, &m) == 1,
        "cnet-web status");
    Tst(strcmp(out, "systemctl --user status cnet-web") == 0, "unit status form");

    /* no soft invent */
    Tst(cnet_slot_extract_ops("is it active", out, sizeof out, &m) == 0,
        "pronoun it not a unit");
    Tst(cnet_slot_extract_ops("who are you", out, sizeof out, &m) == 0,
        "identity not slotted");
    Tst(cnet_slot_extract_ops("completely unknown domain xyzzy", out, sizeof out,
                              &m) == 0,
        "OOD no slot");
    Tst(cnet_slot_extract_ops("systemctl --user status cnetd", out, sizeof out,
                              &m) == 0,
        "already sealed passthrough");

    {
        int i;
        for (i = 0; i < 200; i++)
            (void)cnet_slot_extract_ops("is cnet-web active", out, sizeof out,
                                        &m);
        Tst(strcmp(out, "systemctl --user status cnet-web") == 0,
            "200 extracts stable");
    }

#undef Tst
    printf("\nfailures=%d\n", fail);
    if (fail) {
        printf("SLOT_EXTRACT_FAIL\n");
        return 1;
    }
    printf("SLOT_EXTRACT_PASS\n");
    return 0;
}
