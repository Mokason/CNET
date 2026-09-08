/* cnetd — warm UNIX-socket daemon for CNET front door (CERT-first).
 *
 * Keeps routes/probe table/curl initialized; serves line or JSON asks over
 * a user-local socket. Does NOT auto-CERT. Teacher only if env allows and
 * probe short-circuit does not match.
 *
 * Socket (first that works):
 *   $XDG_RUNTIME_DIR/cnet/cnet.sock
 *   ~/.local/share/cnet-minimal/run/cnet.sock
 *
 * Protocol (one request/response per connection, or line mode):
 *   ASK <query text>\n
 *   PEER <from> <query text>\n   — same as ASK; tags peer identity for dialog
 *   → multi-line status + ANSWER <text>\nEND\n
 *   PING\n → PONG\n
 *   QUIT\n → close
 *
 *   JSON: {"op":"ask","q":"..."}\n
 *   → {"ok":true,"source":"LOCAL","answer":"...","skill":"...","miss":false,...}\n
 *
 * Env:
 *   CNET_MINIMAL_ROOT, CNET_PACKS_ROOT, CNET_SOCK
 *
 * Peer path (Hermes ↔ Marble) uses this socket — not MCP. MCP is factory tools.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include "../include/cnet_platform.h"
#if CNET_HAVE_CURL
#include <curl/curl.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include "../include/cnet_dialog_ctx.h"
#include "../include/cnet_showrunner.h"
#include "../include/cnet_marble_live.h"
#include "../include/cnet_mcp_client.h"
#include "../include/cnet_mcp_read_brick.h"
#include "../include/cnet_json_escape.h"
#include "../include/cnet_domain_route.h"
#include "../include/cnet_probe_shortcircuit.h"
#include "../include/cnet_query_alias.h"
#include "../include/cnet_roe_asi.h"
#include "../include/cnet_slot_extract.h"
#include "../include/cnet_typed_en.h"
#include "../include/cnet_cert_solver.h"
#include "../include/cnet_ffi_convert.h"
#include "../include/cnet_roe_gold.h"
#include "../include/cnet_ood_skill.h"
#include "../include/cnet_chat_lookup.h"
#include "../include/cnet_utterance.h"
#include "../include/cnet_c_speak.h"
#include "../include/cnet_skill_lane.h"
#include "../include/cnet_capsule_loop.h"
#include "../include/cnet_hemisphere.h"
#include "../include/cnet_brain_mirror.h"
#include "../include/cnet_rlm.h"
#include "../include/cnet_core_serve.h"
#include "../include/cnet_live_miss.h"
#include "../include/cnet_md_memory.h"
#include "../include/cnetd_protocol.h"
#include "../include/cnet_capsule_control.h"
#include "../include/cnet_capsule_evidence.h"
#include <sys/wait.h>

#define CD_PATH 512
#define CD_SOCK 108
#define CD_MAX_ALWAYS 12
#define CD_MAX_PACKS 8
#define CD_PAT 256
#define CD_ID 64
#define CD_CLIENT_READ_TIMEOUT_MS 5000
#define CD_CLIENT_READ_TIMEOUT_MIN_MS 50
#define CD_CLIENT_READ_TIMEOUT_MAX_MS 60000

static volatile sig_atomic_t g_stop = 0;

static int client_read_timeout_ms(void) {
    const char *configured = getenv("CNETD_CLIENT_READ_TIMEOUT_MS");
    char *end = NULL;
    long value;
    if (!configured || !configured[0]) return CD_CLIENT_READ_TIMEOUT_MS;
    errno = 0;
    value = strtol(configured, &end, 10);
    if (errno || !end || *end != '\0') return CD_CLIENT_READ_TIMEOUT_MS;
    if (value < CD_CLIENT_READ_TIMEOUT_MIN_MS)
        return CD_CLIENT_READ_TIMEOUT_MIN_MS;
    if (value > CD_CLIENT_READ_TIMEOUT_MAX_MS)
        return CD_CLIENT_READ_TIMEOUT_MAX_MS;
    return (int)value;
}

static void on_sig(int s) {
    (void)s;
    g_stop = 1;
}

/* Install a stop handler that does NOT restart interrupted syscalls.
 *
 * signal() carries SA_RESTART on glibc, which silently restarts the blocked
 * accept() in the serve loop: g_stop gets set, the loop condition is never
 * re-tested, and the daemon sleeps in accept() until some client happens to
 * connect. Callers doing "kill $PID; wait $PID" then block for their whole
 * timeout. sigaction() with an empty flags field gives us EINTR instead, which
 * the serve loop already handles correctly. */
static int install_stop_handler(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* deliberately no SA_RESTART */
    return sigaction(sig, &sa, NULL);
}

typedef struct {
    char pattern[CD_PAT];
    char pack[CD_ID];
} CdRoute;

typedef struct {
    char root[CD_PATH];
    char miss_log[CD_PATH];
    char always_on[CD_MAX_ALWAYS][CD_ID];
    int n_always;
    CdRoute routes[512];
    int n_routes;
    CnetProbeTable probes;
    CnetDomainRouter domain;
    CnetQueryAliasTable aliases;
    CnetDialogCtx dialog; /* process-local warm session (single-user cnetd) */
    CnetRlmSession rlm_sess; /* multi-turn CERT hop memory (never residual CERT) */
    CnetShowrunner show;     /* Marble Live: mood, cooldown, episodic notes */
    CnetFfiConv ffi;         /* residual FFI converter; never admits */
    char last_miss_q[256];   /* last organic miss; gold last uses this */
    char remind_path[CD_PATH];
    char gpt_sol_jobs[CD_PATH]; /* leftover enqueue for gpt-sol improver */
    CnetCapsuleStore *capsule_store;
    CnetCoreHost *capsule_host;
    int capsule_configured;
    int ready;
} CdState;

static int mkdir_p(const char *path) {
    char tmp[CD_PATH];
    size_t len, i;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int path_join2(char *out, size_t cap, const char *a, const char *b) {
    int n;
    if (!out || !cap || !a || !b) return -1;
    n = snprintf(out, cap, "%s/%s", a, b);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static int contains_ci(const char *hay, const char *needle) {
    char h[512], n[256];
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

static int kb_word_char(unsigned char c) {
    return (int)(isalnum(c) || c == '-');
}

/* Whole-word match. "like" must not hit "likely". */
static int contains_word_ci(const char *hay, const char *needle) {
    size_t nlen, i, j;
    if (!hay || !needle || !needle[0]) return 0;
    nlen = strlen(needle);
    for (i = 0; hay[i]; i++) {
        if (i > 0 && kb_word_char((unsigned char)hay[i - 1])) continue;
        for (j = 0; j < nlen; j++) {
            if (!hay[i + j]) break;
            if (tolower((unsigned char)hay[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nlen && !kb_word_char((unsigned char)hay[i + nlen]))
            return 1;
    }
    return 0;
}

static void cd_scopy(char *d, size_t cap, const char *s) {
    size_t i;
    if (!d || !cap) return;
    if (!s) {
        d[0] = 0;
        return;
    }
    for (i = 0; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static int json_get_str(const char *line, const char *key, char *out, size_t cap) {
    char pat[80];
    const char *p;
    size_t i = 0;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    p = strstr(line, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (p[i] && p[i] != '"' && i + 1 < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
    return out[0] ? 0 : -1;
}

static void resolve_paths(char *root_out, size_t rcap, char *sock_out, size_t scap) {
    const char *env_root = getenv("CNET_PACKS_ROOT");
    const char *min = getenv("CNET_MINIMAL_ROOT");
    const char *sock = getenv("CNET_SOCK");
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    const char *home = getenv("HOME");
    char tmp[CD_PATH];

    if (env_root && env_root[0])
        snprintf(root_out, rcap, "%s", env_root);
    else if (min && min[0])
        snprintf(root_out, rcap, "%s/data/roe_daily_packs", min);
    else {
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : ".";
        }
        snprintf(root_out, rcap, "%s/AI/CNET/artifacts/roe_daily_packs", home);
    }

    if (sock && sock[0]) {
        snprintf(sock_out, scap, "%s", sock);
        return;
    }
    if (xdg && xdg[0] && strlen(xdg) + 16 < scap) {
        snprintf(tmp, sizeof tmp, "%s/cnet", xdg);
        if (mkdir_p(tmp) == 0) {
            snprintf(sock_out, scap, "%s/cnet.sock", tmp);
            return;
        }
    }
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : ".";
    }
    snprintf(tmp, sizeof tmp, "%s/.local/share/cnet-minimal/run", home);
    mkdir_p(tmp);
    if (strlen(tmp) + 11 < scap)
        snprintf(sock_out, scap, "%s/cnet.sock", tmp);
    else
        snprintf(sock_out, scap, "/tmp/cnet-%d.sock", (int)getuid());
}

static int cd_init(CdState *S, const char *root) {
    char path[CD_PATH], line[1024];
    FILE *f;
    if (!S) return -1;
    memset(S, 0, sizeof *S);
    snprintf(S->root, sizeof S->root, "%s", root);
    if (path_join2(S->miss_log, sizeof S->miss_log, S->root, "miss_log.jsonl") != 0)
        return -1;
    if (path_join2(S->remind_path, sizeof S->remind_path, S->root,
                   "marble_reminders.txt") != 0)
        return -1;
    {
        const char *minj = getenv("CNET_MINIMAL_ROOT");
        char vdir[CD_PATH];
        S->gpt_sol_jobs[0] = 0;
        if (minj && minj[0]) {
            if (path_join2(vdir, sizeof vdir, minj, "var") == 0) {
                mkdir_p(vdir);
                if (path_join2(S->gpt_sol_jobs, sizeof S->gpt_sol_jobs, vdir,
                               "gpt_sol_jobs.jsonl") != 0)
                    S->gpt_sol_jobs[0] = 0;
            }
        } else if (path_join2(S->gpt_sol_jobs, sizeof S->gpt_sol_jobs, S->root,
                              "gpt_sol_jobs.jsonl") != 0) {
            S->gpt_sol_jobs[0] = 0;
        }
    }
    snprintf(S->always_on[0], sizeof S->always_on[0], "%s", "pack_soul_marble");
    snprintf(S->always_on[1], sizeof S->always_on[1], "%s", "pack_roe_self");
    snprintf(S->always_on[2], sizeof S->always_on[2], "%s", "pack_goal_split");
    snprintf(S->always_on[3], sizeof S->always_on[3], "%s", "pack_toolcall_hermes");
    snprintf(S->always_on[4], sizeof S->always_on[4], "%s", "pack_personal");
    snprintf(S->always_on[5], sizeof S->always_on[5], "%s", "pack_ops_hermes_systemd");
    snprintf(S->always_on[6], sizeof S->always_on[6], "%s", "pack_english_basic");
    S->n_always = 7;

    if (path_join2(path, sizeof path, S->root, "ROUTES.jsonl") != 0) return -1;
    f = fopen(path, "r");
    if (!f) return -2;
    while (fgets(line, sizeof line, f) && S->n_routes < 512) {
        CdRoute *r = &S->routes[S->n_routes];
        memset(r, 0, sizeof *r);
        if (json_get_str(line, "pattern", r->pattern, sizeof r->pattern) != 0) continue;
        if (json_get_str(line, "pack", r->pack, sizeof r->pack) != 0) continue;
        S->n_routes++;
    }
    fclose(f);

    cnet_probe_table_init(&S->probes);
    (void)cnet_probe_table_load(&S->probes, "config/probe_shortcircuit.txt");
    {
        const char *min = getenv("CNET_MINIMAL_ROOT");
        char p2[CD_PATH];
        if (min && min[0]) {
            snprintf(p2, sizeof p2, "%s/config/probe_shortcircuit.txt", min);
            (void)cnet_probe_table_load(&S->probes, p2);
        }
    }
    cnet_domain_route_init(&S->domain);
    (void)cnet_domain_route_load_file(&S->domain, "config/domain_routes.tsv");
    cnet_query_alias_init(&S->aliases);
    (void)cnet_query_alias_load_file(&S->aliases, "config/query_aliases.tsv");
    cnet_dialog_ctx_init(&S->dialog);
    cnet_rlm_session_init(&S->rlm_sess);
    cnet_sr_init(&S->show);
    {
        const char *min = getenv("CNET_MINIMAL_ROOT");
        char ap[CD_PATH];
        if (min && min[0]) {
            if (path_join2(ap, sizeof ap, min, "marble_affect.txt") == 0)
                cnet_sr_set_affect_path(&S->show, ap);
        } else if (path_join2(ap, sizeof ap, S->root, "marble_affect.txt") == 0) {
            cnet_sr_set_affect_path(&S->show, ap);
        }
    }
    cnet_ffi_init_ports(&S->ffi);
    {
        char ov[CD_PATH];
        cnet_te_init();
        if (path_join2(ov, sizeof ov, S->root, "en_irregular_gold.tsv") == 0)
            (void)cnet_te_load_tsv(ov);
        (void)cnet_ood_load_numerals("config/en_numerals.tsv");
        if (path_join2(ov, sizeof ov, S->root, "en_numerals.tsv") == 0)
            (void)cnet_ood_load_numerals(ov);
    }
    {
        /* Durable episodic notes: CNET_EPISODIC_PATH, else
         * $CNET_MINIMAL_ROOT/var/marble_episodic.jsonl. Memory is warm
         * context only -- it never raises a floor. */
        char ep[CNET_ML_PATH];
        if (cnet_ml_episodic_path(ep, sizeof ep, S->root) == 0)
            cnet_sr_set_episodic_path(&S->show, ep);
    }
    {
        const char *min = getenv("CNET_MINIMAL_ROOT");
        char p2[CD_PATH];
        if (min && min[0]) {
            snprintf(p2, sizeof p2, "%s/config/domain_routes.tsv", min);
            (void)cnet_domain_route_load_file(&S->domain, p2);
            snprintf(p2, sizeof p2, "%s/config/query_aliases.tsv", min);
            (void)cnet_query_alias_load_file(&S->aliases, p2);
        }
    }
    S->ready = 1;
    return S->n_routes > 0 ? 0 : -3;
}

static int cd_match_route(const CdState *S, const char *q, CdRoute *out) {
    int best = -1;
    size_t best_len = 0;
    int i;
    for (i = 0; i < S->n_routes; i++) {
        size_t L = strlen(S->routes[i].pattern);
        if (L < 3) continue;
        if (contains_ci(q, S->routes[i].pattern) && L > best_len) {
            best_len = L;
            best = i;
        }
    }
    if (best >= 0 && out) *out = S->routes[best];
    return best;
}

static int load_pack(RoeAsi *R, const char *root, const char *pack) {
    char path[CD_PATH];
    if (path_join2(path, sizeof path, root, pack) != 0) return -1;
    roe_set_catalog_dir(R, path);
    return roe_load_catalog(R);
}

typedef struct {
    char source[32];
    char skill[64];
    char answer[ROE_ANSWER_MAX];
    char utterance[CNET_UTTER_TEXT];
    char domain[32];
    char probe_pat[96];
    char prepared[CNET_QA_OUT];
    char alias_pat[CNET_QA_PAT];
    char dialog_reason[48];
    char dialog_entity[CNET_DC_ENT];
    char slot_unit[CNET_SLOT_UNIT];
    char slot_reason[48];
    char peer[CNET_DC_ID];   /* who asked; informational, never a routing input */
    char action[32];         /* allowlisted chat action that handled the turn */
    char stage[CNET_SR_TEXT];/* residual improv draft; NEVER the CERT answer */
    int  stage_draft;        /* 1 if `stage` holds a residual draft */
    int alias_hit;
    int dialog_hit;
    int slot_hit;
    int typed_en_hit;
    int arith_hit;
    int brick_hit;
    int solver_hit;
    int miss;
    int shortcircuit;
    int verified;
    int may_voice;
    int capsule_handled;    /* terminal authority/refusal; no later fallback */
    unsigned long long tokens;
} CdReply;


static void load_neuromod_into(CnetUtterState *U) {
    FILE *f;
    char buf[2048];
    size_t n;
    const char *paths[] = {
        "logs/governor/neuromod_state.json",
        "logs/marble_24_7/AUTONOMOUS_CYCLE.json",
        NULL};
    int i;
    if (!U) return;
    for (i = 0; paths[i]; i++) {
        f = fopen(paths[i], "r");
        if (!f) continue;
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        if (!n) continue;
        buf[n] = 0;
        {
            const char *p;
            p = strstr(buf, "\"dopamine\"");
            if (p) {
                p = strchr(p, ':');
                if (p) U->dopamine = strtod(p + 1, NULL);
            }
            p = strstr(buf, "\"serotonin\"");
            if (p) {
                p = strchr(p, ':');
                if (p) U->serotonin = strtod(p + 1, NULL);
            }
            p = strstr(buf, "\"adenosine\"");
            if (p) {
                p = strchr(p, ':');
                if (p) U->adenosine = strtod(p + 1, NULL);
            }
            p = strstr(buf, "\"local_hit\"");
            if (p) {
                p = strchr(p, ':');
                if (p) U->local_hit = strtod(p + 1, NULL);
            }
        }
        break;
    }
}


/* G1: does an always-on CERT pack already own this query?
 *
 * cd_ask allocates its RoeAsi and loads catalogs LATE, well after the RLM /
 * CORE planes have had their turn. So when CNET_CORE_OPEN_CHAT is on, the
 * generative plane could bind "who are you" and "would you kill all humans"
 * and return before the sealed soul skills were ever consulted -- identity and
 * refusal answered by residual boilerplate instead of soul_who / soul_not_kill.
 * The law held (miss=1, claimed_cert=0, never LOCAL) but the identity guarantee
 * did not, which is the whole point of a soul pack.
 *
 * Rather than reorder cd_ask, probe the always-on pack catalogs straight off
 * disk with the SAME matcher the engine uses (contains_ci on the pattern), and
 * let the open-chat plane decline when a sealed pack already owns the turn.
 * Always-on packs are CERT packs by construction (pack_soul_marble carries
 * never_self_cert 1 / seal_path forbidden), so a catalog hit is a sealed hit.
 *
 * Cost is a few small catalog reads, and only on the open-chat miss path.
 */
static int cd_social_pattern_allowed(const char *q, const char *pattern, const char *id) {
    char norm[CNET_QA_OUT], norm_id[CNET_QA_OUT];
    cnet_query_normalize(pattern, norm, sizeof norm);
    cnet_query_normalize(id, norm_id, sizeof norm_id);
    if (!strcmp(norm, "who are you") || !strcmp(norm, "still here") ||
        !strcmp(norm, "who is your operator") || !strcmp(norm_id, "soul_who") ||
        !strcmp(norm_id, "soul_ack") || !strcmp(norm_id, "soul_operator"))
        return cnet_query_phrase_is_whole(q, norm);
    return 1;
}

static int cd_sealed_pack_match(const CdState *S, const char *q, char *id_out,
                                size_t id_cap) {
    char path[CD_PATH];
    char line[4096];
    char pat[512], id[128];
    int i;
    if (id_out && id_cap) id_out[0] = 0;
    if (!S || !q || !q[0]) return 0;
    for (i = 0; i < S->n_always; i++) {
        FILE *f;
        if (!S->always_on[i][0]) continue;
        if (snprintf(path, sizeof path, "%s/%s/catalog.jsonl", S->root,
                     S->always_on[i]) >= (int)sizeof path)
            continue;
        f = fopen(path, "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            if (json_get_str(line, "pattern", pat, sizeof pat) != 0) continue;
            if (!pat[0] || !contains_ci(q, pat)) continue;
            if (json_get_str(line, "id", id, sizeof id) != 0) id[0] = 0;
            if (!cd_social_pattern_allowed(q, pat, id)) continue;
            if (id_out && id_cap)
                snprintf(id_out, id_cap, "%s", id[0] ? id : S->always_on[i]);
            fclose(f);
            return 1;
        }
        fclose(f);
    }
    return 0;
}

/* TAG + digits (nibble may be >15). Brick-shaped; leftover must not speak. */
static int cd_lut_shaped(const char *q) {
    const char *p = q;
    int und = 0, digits = 0;
    if (!p) return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return 0;
    while (*p && *p != ' ' && *p != '\t') {
        if (*p == '_') und = 1;
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) {
        digits = 1;
        p++;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return und && digits && *p == '\0';
}

/* Probe, ops slot, or always-on CERT pack already owns this turn.
 * OPEN_CHAT leftover must not bind first. */
static int cd_front_owns(const CdState *S, const char *q) {
    char pbuf[96], nrm[CNET_QA_OUT], sout[CNET_SLOT_OUT];
    CnetSlotMeta sm;
    if (!S || !q || !q[0]) return 0;
    if (cnet_probe_match(&S->probes, q, pbuf, sizeof pbuf)) return 1;
    {
        int aop = 0;
        if (cnet_ood_arith_shaped(q, &aop)) return 1;
    }
    if (cnet_cert_solver_shaped(q)) return 1;
    {
        CnetTeResult te;
        if (cnet_te_ask(q, &te) == 0 && te.grammar_hit) return 1;
    }
    {
        CnetServeBank *sb = cnet_serve_global();
        if (sb && cnet_serve_owns(sb, q)) return 1;
    }
    if (cd_lut_shaped(q)) return 1;
    if (cd_sealed_pack_match(S, q, NULL, 0)) return 1;
    memset(&sm, 0, sizeof sm);
    cnet_query_normalize(q, nrm, sizeof nrm);
    if (cnet_query_identity_bot(nrm)) return 1;
    {
        char tmp[CNET_QA_OUT];
        CnetQueryPrepareMeta am;
        memset(&am, 0, sizeof am);
        cnet_query_prepare(&S->aliases, q, tmp, sizeof tmp, &am);
        if (am.alias_hit) return 1;
    }
    if (cnet_slot_extract_ops(nrm, sout, sizeof sout, &sm) && sm.applied) return 1;
    return 0;
}

static int cd_player_sku(void) {
    const char *e = getenv("CNET_PLAYER_SKU");
    return e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y');
}

static int cd_explicit_lookup(const char *q) {
    if (!q) return 0;
    return strncmp(q, "look up ", 8) == 0 || strncmp(q, "lookup ", 7) == 0 ||
           strncmp(q, "wiki ", 5) == 0;
}

/* 1 = do not call RLM/MCP (law-door teachers). Leftover must be fail-fast
 * (<5ms). STAGE is NOT under this gate: the play door has its own bouncer,
 * cd_play_may_speak(), which is the inverse of this one. */
static int cd_skip_teacher(const CdState *S, const char *q) {
    int aop = 0;
    if (!q || !q[0]) return 1;
    if (cd_front_owns(S, q)) return 0;
    if (cnet_ood_arith_shaped(q, &aop)) return 0;
    if (cd_explicit_lookup(q) && !cd_player_sku()) return 0;
    return 1;
}

static unsigned g_waist_misses;

static void core_serve_try_reload(void) {
    CnetServeBank *b = cnet_serve_global();
    if (b && b->dir[0])
        (void)cnet_serve_bank_reload(b);
    else
        (void)cnet_serve_global_load_env();
}

/* Evolve worker. sync=1 waits for completion then reloads .lut bank. */
static int core_evolve_run(CdState *S, int sync) {
    const char *en = getenv("CNET_CORE_AUTO_EVOLVE");
    int reaped;
    const char *bin = getenv("CNET_CORE_EVOLVE_BIN");
    const char *dir = getenv("CNET_CORE_BUS_BRICKS_DIR");
    pid_t pid;
    if (!dir || !dir[0]) return -1;
    /* goals/teach always allowed; periodic needs AUTO_EVOLVE */
    if (!sync && (!en || en[0] != '1')) return -1;
    if (S && S->miss_log[0])
        setenv("CNET_MISS_LOG", S->miss_log, 1);
    setenv("CNET_CORE_BUS_BRICKS_DIR", dir, 1);
    if (!getenv("CNET_CORE_EVOLVE_FACTORY"))
        setenv("CNET_CORE_EVOLVE_FACTORY", "0", 0);
    /* Reap any finished async evolve children first. The async branch below
     * polls with WNOHANG and never comes back, so without this sweep every
     * periodic evolve leaks a zombie for the life of the daemon. */
    do {
        int zst = 0;
        reaped = (int)waitpid(-1, &zst, WNOHANG);
    } while (reaped > 0);
    pid = fork();
    if (pid == 0) {
        const char *path = bin && bin[0] ? bin : "bin/cnet_core_evolve";
        execl(path, path, "--once", (char *)NULL);
        execlp("cnet_core_evolve", "cnet_core_evolve", "--once", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int st = 0;
        if (sync)
            (void)waitpid(pid, &st, 0);
        else
            (void)waitpid(pid, &st, WNOHANG);
        core_serve_try_reload();
        return 0;
    }
    return -1;
}

static void core_evolve_tick(CdState *S) {
    const char *en = getenv("CNET_CORE_AUTO_EVOLVE");
    const char *every_s = getenv("CNET_CORE_EVOLVE_EVERY");
    unsigned every = 4;
    if (!en || en[0] != '1') return;
    if (every_s && every_s[0]) every = (unsigned)atoi(every_s);
    if (every == 0) every = 1;
    g_waist_misses++;
    if (g_waist_misses % every != 0) return;
    (void)core_evolve_run(S, 0);
}

static int core_try_serve(const char *q, CdReply *out) {
    CnetServeBank *sb = cnet_serve_global();
    CnetServeResult sr;
    if (!sb || !out) return -1;
    if (cnet_serve_result(sb, q, &sr) != 0 || !sr.proved) return -1;
    snprintf(out->answer, sizeof out->answer, "%.2047s", sr.spoken);
    snprintf(out->utterance, sizeof out->utterance, "%.767s", sr.spoken);
    snprintf(out->source, sizeof out->source, "LOCAL");
    snprintf(out->skill, sizeof out->skill, "%.63s",
             sr.brick[0] ? sr.brick : "core_brick");
    out->verified = 1;
    out->miss = 0;
    out->may_voice = 1;
    out->tokens = 0;
    return 0;
}

static int cd_brick_shaped_ask(const char *q, CdReply *out) {
    if (!cd_lut_shaped(q) || !out) return -1;
    core_serve_try_reload();
    cd_scopy(out->prepared, sizeof out->prepared, q);
    if (core_try_serve(q, out) == 0)
        return 0;
    snprintf(out->answer, sizeof out->answer, "outside_table_abstain");
    snprintf(out->utterance, sizeof out->utterance, "outside_table_abstain");
    snprintf(out->source, sizeof out->source, "CNET");
    snprintf(out->skill, sizeof out->skill, "core_brick");
    snprintf(out->domain, sizeof out->domain, "ABSTAIN");
    out->verified = 0;
    out->miss = 1;
    out->may_voice = 0;
    out->brick_hit = 1;
    out->tokens = 0;
    return 0;
}

/* ---- multi-hit sealed composer (O2) ----------------------------------
 *
 * roe_turn returns ONE skill. A question that names two things a sealed pack
 * both knows -- "what is the past of go and the past of see" -- came back with
 * only the `go` answer, stamped CLAIMED_CERT 1. A complete-looking certified
 * answer that covers half the question is a CERT-integrity problem: the reader
 * has no way to see that the other half was dropped.
 *
 * So when several sealed skills match DISJOINT parts of the query, emit all of
 * them in query order.
 *
 * This does not lower the floor. Every sentence emitted is verbatim from a
 * skill that is already `certified` and `active` in the loaded catalog -- there
 * is no generation, no paraphrase, no joining of facts into a new claim. It is
 * retrieval of several certified units instead of one, so CLAIMED_CERT stays
 * earned. Arguably it raises the floor, because the alternative was silently
 * answering a third of the question with full confidence.
 *
 * Disjointness is what keeps it honest. "a or an" and "an" both match the same
 * span, so composing them would just repeat; only the longest match at a given
 * position survives. Fewer than two surviving spans means nothing changes and
 * the single-hit path behaves exactly as before.
 */
#define CD_COMPOSE_MAX 3

static long cd_find_ci(const char *hay, const char *needle) {
    size_t nl = needle ? strlen(needle) : 0;
    size_t i, j;
    if (!hay || nl == 0) return -1;
    for (i = 0; hay[i]; i++) {
        for (j = 0; j < nl; j++) {
            if (!hay[i + j]) return -1;
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nl) return (long)i;
    }
    return -1;
}

typedef struct {
    long off;
    size_t len;
    const char *answer;
    const char *id;
} CdSpan;

/* Disjoint sealed matches for q: longest-first overlap resolution, then sorted
 * into query order. Shared by the composer and the coverage check. */
static int cd_sealed_spans(const RoeAsi *R, const char *q, CdSpan *keep, int maxk) {
    CdSpan cand[64];
    int ncand = 0, nkeep = 0, i, j;
    if (!R || !q || !keep || maxk <= 0) return 0;

    for (i = 0; i < (int)R->n_skills && ncand < 64; i++) {
        const RoeSkill *sk = &R->skills[i];
        long off;
        if (!sk->active || !sk->certified) continue;
        if (!sk->pattern[0] || !sk->answer[0]) continue;
        off = cd_find_ci(q, sk->pattern);
        if (off < 0) continue;
        cand[ncand].off = off;
        cand[ncand].len = strlen(sk->pattern);
        cand[ncand].answer = sk->answer;
        cand[ncand].id = sk->id;
        ncand++;
    }
    if (ncand < 1) return 0;

    /* Longest-first, accept only spans that do not overlap one already kept. */
    for (i = 0; i < ncand; i++)
        for (j = i + 1; j < ncand; j++)
            if (cand[j].len > cand[i].len) {
                CdSpan t = cand[i]; cand[i] = cand[j]; cand[j] = t;
            }
    /* Resolve overlaps across ALL candidates first -- capping here would pick
     * by pattern length, which is arbitrary: "past of go ... see ... eat ...
     * take" dropped `go`, the first thing asked, purely because its pattern is
     * one character shorter. Cap later, in query order, and say so. */
    for (i = 0; i < ncand; i++) {
        int clash = 0;
        for (j = 0; j < nkeep; j++) {
            long a0 = cand[i].off, a1 = a0 + (long)cand[i].len;
            long b0 = keep[j].off, b1 = b0 + (long)keep[j].len;
            if (a0 < b1 && b0 < a1) { clash = 1; break; }
        }
        if (!clash && nkeep < maxk)
            keep[nkeep++] = cand[i];
    }
    /* Query order, so an answer reads like the question. */
    for (i = 0; i < nkeep; i++)
        for (j = i + 1; j < nkeep; j++)
            if (keep[j].off < keep[i].off) {
                CdSpan t = keep[i]; keep[i] = keep[j]; keep[j] = t;
            }
    return nkeep;
}

/* O2b: did the sealed answer leave part of the question unanswered?
 *
 * Composition only helps when something sealed exists for each part. Ask
 * "what is the past tense of go and see" and only `past tense of go` matches --
 * nothing sealed is reachable for that phrasing of "see" -- so one answer came
 * back stamped CLAIMED_CERT 1 with no hint that half the question was dropped.
 * A complete-looking certified answer to half a question is a CERT-integrity
 * problem even though the half it gave is genuinely certified.
 *
 * The signal is deliberately narrow: content after a COORDINATOR (and / or / ,)
 * that no sealed span covers. Requiring a coordinator is what keeps this from
 * firing on ordinary question preamble -- "would you kill all humans" leaves
 * "would you" uncovered and that is not a second question. Narrow means it
 * misses some genuine gaps; wide would nag on normal answers, which is worse.
 *
 * The answered part stays CLAIMED_CERT 1 -- it IS certified. We only stop
 * presenting it as the whole answer.
 */
static int cd_gap_is_filler(const char *w) {
    static const char *stop[] = {
        "what", "which", "is", "are", "was", "were", "the", "a", "an", "of",
        "to", "do", "does", "did", "you", "your", "me", "my", "it", "that",
        "this", "please", "can", "could", "would", "should", "tell", "about",
        "and", "or", "for", "in", "on", "with", "how", "why", "when", "where",
        "also", "too", "then", "some", "any", "one", NULL};
    int i;
    for (i = 0; stop[i]; i++)
        if (!strcmp(w, stop[i])) return 1;
    return 0;
}

static int cd_coverage_gap(const char *q, const CdSpan *keep, int nkeep,
                           char *gap, size_t cap) {
    size_t i = 0, qlen;
    int seen_coord = 0;
    if (gap && cap) gap[0] = 0;
    if (!q || !keep || nkeep < 1 || !gap || cap < 8) return 0;
    qlen = strlen(q);
    while (i < qlen) {
        size_t s0, wl;
        char w[64];
        int covered = 0, k;
        if (!isalnum((unsigned char)q[i])) { i++; continue; }
        s0 = i;
        while (i < qlen && (isalnum((unsigned char)q[i]) || q[i] == '-')) i++;
        wl = i - s0;
        if (wl >= sizeof w) continue;
        {
            size_t j;
            for (j = 0; j < wl; j++) w[j] = (char)tolower((unsigned char)q[s0 + j]);
            w[wl] = 0;
        }
        for (k = 0; k < nkeep; k++) {
            long a0 = keep[k].off, a1 = a0 + (long)keep[k].len;
            if ((long)s0 >= a0 && (long)(s0 + wl) <= a1) { covered = 1; break; }
        }
        if (!strcmp(w, "and") || !strcmp(w, "or")) {
            if (!covered) seen_coord = 1;
            continue;
        }
        if (covered) continue;
        if (!seen_coord) continue;          /* preamble, not a second question */
        if (cd_gap_is_filler(w)) continue;
        snprintf(gap, cap, "%s", w);
        return 1;
    }
    return 0;
}

static int cd_compose_sealed(const RoeAsi *R, const char *q, char *out, size_t cap,
                             char *ids, size_t ids_cap) {
    CdSpan keep[16];
    int nkeep, i;
    size_t o = 0, io = 0;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (ids && ids_cap) ids[0] = 0;
    nkeep = cd_sealed_spans(R, q, keep, (int)(sizeof keep / sizeof keep[0]));
    if (nkeep < 2) return 0;
    {
        int emit = nkeep > CD_COMPOSE_MAX ? CD_COMPOSE_MAX : nkeep;
        for (i = 0; i < emit; i++) {
            if (o + 4 >= cap) break;
            o += (size_t)snprintf(out + o, cap - o, "%s%s", i ? "  " : "",
                                  keep[i].answer);
            if (ids && ids_cap && io + 2 < ids_cap)
                io += (size_t)snprintf(ids + io, ids_cap - io, "%s%s", i ? "+" : "",
                                       keep[i].id);
        }
        /* Never let a truncated composition read as a complete answer -- that is
         * the very defect this function exists to remove. Disclosing that we
         * held some back is a statement about retrieval, not a claim about the
         * world, so the answer stays verbatim-sealed plus this marker. */
        if (nkeep > emit && o + 64 < cap)
            o += (size_t)snprintf(out + o, cap - o,
                                  "  (+%d more sealed %s matched this question; "
                                  "ask %s separately)",
                                  nkeep - emit,
                                  (nkeep - emit) == 1 ? "answer" : "answers",
                                  (nkeep - emit) == 1 ? "it" : "them");
        return emit;
    }
}

/* Defined below with the rest of the knowledge-base helpers; cd_ask needs them
 * early so ingested notes can outrank the residual open-chat plane. */
static int kb_recall_overlap(const char *q, char *out, size_t cap);
static int kb_note_hit(CdState *S, const char *q, const char *notes);
static void cd_check_citations(char *s, const char *ctx, int n_chunks);

#include "cnet_capsule_core.h"
#include "cnet_semantic_cortex.h"

static int cd_ask(CdState *S, const char *q, CdReply *out) {
    RoeAsi *R = NULL;
    RoeNet net;
    RoeReply rep;
    CdRoute rt;
    CnetDomainDecision dd;
    CnetQueryPrepareMeta ameta;
    CnetDialogResolveMeta dmeta;
    CnetSlotMeta smeta;
    char ppat[96];
    char q_prep[CNET_QA_OUT];
    char q_slot[CNET_SLOT_OUT];
    char q_final[CNET_QA_OUT];
    const char *pm;
    const char *q_use;
    int i, matched;
    int typed_handled = 0;

    if (!S || !q || !out || !S->ready) return -1;
    memset(out, 0, sizeof *out);
    memset(&rt, 0, sizeof rt);

    /* Explicit typed intent belongs to the capsule authority, including a
       refusal. Residual prose must not answer over a failed capsule guard. */
    char capsule_intent[160];
    int table_query = (!strncmp(q,"data",4) && (!q[4] || isspace((unsigned char)q[4]))) ||
        (!strncmp(q,"symbol",6) && (!q[6] || isspace((unsigned char)q[6])));
    int intent_status = S->capsule_configured && !table_query ?
        cnet_semantic_capsule_intent(q, capsule_intent, sizeof capsule_intent) : 0;
    int source_fact = !strncmp(q,"source-fact ",12) || !strcmp(q,"source-fact");
    if(source_fact) {
        const char *name = strlen(q)>12 ? q+12 : "";
        intent_status = cnet_capsule_evidence_request(name,capsule_intent,sizeof capsule_intent) ? -2 : 1;
    }
    if (intent_status < 0) {
        out->capsule_handled = 1;
        cd_scopy(out->source, sizeof out->source, "CNET");
        cd_scopy(out->skill, sizeof out->skill, source_fact ? "capsule_refusal" : "capsule_clarify");
        cd_scopy(out->answer, sizeof out->answer, source_fact ? "ABSTAIN: unsupported_source_fact" :
            "CLARIFY: specify one whole-number value, input tag and output tag: convert N INPUT_TAG to OUTPUT_TAG");
        cd_scopy(out->utterance, sizeof out->utterance, out->answer);
        out->miss = 1;
        return 0;
    }
    if (table_query || !strncmp(q, "capsule ", 8) || intent_status == 1) {
        out->capsule_handled = 1;
        const char *typed = intent_status == 1 ? capsule_intent : q;
        const char *error = S->capsule_configured ? "capsule_runtime_unavailable" : "capsule_directory_not_configured";
        CnetCapsuleCoreReply cr = {0};
        CnetCoreLease *lease = S->capsule_store ? cnet_capsule_store_pin(S->capsule_store) :
            S->capsule_host ? cnet_core_host_pin(S->capsule_host) : NULL;
        int ok = lease && cnet_core_host_ask_text(lease, typed, &cr,out->answer,sizeof out->answer) == 0 && cr.verified;
        snprintf(out->source, sizeof out->source, "%s", ok ? "LOCAL" : "CNET");
        snprintf(out->skill, sizeof out->skill, "%s", ok ? "capsule_core" : "capsule_refusal");
        if (!ok) snprintf(out->answer, sizeof out->answer, "ABSTAIN: %s", lease ? cr.reason : error);
        const char *demand = getenv("CNET_CAPSULE_DEMAND_DIR");
        if (!table_query && !ok && lease && demand && (!strcmp(cr.reason, "unknown_or_ambiguous_interface") ||
            !strcmp(cr.reason, "no_covered_certified_plan"))) {
            int queued = cnet_capsule_demand_note(demand, typed);
            size_t used = strlen(out->answer);
            snprintf(out->answer + used, sizeof out->answer - used, "; demand=%s",
                queued == 0 ? "queued" : queued == 1 ? "existing" : "refused");
        }
        cd_scopy(out->utterance, sizeof out->utterance, out->answer);
        cd_scopy(out->prepared, sizeof out->prepared, typed);
        out->verified = ok; out->miss = !ok; out->may_voice = ok;
        cnet_core_host_unpin(lease);
        return 0;
    }

    /* Morphology before CORE/lookup so "past tense of fly" is not swallowed
     * by residual hop / kb_recall. Probes stay probes. */
    {
        char p0[96];
        const char *pm0 = cnet_probe_match(&S->probes, q, p0, sizeof p0);
        if (pm0) {
            out->shortcircuit = 1;
            snprintf(out->probe_pat, sizeof out->probe_pat, "%s",
                     p0[0] ? p0 : pm0);
            snprintf(out->source, sizeof out->source, "CNET");
            snprintf(out->skill, sizeof out->skill, "utter_probe");
            snprintf(out->answer, sizeof out->answer, "probe_shortcircuit");
            snprintf(out->utterance, sizeof out->utterance, "probe_shortcircuit");
            cd_scopy(out->prepared, sizeof out->prepared, q);
            out->miss = 1;
            out->verified = 0;
            out->tokens = 0;
            out->may_voice = 1;
            return 0;
        }
        if (cd_brick_shaped_ask(q, out) == 0)
            return 0;
        {
            CnetTeResult te;
            int some_local = 0, ti;
            if (cnet_te_ask(q, &te) == 0 && te.grammar_hit) {
                for (ti = 0; ti < te.n_cand; ti++)
                    if (te.cand[ti].local) some_local = 1;
                if (some_local && cnet_te_audit_ok(&te) &&
                    cnet_te_format_answer(&te, out->answer, sizeof out->answer) ==
                        0) {
                    out->typed_en_hit = 1;
                    snprintf(out->source, sizeof out->source, "LOCAL");
                    snprintf(out->skill, sizeof out->skill, "typed_en");
                    snprintf(out->domain, sizeof out->domain,
                             te.claimed_cert ? "CERT" : "TYPED_GAP");
                    cd_scopy(out->prepared, sizeof out->prepared, q);
                    out->miss = 0;
                    out->verified = te.claimed_cert ? 1 : 0;
                    out->tokens = 0;
                    out->may_voice = 1;
                    snprintf(out->utterance, sizeof out->utterance, "%.767s",
                             out->answer);
                    return 0;
                }
                out->typed_en_hit = 1;
                snprintf(out->source, sizeof out->source, "CNET");
                snprintf(out->skill, sizeof out->skill, "typed_en");
                snprintf(out->domain, sizeof out->domain, "ABSTAIN");
                if (cnet_te_format_answer(&te, out->answer, sizeof out->answer) !=
                    0)
                    snprintf(out->answer, sizeof out->answer,
                             "ABSTAIN: no sealed form. Not CERT.");
                snprintf(out->utterance, sizeof out->utterance, "%.767s",
                         out->answer);
                cd_scopy(out->prepared, sizeof out->prepared, q);
                out->miss = 1;
                out->verified = 0;
                out->tokens = 0;
                out->may_voice = 1;
                if (q && q[0])
                    cd_scopy(S->last_miss_q, sizeof S->last_miss_q, q);
                return 0;
            }
        }
        {
            CnetSkillLaneResult ar;
            memset(&ar, 0, sizeof ar);
            cnet_ood_numerals_reload();
            if (cnet_ood_try_add(q, &ar) == 0) {
                out->arith_hit = 1;
                snprintf(out->skill, sizeof out->skill, "%s",
                         ar.skill[0] ? ar.skill : CNET_OOD_ADD);
                cd_scopy(out->prepared, sizeof out->prepared, q);
                out->tokens = 0;
                out->may_voice = 1;
                if (ar.kind == CNET_SKILL_LANE_EXACT && ar.claimed_cert) {
                    snprintf(out->source, sizeof out->source, "LOCAL");
                    snprintf(out->domain, sizeof out->domain, "CERT");
                    snprintf(out->answer, sizeof out->answer, "%.2047s",
                             ar.spoken[0] ? ar.spoken : ar.value);
                    snprintf(out->utterance, sizeof out->utterance, "%.767s",
                             out->answer);
                    out->miss = 0;
                    out->verified = 1;
                    return 0;
                }
                if (ar.kind == CNET_SKILL_LANE_ABSTAIN) {
                    snprintf(out->source, sizeof out->source, "CNET");
                    snprintf(out->domain, sizeof out->domain, "ABSTAIN");
                    snprintf(out->answer, sizeof out->answer, "%.2047s",
                             ar.spoken[0] ? ar.spoken
                                          : "ABSTAIN: no sealed numeral. Not CERT.");
                    snprintf(out->utterance, sizeof out->utterance, "%.767s",
                             out->answer);
                    out->miss = 1;
                    out->verified = 0;
                    if (q && q[0])
                        cd_scopy(S->last_miss_q, sizeof S->last_miss_q, q);
                    if (S->miss_log[0]) {
                        FILE *mf = fopen(S->miss_log, "a");
                        char unk[32], qesc[1024];
                        unk[0] = 0;
                        (void)cnet_ood_gap_unknown(q, unk, sizeof unk);
                        cnet_json_escape(q, qesc, sizeof qesc);
                        if (mf) {
                            fprintf(mf,
                                    "{\"query\":\"%s\",\"source\":\"CNET\","
                                    "\"skill\":\"%s\",\"via\":\"numeral_gap\","
                                    "\"unknown\":\"%s\",\"auto_cert\":false,"
                                    "\"learnable\":true,\"shortcircuit\":false}\n",
                                    qesc, out->skill, unk);
                            fclose(mf);
                        }
                    }
                    return 0;
                }
            }
        }
        {
            CnetCertSolverResult sr;
            memset(&sr, 0, sizeof sr);
            if (cnet_cert_solver_ask(q, &sr) == 0 && sr.hit) {
                out->solver_hit = 1;
                snprintf(out->skill, sizeof out->skill, "%s",
                         CNET_CERT_SOLVER_SKILL);
                cd_scopy(out->prepared, sizeof out->prepared, q);
                out->tokens = 0;
                out->may_voice = 1;
                if (sr.claimed_cert && sr.bound) {
                    snprintf(out->source, sizeof out->source, "LOCAL");
                    snprintf(out->domain, sizeof out->domain, "CERT");
                    snprintf(out->answer, sizeof out->answer, "%s\n%.3200s",
                             sr.value, sr.show);
                    snprintf(out->utterance, sizeof out->utterance, "%.767s",
                             sr.value);
                    out->miss = 0;
                    out->verified = 1;
                    return 0;
                }
                snprintf(out->source, sizeof out->source, "CNET");
                snprintf(out->domain, sizeof out->domain, "ABSTAIN");
                snprintf(out->answer, sizeof out->answer, "%s\n%.3200s",
                         sr.spoken[0] ? sr.spoken : "ABSTAIN: no bind.",
                         sr.show);
                snprintf(out->utterance, sizeof out->utterance, "%.767s",
                         out->answer);
                out->miss = 1;
                out->verified = 0;
                if (q && q[0])
                    cd_scopy(S->last_miss_q, sizeof S->last_miss_q, q);
                return 0;
            }
        }
    }

    /* CORE switch: light CERT .lut bricks first (loaded/evolved on disk). */
    core_serve_try_reload();
    /* Layer3 live: specialist / committee / goal queue (CERT-only). */
    {
        const char *qq = q;
        while (*qq == ' ' || *qq == '\t') qq++;
        if (strncmp(qq, "specialist ", 11) == 0 || strncmp(qq, "committee ", 10) == 0) {
            int committee = (strncmp(qq, "committee ", 10) == 0);
            char tag[64];
            unsigned n = 0;
            const char *p = qq + (committee ? 10 : 11);
            CnetServeBank *sb = cnet_serve_global();
            CnetServeResult sr, sr2;
            int i, alt_ok = 0;
            tag[0] = 0;
            if (sscanf(p, "%63s %u", tag, &n) == 2 && sb) {
                char turn[96];
                snprintf(turn, sizeof turn, "%s %u", tag, n & 15u);
                if (cnet_serve_result(sb, turn, &sr) == 0 && sr.proved) {
                    if (committee) {
                        for (i = 0; i < sb->n; ++i) {
                            if (!sb->bricks[i].live) continue;
                            if (strcmp(sb->bricks[i].tag, tag) == 0) continue;
                            snprintf(turn, sizeof turn, "%s %u", sb->bricks[i].tag, n & 15u);
                            if (cnet_serve_result(sb, turn, &sr2) == 0 && sr2.proved) {
                                alt_ok = 1;
                                break;
                            }
                        }
                        if (!alt_ok) {
                            snprintf(out->answer, sizeof out->answer, "committee_incomplete");
                            snprintf(out->utterance, sizeof out->utterance, "committee_incomplete");
                            snprintf(out->source, sizeof out->source, "CNET");
                            snprintf(out->skill, sizeof out->skill, "committee");
                            out->verified = 0;
                            out->miss = 1;
                            out->may_voice = 0;
                            cd_scopy(out->prepared, sizeof out->prepared, q);
                            return 0;
                        }
                    }
                    snprintf(out->answer, sizeof out->answer, "%.2047s", sr.spoken);
                    snprintf(out->utterance, sizeof out->utterance, "%.767s", sr.spoken);
                    snprintf(out->source, sizeof out->source, "LOCAL");
                    snprintf(out->skill, sizeof out->skill, "%.63s",
                             committee ? "committee" : "specialist");
                    cd_scopy(out->prepared, sizeof out->prepared, q);
                    out->verified = 1;
                    out->miss = 0;
                    out->may_voice = 1;
                    out->tokens = 0;
                    return 0;
                }
            }
        }
        if (strncmp(qq, "goal:", 5) == 0 || strncmp(qq, "goal ", 5) == 0) {
            const char *dir = getenv("CNET_CORE_BUS_BRICKS_DIR");
            const char *body = qq;
            FILE *gf;
            while (*body && *body != ' ' && *body != ':') body++;
            while (*body == ' ' || *body == ':') body++;
            if (dir && dir[0] && body[0]) {
                char gpath[768];
                snprintf(gpath, sizeof gpath, "%s/pending_goals.txt", dir);
                gf = fopen(gpath, "a");
                if (gf) {
                    fprintf(gf, "%s\n", body);
                    fclose(gf);
                }
                setenv("CNET_AGI3", "1", 0);
                setenv("CNET_CORE_AUTO_EVOLVE", "1", 0);
                (void)core_evolve_run(S, 1); /* sync so result can land */
                core_serve_try_reload();
                /* If goal looks like prove TAG at N, try CERT serve now */
                {
                    char tag[64];
                    unsigned n = 0;
                    const char *bp = body;
                    char turn[96];
                    if (sscanf(bp, "prove %63s at %u", tag, &n) == 2 ||
                        sscanf(bp, "prove %63s %u", tag, &n) == 2) {
                        snprintf(turn, sizeof turn, "%s %u", tag, n & 15u);
                        cd_scopy(out->prepared, sizeof out->prepared, q);
                        if (core_try_serve(turn, out) == 0) {
                            snprintf(out->skill, sizeof out->skill, "goal_cert");
                            return 0;
                        }
                    }
                }
                cd_scopy(out->prepared, sizeof out->prepared, q);
                if (core_try_serve(q, out) == 0) {
                    snprintf(out->skill, sizeof out->skill, "goal_cert");
                    return 0;
                }
                snprintf(out->answer, sizeof out->answer, "goal_processed_abstain");
                snprintf(out->utterance, sizeof out->utterance,
                         "Goal processed; no CERT brick matched yet.");
                snprintf(out->source, sizeof out->source, "CNET");
                snprintf(out->skill, sizeof out->skill, "goal_queue");
                out->verified = 0;
                out->miss = 1;
                out->may_voice = 0;
                out->tokens = 0;
                return 0;
            }
        }
    }
    {
        CnetServeBank *sb = cnet_serve_global();
        CnetServeResult sr;
        /* Multi-hop chains belong to RLM. Prefix-CERT of the first brick
           would drop the rest of the turn. */
        if (!cnet_rlm_is_chain_turn(q) &&
            ((sb && cnet_serve_owns(sb, q)) || cd_lut_shaped(q))) {
            if (cnet_serve_result(sb, q, &sr) == 0 && sr.proved) {
            snprintf(out->answer, sizeof out->answer, "%.2047s", sr.spoken);
            snprintf(out->utterance, sizeof out->utterance, "%.767s", sr.spoken);
            snprintf(out->source, sizeof out->source, "LOCAL");
            snprintf(out->skill, sizeof out->skill, "%.63s",
                     sr.brick[0] ? sr.brick : "core_brick");
            cd_scopy(out->prepared, sizeof out->prepared, q);
            out->verified = 1;
            out->miss = 0;
            out->may_voice = 1;
            out->tokens = 0;
            return 0;
            }
            snprintf(out->answer, sizeof out->answer, "outside_table_abstain");
            snprintf(out->utterance, sizeof out->utterance, "outside_table_abstain");
            snprintf(out->source, sizeof out->source, "CNET");
            snprintf(out->skill, sizeof out->skill, "core_brick");
            cd_scopy(out->prepared, sizeof out->prepared, q);
            out->verified = 0;
            out->miss = 1;
            out->may_voice = 0;
            out->tokens = 0;
            return 0;
        }
    }

    /* RLM outer host wraps CORE CERT planes only (OPEN_CHAT answers killed).
       True miss → live waist abstain + optional evolve tick. */
    {
        CnetRlmPolicy rpol;
        CnetRlmResult rr;
        CnetChatLookupTurn hop;
        const CnetChatLookupTurn *hop_arg = NULL;
        memset(&hop, 0, sizeof hop);
        if (cnet_chat_lookup_cnetd_hop(q, &hop) == 0)
            hop_arg = &hop;
        else if (hop.residual_calls != 0)
            hop_arg = &hop;

        /* Residual hop refuse — never treat LLM hop as mouth */
        if (hop_arg != NULL && hop_arg->residual_calls != 0) {
            snprintf(out->answer, sizeof out->answer, "residual_mouth");
            snprintf(out->utterance, sizeof out->utterance, "residual_mouth");
            snprintf(out->source, sizeof out->source, "CNET");
            snprintf(out->skill, sizeof out->skill, "lookup_refuse");
            cd_scopy(out->prepared, sizeof out->prepared, q);
            out->verified = 0;
            out->miss = 1;
            out->may_voice = 0;
            out->tokens = 0;
            return 0;
        }
        /* Bound lookup hop: only arithmetic OOD may short-circuit as LOCAL.
           Wiki/held/other residual must not seal before pack ROE. */
        if (hop_arg != NULL && hop_arg->answered && hop_arg->report.bound &&
            hop_arg->report.value[0] != '\0') {
            CnetSkillLaneResult lane;
            CnetHemiResult hr;
            int arith = 0;
            memset(&lane, 0, sizeof lane);
            if (cnet_skill_lane_bind_lookup(hop_arg, &lane) == 0 && lane.bound) {
                cnet_hemi_classify_lane(&lane, 1, &hr);
                (void)cnet_brain_mirror_core(&hr);
                if (hr.skill[0] &&
                    (strcmp(hr.skill, "add_u32_v1") == 0 ||
                     strcmp(hr.skill, "sub_u32_v1") == 0 ||
                     strcmp(hr.skill, "mul_u32_v1") == 0 ||
                     strcmp(hr.skill, "div_u32_v1") == 0 ||
                     strcmp(hr.skill, "mod_u32_v1") == 0 ||
                     strcmp(hr.skill, "cmp_u32_v1") == 0 ||
                     strcmp(hr.skill, "min_u32_v1") == 0 ||
                     strcmp(hr.skill, "max_u32_v1") == 0 ||
                     strncmp(hr.skill, "add_", 4) == 0 ||
                     strncmp(hr.skill, "sub_", 4) == 0 ||
                     strncmp(hr.skill, "mul_", 4) == 0 ||
                     strncmp(hr.skill, "div_", 4) == 0 ||
                     strncmp(hr.skill, "mod_", 4) == 0 ||
                     strncmp(hr.skill, "cmp_", 4) == 0 ||
                     strncmp(hr.skill, "min_", 4) == 0 ||
                     strncmp(hr.skill, "max_", 4) == 0))
                    arith = 1;
                if (arith) {
                    snprintf(out->answer, sizeof out->answer, "%.2047s",
                             hr.spoken[0] ? hr.spoken : hr.value);
                    snprintf(out->utterance, sizeof out->utterance, "%.767s",
                             out->answer);
                    snprintf(out->source, sizeof out->source, "LOCAL");
                    snprintf(out->skill, sizeof out->skill, "%s",
                             hr.skill[0] ? hr.skill : "lookup");
                    cd_scopy(out->prepared, sizeof out->prepared, q);
                    out->verified = hr.claimed_cert ? 1 : 0;
                    out->miss = 0;
                    out->may_voice = hr.may_voice ? 1 : 0;
                    out->tokens = 0;
                    return 0;
                }
                /* non-arith bound hop: fall through to packs */
            }
        }

        if (cd_skip_teacher(S, q)) {
            out->miss = 1;
            out->verified = 0;
            out->stage_draft = 0;
            out->stage[0] = 0;
            out->answer[0] = 0;
            out->utterance[0] = 0;
            snprintf(out->source, sizeof out->source, "CORE");
            snprintf(out->skill, sizeof out->skill, "leftover_fast");
            cd_scopy(out->prepared, sizeof out->prepared, q);
            return 0;
        }

        cnet_rlm_policy_default(&rpol);
        memset(&rr, 0, sizeof rr);
        {
            int rlm_rc = cnet_rlm_ask_session(q, &rpol, &S->rlm_sess, &rr);
            if (rlm_rc == 0 && rr.final.bound) {
            const CnetHemiResult *hr = &rr.final;
            const char *spoken;
            int arith = 0;
            char tch[64];
            unsigned tin = 0, tout = 0;
            /* CORE OPEN_CHAT: generative plane, claimed_cert always 0.
             * G1: a sealed always-on pack outranks it. If the soul (or any
             * always-on CERT pack) already owns this query, decline the bind
             * and fall through so the pack answers it as LOCAL CERT. */
            if ((hr->plane == CNET_CORE_PLANE_OPEN_CHAT || hr->open_chat) &&
                cd_front_owns(S, q)) {
                /* fall through to probe / slot / CERT pack path below */
            } else if ((hr->plane == CNET_CORE_PLANE_OPEN_CHAT || hr->open_chat) &&
                       cnet_live_parse_teach(q, tch, sizeof tch, &tin, &tout) ==
                           0) {
                /* teach TAG n m must not be leftover mouth — harvest below */
            } else if (hr->plane == CNET_CORE_PLANE_OPEN_CHAT || hr->open_chat) {
                spoken = hr->spoken[0] ? hr->spoken
                                       : (hr->value[0] ? hr->value : hr->refusal);
                snprintf(out->answer, sizeof out->answer, "%.2047s", spoken);
                snprintf(out->utterance, sizeof out->utterance, "%.767s", spoken);
                snprintf(out->source, sizeof out->source, "CORE");
                snprintf(out->skill, sizeof out->skill, "%s",
                         hr->skill[0] ? hr->skill : "core_open_chat");
                cd_scopy(out->prepared, sizeof out->prepared, q);
                out->verified = 0;
                out->miss = 1; /* learnable; never LOCAL CERT */
                out->may_voice = hr->may_voice ? 1 : 0;
                out->tokens = 0;
                out->stage_draft = 1;
                cd_scopy(out->stage, sizeof out->stage, out->answer);
                /* Retrieve-before-talk: replace the bare held draft with a
                 * context-packed stage mouth so we don't invent Comcast/FAQ. */
                if (cnet_ml_stage_enabled()) {
                    char ctx[1400], draft[CNET_ML_TEXT];
                    int nc = cnet_ml_context_pack(q, ctx, sizeof ctx);
                    if (cnet_ml_stage_draft_ctx(q, ctx[0] ? ctx : NULL, draft,
                                               sizeof draft) &&
                        draft[0]) {
                        cd_check_citations(draft, ctx, nc);
                        snprintf(out->answer, sizeof out->answer, "%s", draft);
                        snprintf(out->utterance, sizeof out->utterance, "%.767s",
                                 draft);
                        cd_scopy(out->stage, sizeof out->stage, draft);
                    }
                }
                /* RSI: an open-chat bind must not SWALLOW a typed brick miss.
                 *
                 * This branch returns before the typed-brick harvest further
                 * down, so with CNET_CORE_OPEN_CHAT=1 a query like "q1_xor16 5"
                 * whose .lut is missing came back as a polite open-chat miss
                 * forever -- measured 0/10 recovery, no evolve child ever
                 * spawned. The law was fine (miss=1, claimed_cert=0, never
                 * LOCAL) but coverage could not grow back, which is the whole
                 * point of the loop.
                 *
                 * So harvest the typed miss and run the (rate-limited) evolve
                 * tick, then reload and retry the bank. Retry only promotes
                 * when core_try_serve proves the brick, so this cannot turn an
                 * open-chat answer into CERT -- if nothing proves, the
                 * open-chat reply above is returned unchanged. */
                {
                    char rsi_tag[64];
                    unsigned rsi_n = 0;
                    if (cnet_live_parse_tag_n(q, rsi_tag, sizeof rsi_tag, &rsi_n) == 0) {
                        if (S->miss_log[0])
                            (void)cnet_live_miss_append(S->miss_log, rsi_tag, rsi_n, 0, 0);
                        core_evolve_tick(S);
                        core_serve_try_reload();
                        if (core_try_serve(q, out) == 0) return 0;
                    }
                }
                return 0;
            }
            /* Wiki/residual claiming CERT must not beat pack ROE. Arithmetic OK. */
            if (hr->skill[0] &&
                (strcmp(hr->skill, "add_u32_v1") == 0 ||
                 strcmp(hr->skill, "sub_u32_v1") == 0 ||
                 strcmp(hr->skill, "mul_u32_v1") == 0 ||
                 strcmp(hr->skill, "div_u32_v1") == 0 ||
                 strcmp(hr->skill, "mod_u32_v1") == 0 ||
                 strcmp(hr->skill, "cmp_u32_v1") == 0 ||
                 strcmp(hr->skill, "min_u32_v1") == 0 ||
                 strcmp(hr->skill, "max_u32_v1") == 0 ||
                 strncmp(hr->skill, "add_", 4) == 0 ||
                 strncmp(hr->skill, "sub_", 4) == 0 ||
                 strncmp(hr->skill, "mul_", 4) == 0 ||
                 strncmp(hr->skill, "div_", 4) == 0 ||
                 strncmp(hr->skill, "mod_", 4) == 0 ||
                 strncmp(hr->skill, "cmp_", 4) == 0 ||
                 strncmp(hr->skill, "min_", 4) == 0 ||
                 strncmp(hr->skill, "max_", 4) == 0))
                arith = 1;
            if (!arith &&
                (strstr(hr->skill, "wiki") || strstr(hr->skill, "held") ||
                 hr->skill[0] == '\0')) {
                /* fall through to pack path */
            } else if (arith || hr->plane == CNET_CORE_PLANE_CERT ||
                       hr->hemi == CNET_HEMI_CORE) {
            spoken =
                hr->spoken[0] ? hr->spoken : (hr->value[0] ? hr->value : hr->refusal);
            snprintf(out->answer, sizeof out->answer, "%.2047s", spoken);
            snprintf(out->utterance, sizeof out->utterance, "%.767s", spoken);
            if (hr->plane == CNET_CORE_PLANE_CERT || hr->hemi == CNET_HEMI_CORE)
                snprintf(out->source, sizeof out->source, "LOCAL");
            else
                snprintf(out->source, sizeof out->source, "RLM");
            snprintf(out->skill, sizeof out->skill, "%s",
                     hr->skill[0] ? hr->skill : "rlm");
            cd_scopy(out->prepared, sizeof out->prepared, q);
            out->verified = hr->claimed_cert ? 1 : 0;
            out->miss = 0;
            out->may_voice = hr->may_voice ? 1 : 0;
            out->tokens = 0;
            if (S->miss_log[0] &&
                (hr->plane == CNET_CORE_PLANE_OPEN_CHAT || hr->open_chat)) {
                FILE *mf = fopen(S->miss_log, "a");
                if (mf) {
                    fprintf(mf,
                            "{\"via\":\"cnet_rlm\",\"skill\":\"%s\","
                            "\"plane\":\"OPEN_CHAT\",\"via_rlm\":true,"
                            "\"via_core\":true,\"claimed_cert\":0,"
                            "\"intent\":\"%s\",\"may_voice\":%s}\n",
                            out->skill, cnet_core_intent_name(rr.intent),
                            out->may_voice ? "true" : "false");
                    fclose(mf);
                }
            }
            return 0;
            }
            /* else fall through */
            } else if (rlm_rc >= 0 && cnet_rlm_is_chain_turn(q) && rr.n_steps > 0) {
                /* Multi-hop was hosted by RLM. Do not pack-soft-seal.
                   Self-improve: harvest typed miss → sync evolve → one retry
                   so known curriculum gaps can mint and succeed this turn. */
                const CnetHemiResult *hr = &rr.final;
                const char *why =
                    hr->refusal[0] ? hr->refusal
                                   : (rr.summary[0] ? rr.summary : "rlm_budget");
                const char *bdir = getenv("CNET_CORE_BUS_BRICKS_DIR");
                int harvested = 0;
                int retried = 0;

                if (S->miss_log[0])
                    harvested = cnet_live_miss_harvest_turn(S->miss_log, q);
                /* Queue goals for failed hop skills with numeric inputs. */
                if (bdir && bdir[0]) {
                    unsigned i;
                    for (i = 0; i < (unsigned)rr.n_steps && i < 4u; i++) {
                        const CnetHemiResult *st = &rr.steps[i].step;
                        if (st->claimed_cert) continue;
                        if (st->skill[0])
                            (void)cnet_live_miss_queue_goal(bdir, st->skill, 0);
                    }
                    (void)cnet_live_miss_harvest_turn(
                        S->miss_log[0] ? S->miss_log : "/dev/null", q);
                    /* parse first hop n for goal */
                    {
                        char ttag[64];
                        unsigned tin = 0;
                        const char *p = q;
                        /* best-effort: harvest already wrote rows; also goal */
                        while (*p) {
                            size_t ti = 0;
                            unsigned v = 0;
                            int saw = 0;
                            while (*p == ' ' || *p == '|' || *p == '\t') p++;
                            if (!(isalpha((unsigned char)*p) || *p == '_'))
                                break;
                            while (*p && *p != ' ' && *p != ':' && *p != '|' &&
                                   ti + 1 < sizeof ttag)
                                ttag[ti++] = *p++;
                            ttag[ti] = 0;
                            if (*p == ':') p++;
                            while (*p == ' ') p++;
                            if (isdigit((unsigned char)*p)) {
                                while (isdigit((unsigned char)*p)) {
                                    saw = 1;
                                    v = v * 10u + (unsigned)(*p - '0');
                                    if (v > 15) {
                                        saw = 0;
                                        break;
                                    }
                                    p++;
                                }
                            }
                            if (ttag[0] && saw)
                                (void)cnet_live_miss_queue_goal(bdir, ttag, v);
                            while (*p && *p != 't' && *p != 'T' && *p != '|')
                                p++;
                            if ((p[0] == 't' || p[0] == 'T') &&
                                (p[1] == 'h' || p[1] == 'H'))
                                p += 4;
                            else if (*p == '|')
                                p++;
                            else
                                break;
                        }
                        (void)tin;
                    }
                }

                /* Sync evolve so missing factory curriculum can land now.
                   Force factory only for this child evolve — do not permanently
                   pollute the daemon env. */
                setenv("CNET_CORE_AUTO_EVOLVE", "1", 0);
                {
                    const char *prev_fac = getenv("CNET_CORE_EVOLVE_FACTORY");
                    char prev_buf[8];
                    int had_prev = 0;
                    if (prev_fac && prev_fac[0]) {
                        snprintf(prev_buf, sizeof prev_buf, "%.7s", prev_fac);
                        had_prev = 1;
                    }
                    setenv("CNET_CORE_EVOLVE_FACTORY", "1", 1);
                    (void)core_evolve_run(S, 1);
                    if (had_prev)
                        setenv("CNET_CORE_EVOLVE_FACTORY", prev_buf, 1);
                    else
                        unsetenv("CNET_CORE_EVOLVE_FACTORY");
                }
                core_serve_try_reload();

                /* One retry through RLM after evolve/reload. */
                {
                    CnetRlmResult rr2;
                    memset(&rr2, 0, sizeof rr2);
                    if (cnet_rlm_ask_session(q, &rpol, &S->rlm_sess, &rr2) == 0 &&
                        rr2.final.bound &&
                        rr2.final.plane == CNET_CORE_PLANE_CERT &&
                        rr2.final.claimed_cert) {
                        const CnetHemiResult *h2 = &rr2.final;
                        const char *sp =
                            h2->spoken[0] ? h2->spoken
                                          : (h2->value[0] ? h2->value : why);
                        snprintf(out->answer, sizeof out->answer, "%.2047s", sp);
                        snprintf(out->utterance, sizeof out->utterance, "%.767s",
                                 sp);
                        snprintf(out->source, sizeof out->source, "LOCAL");
                        snprintf(out->skill, sizeof out->skill, "%.63s",
                                 h2->skill[0] ? h2->skill : "rlm_retry");
                        cd_scopy(out->prepared, sizeof out->prepared, q);
                        out->verified = 1;
                        out->miss = 0;
                        out->may_voice = h2->may_voice ? 1 : 0;
                        out->tokens = 0;
                        retried = 1;
                        if (S->miss_log[0]) {
                            FILE *mf = fopen(S->miss_log, "a");
                            if (mf) {
                                fprintf(mf,
                                        "{\"via\":\"cnetd\",\"reason\":"
                                        "\"self_improve_retry_ok\","
                                        "\"harvested\":%d,\"claimed_cert\":1,"
                                        "\"auto_cert\":false}\n",
                                        harvested);
                                fclose(mf);
                            }
                        }
                        return 0;
                    }
                }

                snprintf(out->answer, sizeof out->answer, "%.2047s", why);
                snprintf(out->utterance, sizeof out->utterance, "%.767s", why);
                snprintf(out->source, sizeof out->source, "CNET");
                snprintf(out->skill, sizeof out->skill, "%.63s",
                         hr->skill[0] ? hr->skill : "rlm_chain_abstain");
                cd_scopy(out->prepared, sizeof out->prepared, q);
                out->verified = 0;
                out->miss = 1;
                out->may_voice = 0;
                out->tokens = 0;
                if (S->miss_log[0]) {
                    FILE *mf = fopen(S->miss_log, "a");
                    if (mf) {
                        fprintf(mf,
                                "{\"via\":\"cnetd\",\"reason\":"
                                "\"self_improve_pending\",\"harvested\":%d,"
                                "\"retried\":%d,\"claimed_cert\":0,"
                                "\"auto_cert\":false,\"learnable\":true}\n",
                                harvested, retried);
                        fclose(mf);
                    }
                }
                return 0;
            }
        }
        /* LIVE WAIST: typed CORE miss capture + evolve + retry CERT once.
           Freeform queries fall through to pack ROE (alias/dialog/slot/CERT).
           Do not hard-return outside_table_abstain before packs — that killed
           Autonomous-ASI LOCAL (soul/ops/english) on monorepo cnetd overnight. */
        {
            char tag[64];
            unsigned in_n = 0, out_n = 0;
            int taught = 0;
            int pairs = 0;
            float _lut[16];
            tag[0] = 0;
            if (cnet_live_parse_teach(q, tag, sizeof tag, &in_n, &out_n) == 0) {
                if (S->miss_log[0])
                    (void)cnet_live_miss_append(S->miss_log, tag, in_n, 1, out_n);
                taught = 1;
            } else if (cnet_live_parse_tag_n(q, tag, sizeof tag, &in_n) == 0) {
                if (S->miss_log[0])
                    (void)cnet_live_miss_append(S->miss_log, tag, in_n, 0, 0);
            }
            /* Only run CORE evolve/serve path for typed brick traffic. */
            if (taught || tag[0]) {
                if (tag[0] && S->miss_log[0])
                    pairs = cnet_live_miss_domain_pairs(S->miss_log, tag, _lut);
                if (taught || pairs >= 16) {
                    setenv("CNET_CORE_AUTO_EVOLVE", "1", 0);
                    (void)core_evolve_run(S, 1);
                } else {
                    core_evolve_tick(S);
                }
                core_serve_try_reload();
                cd_scopy(out->prepared, sizeof out->prepared, q);
                if (core_try_serve(q, out) == 0)
                    return 0;
                if (taught && tag[0]) {
                    char turn[96];
                    snprintf(turn, sizeof turn, "%s %u", tag, in_n);
                    if (core_try_serve(turn, out) == 0)
                        return 0;
                }
                if (taught && tag[0]) {
                    snprintf(out->answer, sizeof out->answer,
                             "Taught %s %u->%u (%d/16). Not CERT until 16/16.",
                             tag, in_n, out_n, pairs);
                    snprintf(out->utterance, sizeof out->utterance, "%.767s",
                             out->answer);
                    snprintf(out->source, sizeof out->source, "CNET");
                    snprintf(out->skill, sizeof out->skill, "typed_teach_pending");
                    out->verified = 0;
                    out->miss = 1;
                    out->may_voice = 1;
                    out->tokens = 0;
                    return 0;
                }
                /* Typed CORE miss: abstain (no pack soft-seal). */
                snprintf(out->answer, sizeof out->answer, "outside_table_abstain");
                snprintf(out->utterance, sizeof out->utterance, "outside_table_abstain");
                snprintf(out->source, sizeof out->source, "CNET");
                snprintf(out->skill, sizeof out->skill,
                         taught ? "typed_teach_pending" : "outside_table_abstain");
                out->verified = 0;
                out->miss = 1;
                out->may_voice = 0;
                out->tokens = 0;
                return 0;
            }
            /* freeform → pack path below */
        }
    }
    memset(&ameta, 0, sizeof ameta);
    memset(&dmeta, 0, sizeof dmeta);
    memset(&smeta, 0, sizeof smeta);
    ppat[0] = 0;
    q_prep[0] = 0;
    q_slot[0] = 0;
    q_final[0] = 0;

    /* Probe SC always on ORIGINAL user text (never alias-smoothed). */
    pm = cnet_probe_match(&S->probes, q, ppat, sizeof ppat);
    if (pm) {
        out->shortcircuit = 1;
        snprintf(out->probe_pat, sizeof out->probe_pat, "%s", ppat[0] ? ppat : pm);
        q_use = q; /* probes stay literal */
        cd_scopy(out->prepared, sizeof out->prepared, q);
    } else {
        /* A: normalize + alias → sealed patterns */
        cnet_query_prepare(&S->aliases, q, q_prep, sizeof q_prep, &ameta);
        out->alias_hit = ameta.alias_hit ? 1 : 0;
        if (ameta.matched_alias[0])
            cd_scopy(out->alias_pat, sizeof out->alias_pat, ameta.matched_alias);

        /* C: pack-local ops slot extract on prepared text */
        if (cnet_slot_extract_ops(q_prep, q_slot, sizeof q_slot, &smeta) &&
            smeta.applied) {
            out->slot_hit = 1;
            cd_scopy(out->slot_unit, sizeof out->slot_unit, smeta.unit);
            cd_scopy(out->slot_reason, sizeof out->slot_reason, smeta.reason);
            cd_scopy(q_final, sizeof q_final, q_slot);
            q_use = q_final;
        } else if (cnet_dialog_resolve(&S->dialog, q_prep, q_final, sizeof q_final,
                                       &dmeta) &&
                   dmeta.applied) {
            /* B: anaphora when no explicit slot unit */
            out->dialog_hit = 1;
            cd_scopy(out->dialog_reason, sizeof out->dialog_reason, dmeta.reason);
            cd_scopy(out->dialog_entity, sizeof out->dialog_entity,
                     dmeta.entity_used);
            q_use = q_final;
        } else {
            cd_scopy(q_final, sizeof q_final, q_prep);
            q_use = q_final;
        }
        cd_scopy(out->prepared, sizeof out->prepared, q_use);
    }

    /* Typed English student: morphology grammars before FAQ catalog.
     * Probe SC never rewritten. Unknown lemma falls through to miss/Teacher. */
    if (!pm) {
        CnetTeResult te;
        int some_local = 0, ti;
        if (cnet_te_ask(q_use, &te) == 0 && te.grammar_hit) {
            for (ti = 0; ti < te.n_cand; ti++)
                if (te.cand[ti].local) some_local = 1;
            if (some_local && cnet_te_audit_ok(&te) &&
                cnet_te_format_answer(&te, out->answer, sizeof out->answer) == 0) {
                typed_handled = 1;
                out->typed_en_hit = 1;
                snprintf(out->source, sizeof out->source, "LOCAL");
                snprintf(out->skill, sizeof out->skill, "typed_en");
                snprintf(out->domain, sizeof out->domain,
                         te.claimed_cert ? "CERT" : "TYPED_GAP");
                out->miss = 0;
                out->verified = te.claimed_cert ? 1 : 0;
                out->tokens = 0;
                out->may_voice = 1;
                snprintf(out->utterance, sizeof out->utterance, "%.767s",
                         out->answer);
            }
        }
    }

    if (!typed_handled) {
    cnet_domain_route_resolve(&S->domain, q_use, &dd);
    snprintf(out->domain, sizeof out->domain, "%s", dd.kind_name ? dd.kind_name : "NONE");

    R = (RoeAsi *)calloc(1, sizeof *R);
    if (!R) return -1;
    roe_init(R);
    for (i = 0; i < S->n_always; i++)
        (void)load_pack(R, S->root, S->always_on[i]);
    matched = cd_match_route(S, q_use, &rt);
    if (matched >= 0 && rt.pack[0]) {
        int known = 0;
        for (i = 0; i < S->n_always; i++)
            if (strcmp(S->always_on[i], rt.pack) == 0) known = 1;
        if (!known) (void)load_pack(R, S->root, rt.pack);
    }
    /* Domain route CERT pack hint also loads when ROUTES miss */
    if (dd.kind == CNET_ROUTE_CERT && dd.pack_or_skill[0]) {
        int known = 0;
        for (i = 0; i < S->n_always; i++)
            if (strcmp(S->always_on[i], dd.pack_or_skill) == 0) known = 1;
        if (!known && (!rt.pack[0] || strcmp(rt.pack, dd.pack_or_skill) != 0))
            (void)load_pack(R, S->root, dd.pack_or_skill);
        if (!rt.pack[0])
            cd_scopy(rt.pack, sizeof rt.pack, dd.pack_or_skill);
    }

    roe_net_from_env(&net);
    /* Teach path: Teacher on organic miss (default ON). Probes never teacher.
     * Self-answer fills only when teacher off or teacher fails. */
    {
        const char *tm = getenv("CNET_TEACHER_ON_MISS");
        const char *sa = getenv("CNET_SELF_ANSWER");
        int teacher_on_miss = 0; /* leftover LLM answers killed — tables only */
        (void)sa;
        if (tm && (tm[0] == '0' || tm[0] == 'n' || tm[0] == 'N' || tm[0] == 'f'))
            teacher_on_miss = 0;
        if (teacher_on_miss) {
            net.enable_llm = 1;
            /* lookup optional — leave env default */
        } else {
            net.enable_llm = 0;
            net.enable_lookup = 0;
        }
    }
    if (pm) {
        net.enable_llm = 0;
        net.enable_lookup = 0;
    }
    if (net.enable_llm || net.enable_lookup) roe_set_net(R, &net);
    /* R is request-local: legacy catalogs cannot bypass the social boundary. */
    for (size_t si = 0; si < R->n_skills; si++)
        if (!cd_social_pattern_allowed(strlen(q) >= CNET_QA_OUT ? q : q_use,
                                       R->skills[si].pattern, R->skills[si].id))
            R->skills[si].active = 0;
    roe_turn(R, q_use, &rep);

    snprintf(out->source, sizeof out->source, "%s",
             rep.source_name[0] ? rep.source_name : "UNKNOWN");
    snprintf(out->skill, sizeof out->skill, "%s", rep.skill_id);
    snprintf(out->answer, sizeof out->answer, "%s", rep.answer);
    /* O2: if the question names several things sealed skills each answer,
     * give all of them rather than the first. Verbatim sealed content only. */
    if (rep.source == ROE_SRC_LOCAL) {
        char comp[ROE_ANSWER_MAX];
        char cids[192];
        CdSpan spans[16];
        int nsp;
        char gap[64];
        if (cd_compose_sealed(R, q_use, comp, sizeof comp, cids, sizeof cids) > 1) {
            snprintf(out->answer, sizeof out->answer, "%s", comp);
            snprintf(out->skill, sizeof out->skill, "%.63s", cids);
        }
        /* O2b: disclose an unanswered coordinated part. The certified half stays
         * certified; it just stops claiming to be the whole answer. */
        nsp = cd_sealed_spans(R, q_use, spans, (int)(sizeof spans / sizeof spans[0]));
        if (nsp >= 1 && cd_coverage_gap(q_use, spans, nsp, gap, sizeof gap)) {
            size_t al = strlen(out->answer);
            if (al + 72 < sizeof out->answer)
                snprintf(out->answer + al, sizeof out->answer - al,
                         "  (nothing sealed here for \"%.24s\" - ask it separately)",
                         gap);
        }
    }
    out->verified = rep.verified ? 1 : 0;
    out->tokens = (unsigned long long)rep.tokens_est;
    out->miss = (rep.source != ROE_SRC_LOCAL) ? 1 : 0;
    /* DOMAIN is the serve path, not the unused domain_routes miss.
     * Pack ROUTES can seal LOCAL/CERT while the TSV table abstains
     * (English pack, soul, …). Printing ABSTAIN next to claimed_cert=1
     * lied about the turn. Keep table ABSTAIN only when we did not seal. */
    if (rep.source == ROE_SRC_LOCAL && out->verified && !out->miss) {
        if (!out->domain[0] || strcmp(out->domain, "ABSTAIN") == 0 ||
            strcmp(out->domain, "NONE") == 0)
            snprintf(out->domain, sizeof out->domain, "CERT");
    }

    } /* !typed_handled */

    /* Warm dialog ctx AFTER turn — entities from prepared CERT-shaped query. */
    if (!out->shortcircuit) {
        const char *pack_hint = "pack_english_basic";
        if (!typed_handled)
            pack_hint = rt.pack[0] ? rt.pack : dd.pack_or_skill;
        cnet_dialog_ctx_update(&S->dialog, q_use, out->skill, pack_hint,
                               !out->miss);
    }

    /* C-native utterance always; self-answer replaces only non-LLM misses */
    {
        static CnetUtterBank UB;
        static int ub_ready;
        CnetUtterState U;
        const char *when = NULL;
        const char *env;
        int self_answer = 1;
        int teacher_on_miss = 0; /* killed leftover ROE mouth */
        env = getenv("CNET_SELF_ANSWER");
        if (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f'))
            self_answer = 0;
        env = getenv("CNET_TEACHER_ON_MISS");
        if (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f'))
            teacher_on_miss = 0;

        if (!ub_ready) {
            cnet_utter_bank_init_default(&UB);
            (void)cnet_utter_bank_load_tsv(&UB, "config/utterance_phrases.tsv");
            {
                const char *min = getenv("CNET_MINIMAL_ROOT");
                char p2[CD_PATH];
                if (min && min[0]) {
                    snprintf(p2, sizeof p2, "%s/config/utterance_phrases.tsv", min);
                    (void)cnet_utter_bank_load_tsv(&UB, p2);
                }
            }
            ub_ready = 1;
        }
        cnet_utter_state_init(&U);
        env = getenv("CNET_NEVER_VOICE_LLM");
        if (env && env[0] == '0') U.never_voice_llm = 0;
        cd_scopy(U.source, sizeof U.source, out->source);
        cd_scopy(U.skill, sizeof U.skill, out->skill);
        cd_scopy(U.domain, sizeof U.domain, out->domain);
        snprintf(U.pattern, sizeof U.pattern, "%.95s", q_use ? q_use : "");
        snprintf(U.base_answer, sizeof U.base_answer, "%.767s", out->answer);

        if (out->shortcircuit) when = "miss";
        else if (!out->miss && (contains_ci(q_use, "who are you") || contains_ci(q_use, "speech")))
            when = "identity";
        else if (!out->miss && (contains_ci(q_use, "status") || contains_ci(q_use, "local hit")))
            when = "status";
        else if (contains_ci(q_use, "how do you answer") || contains_ci(q_use, "self answer") ||
                 contains_ci(q_use, "without teacher") || contains_ci(q_use, "who writes"))
            when = "meta";
        else if (out->miss)
            when = "miss";
        else if (contains_ci(q_use, "who are you") || contains_ci(q_use, "speech"))
            when = "identity";
        else if (contains_ci(q_use, "status") || contains_ci(q_use, "local hit"))
            when = "status";

        load_neuromod_into(&U);
        (void)cnet_utter_compose(&UB, &U, when, out->utterance, sizeof out->utterance);
        if (!out->utterance[0])
            snprintf(out->utterance, sizeof out->utterance, "%.767s", out->answer);
        /* Affect wrap: mood colors utterance only, never the CERT answer. */
        if (!out->miss && out->verified &&
            (!when || strcmp(when, "identity") != 0)) {
            char colored[sizeof out->utterance];
            if (cnet_sr_color_utterance(&S->show, out->utterance, colored,
                                        sizeof colored) == 0 &&
                colored[0])
                snprintf(out->utterance, sizeof out->utterance, "%s", colored);
        }

        /*
         * Canned miss_topic/utter_self is not conversation (user law).
         * Organic miss keeps whatever CORE/STAGE/INFO/pack already wrote.
         * Probe short-circuit may still use the C probe line — no teacher.
         */
        if (out->miss && out->shortcircuit && self_answer &&
            strcmp(out->source, "LLM") != 0) {
            snprintf(out->answer, sizeof out->answer, "%.2047s", out->utterance);
            snprintf(out->source, sizeof out->source, "CNET");
            snprintf(out->skill, sizeof out->skill, "utter_probe");
            out->verified = 0;
            out->tokens = 0;
            snprintf(U.source, sizeof U.source, "CNET");
        } else if (out->miss && strcmp(out->source, "LLM") == 0) {
            /* Teacher taught this turn — keep full draft; utterance stays C meta/miss line */
            /* Optionally attach CNET coda for law reminder if utterance empty */
            if (!out->utterance[0])
                snprintf(out->utterance, sizeof out->utterance,
                         "Teacher draft logged for later gold review. Law: never self-cert.");
        }
        (void)teacher_on_miss;

        /* Draft mouth: wrap a bound CERT scalar. Teacher is never the mouth. */
        if (!out->miss && out->verified && strcmp(out->source, "LLM") != 0 &&
            cnet_c_speak_slot_like(out->answer)) {
            CnetCSpeakResult cap;
            if (cnet_c_speak_after_capsule(out->answer, out->skill, &cap) == 0 &&
                cap.wrapped && cap.residual_calls == 0 &&
                strstr(cap.spoken, out->answer) != NULL) {
                snprintf(out->utterance, sizeof out->utterance, "%s", cap.spoken);
                snprintf(out->answer, sizeof out->answer, "%s", cap.spoken);
            }
        }

        out->may_voice = cnet_utter_may_voice(&U, out->source);
        if (strcmp(out->source, "CNET") == 0 || strcmp(out->source, "LOCAL") == 0)
            out->may_voice = 1;
        /* never voice raw teacher unless override */
        if (strcmp(out->source, "LLM") == 0 && U.never_voice_llm)
            out->may_voice = 0;
    }

    /* append miss with shortcircuit / open-chat learn tags */
    if (out->miss) {
        FILE *f = fopen(S->miss_log, "a");
        const char *var_miss = getenv("CNET_MISS_LOG");
        FILE *f2 = NULL;
        char ts[40];
        char qesc[1024], aesc[ROE_ANSWER_MAX * 2];
        time_t t = time(NULL);
        struct tm tm;
        if (!out->shortcircuit && q && q[0])
            cd_scopy(S->last_miss_q, sizeof S->last_miss_q, q);
        gmtime_r(&t, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
        cnet_json_escape(q, qesc, sizeof qesc);
        cnet_json_escape(out->answer, aesc, sizeof aesc);
        if (var_miss && var_miss[0] && strcmp(var_miss, S->miss_log) != 0)
            f2 = fopen(var_miss, "a");
        {
            int is_llm = (strcmp(out->source, "LLM") == 0);
            int is_cnet = (strcmp(out->source, "CNET") == 0);
            FILE *sinks[2];
            int si;
            sinks[0] = f;
            sinks[1] = f2;
            for (si = 0; si < 2; si++) {
                FILE *fp = sinks[si];
                if (!fp) continue;
                if (out->shortcircuit)
                    fprintf(fp,
                            "{\"ts\":\"%s\",\"query\":\"%s\",\"source\":\"%s\","
                            "\"answer\":\"%s\",\"shortcircuit\":true,\"probe_pat\":\"%s\","
                            "\"teacher\":false,\"learnable\":false,\"self_answer\":true,"
                            "\"via\":\"cnetd\",\"tokens_est\":%llu}\n",
                            ts, qesc, out->source, aesc, out->probe_pat,
                            (unsigned long long)out->tokens);
                else
                    fprintf(fp,
                            "{\"ts\":\"%s\",\"query\":\"%s\",\"source\":\"%s\","
                            "\"answer\":\"%s\",\"shortcircuit\":false,"
                            "\"teacher\":%s,\"learnable\":%s,\"open_chat\":%s,"
                            "\"self_answer\":%s,\"auto_cert\":false,\"via\":\"cnetd\","
                            "\"tokens_est\":%llu}\n",
                            ts, qesc, out->source, aesc,
                            is_llm ? "true" : "false",
                            (is_llm || is_cnet) ? "true" : "false",
                            is_llm ? "true" : "false",
                            is_cnet ? "true" : "false",
                            (unsigned long long)out->tokens);
                fclose(fp);
            }
        }
    }
    if (R) free(R);
    return 0;
}


static ssize_t full_write(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t k = write(fd, p + off, n - off);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (k == 0) break;
        off += (size_t)k;
    }
    return (ssize_t)off;
}

static void write_json_reply(int fd, const CdReply *r) {
    /* Worst case one input byte becomes six output bytes (backslash-u-00XX), so
       every buffer is 6N+8 and cnet_json_escape can never hit its truncation
       path here. Until 2026-08-12 only answer and utterance were escaped at
       all -- the other ten %s fields carried model-, user- and file-derived
       text straight into the document, so a single double quote in a skill name
       or a raw C0 byte in a probe pattern produced a reply the daemon's own
       client could not parse. */
    char aesc[ROE_ANSWER_MAX * 6 + 8], uesc[CNET_UTTER_TEXT * 6 + 8];
    char src_e[32 * 6 + 8], skill_e[64 * 6 + 8], dom_e[32 * 6 + 8];
    char probe_e[96 * 6 + 8], prep_e[CNET_QA_OUT * 6 + 8];
    char alias_e[CNET_QA_PAT * 6 + 8], dreason_e[48 * 6 + 8];
    char dent_e[CNET_DC_ENT * 6 + 8], sunit_e[CNET_SLOT_UNIT * 6 + 8];
    char sreason_e[48 * 6 + 8];
    char peer_e[sizeof r->peer * 6 + 8], stage_e[sizeof r->stage * 6 + 8];
    char buf[sizeof aesc + sizeof uesc + sizeof src_e + sizeof skill_e +
             sizeof dom_e + sizeof probe_e + sizeof prep_e + sizeof alias_e +
             sizeof dreason_e + sizeof dent_e + sizeof sunit_e +
             sizeof sreason_e + sizeof peer_e + sizeof stage_e + 896];
    (void)cnet_json_escape(r->peer, peer_e, sizeof peer_e);
    (void)cnet_json_escape(r->stage, stage_e, sizeof stage_e);
    (void)cnet_json_escape(r->answer, aesc, sizeof aesc);
    (void)cnet_json_escape(r->utterance, uesc, sizeof uesc);
    (void)cnet_json_escape(r->source, src_e, sizeof src_e);
    (void)cnet_json_escape(r->skill, skill_e, sizeof skill_e);
    (void)cnet_json_escape(r->domain, dom_e, sizeof dom_e);
    (void)cnet_json_escape(r->probe_pat, probe_e, sizeof probe_e);
    (void)cnet_json_escape(r->prepared, prep_e, sizeof prep_e);
    (void)cnet_json_escape(r->alias_pat, alias_e, sizeof alias_e);
    (void)cnet_json_escape(r->dialog_reason, dreason_e, sizeof dreason_e);
    (void)cnet_json_escape(r->dialog_entity, dent_e, sizeof dent_e);
    (void)cnet_json_escape(r->slot_unit, sunit_e, sizeof sunit_e);
    (void)cnet_json_escape(r->slot_reason, sreason_e, sizeof sreason_e);
    snprintf(buf, sizeof buf,
             "{\"ok\":true,\"source\":\"%s\",\"skill\":\"%s\",\"answer\":\"%s\","
             "\"utterance\":\"%s\",\"may_voice\":%s,"
             "\"miss\":%s,\"verified\":%s,\"tokens_est\":%llu,"
             "\"domain_route\":\"%s\",\"shortcircuit\":%s,\"probe_pat\":\"%s\","
             "\"prepared\":\"%s\",\"alias_hit\":%s,\"alias\":\"%s\","
             "\"dialog_hit\":%s,\"dialog_reason\":\"%s\",\"dialog_entity\":\"%s\","
             "\"slot_hit\":%s,\"slot_unit\":\"%s\",\"slot_reason\":\"%s\","
             "\"teacher\":%s,\"composer\":\"cnet_utterance\",\"never_voice_llm\":true,"
             "\"self_answer\":%s,\"peer\":\"%s\",\"stage_draft\":%s,\"stage\":\"%s\"}\n",
             src_e, skill_e, aesc, uesc, r->may_voice ? "true" : "false",
             r->miss ? "true" : "false",
             r->verified ? "true" : "false", r->tokens, dom_e,
             r->shortcircuit ? "true" : "false", probe_e,
             prep_e,
             r->alias_hit ? "true" : "false",
             alias_e,
             r->dialog_hit ? "true" : "false",
             dreason_e,
             dent_e,
             r->slot_hit ? "true" : "false",
             sunit_e,
             sreason_e,
             (strcmp(r->source, "LLM") == 0) ? "true" : "false",
             (strcmp(r->source, "CNET") == 0 || strcmp(r->source, "LOCAL") == 0) ? "true"
                                                                                : "false",
             peer_e, r->stage_draft ? "true" : "false", stage_e);
    (void)full_write(fd, buf, strlen(buf));
}

static void write_text_reply(int fd, const CdReply *r) {
    char head[768];
    char body[ROE_ANSWER_MAX + 64];
    char utter[CNET_UTTER_TEXT + 64];
    int n;
    n = snprintf(head, sizeof head,
                 "SOURCE %s\nSKILL %s\nDOMAIN %s\nMISS %d\nSHORTCIRCUIT %d\n"
                 "ALIAS_HIT %d\nDIALOG_HIT %d\nSLOT_HIT %d\nTYPED_EN_HIT %d\nPREPARED %.400s\n"
                 "MAY_VOICE %d\nPEER %s\nACTION %s\nSTAGE_DRAFT %d\nCLAIMED_CERT %d\n",
                 r->source, r->skill[0] ? r->skill : "-", r->domain, r->miss,
                 r->shortcircuit, r->alias_hit, r->dialog_hit, r->slot_hit,
                 r->typed_en_hit,
                 r->prepared[0] ? r->prepared : "-", r->may_voice,
                 r->peer[0] ? r->peer : "-", r->action[0] ? r->action : "-",
                 r->stage_draft,
                 /* CERT is claimed only by a verified sealed answer. An action,
                  * a miss, or a stage draft never claims it. */
                 (r->verified && !r->miss && !r->stage_draft) ? 1 : 0);
    if (n > 0) (void)full_write(fd, head, (size_t)n);
    n = snprintf(utter, sizeof utter, "UTTERANCE %s\n",
                 r->utterance[0] ? r->utterance : "-");
    if (n > 0) (void)full_write(fd, utter, (size_t)n);
    if (r->stage_draft) {
        char st[CNET_SR_TEXT + 64];
        n = snprintf(st, sizeof st, "STAGE %s\n", r->stage);
        if (n > 0) (void)full_write(fd, st, (size_t)n);
    }
    n = snprintf(body, sizeof body, "ANSWER %s\nEND\n", r->answer);
    if (n > 0) (void)full_write(fd, body, (size_t)n);
}

/* ---- Marble Live action bus -------------------------------------------
 *
 * A closed allowlist of chat verbs, handled BEFORE the CERT path and answered
 * with SOURCE ACTION so an action can never be mistaken for a sealed skill.
 * Only three verbs reach here; `propose_capsule` is the only one that touches
 * argv, and it goes through cnet_ml_sanitize_unit first. Everything else falls
 * through to CERT as normal -- notably "who are you", which is a sealed soul
 * skill and must NOT be intercepted.
 *
 * propose != admit: this writes a pending row under var/capsule_inbox and
 * never seals anything.
 */
static const char *cd_after_mark(const char *q, const char **marks) {
    int i;
    for (i = 0; marks[i]; i++) {
        const char *p = strcasestr(q, marks[i]);
        if (p) {
            p += strlen(marks[i]);
            while (*p == ' ' || *p == ':') p++;
            if (*p) return p;
        }
    }
    return NULL;
}

static const char *kb_path_live(void) {
    static char p[CD_PATH];
    const char *e = getenv("CNET_KNOWLEDGE_PATH");
    const char *min = getenv("CNET_MINIMAL_ROOT");
    if (e && e[0]) return e;
    if (min && min[0]) {
        snprintf(p, sizeof p, "%s/var/marble_knowledge.jsonl", min);
        return p;
    }
    return "var/marble_knowledge.jsonl";
}

static int kb_ingest(const char *src, const char *text) {
    const char *path = kb_path_live();
    FILE *f;
    char dir[CD_PATH];
    char *sl;
    if (!text || !text[0]) return -1;
    snprintf(dir, sizeof dir, "%s", path);
    sl = strrchr(dir, '/');
    if (sl) {
        *sl = 0;
        mkdir_p(dir);
    }
    f = fopen(path, "a");
    if (!f) return -1;
    fprintf(f, "{\"ts\":%ld,\"source\":\"", (long)time(NULL));
    {
        const char *p = src ? src : "peer";
        for (; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', f);
            fputc(*p, f);
        }
    }
    fprintf(f, "\",\"chunk\":\"");
    {
        const char *p = text;
        int n = 0;
        for (; *p && n < 900; p++, n++) {
            if (*p == '"' || *p == '\\') fputc('\\', f);
            if (*p == '\n' || *p == '\r') fputc(' ', f);
            else fputc(*p, f);
        }
    }
    fprintf(f, "\",\"claimed_cert\":0,\"kind\":\"ingest\"}\n");
    fclose(f);
    return 0;
}

static int kb_recall(const char *topic, char *out, size_t cap) {
    const char *path = kb_path_live();
    FILE *f = fopen(path, "r");
    char line[2048];
    size_t o = 0;
    int hits = 0;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f) && hits < 4) {
        const char *c = strstr(line, "\"chunk\":\"");
        if (!c) continue;
        if (topic && topic[0] && !contains_ci(line, topic)) continue;
        c += 9;
        o += (size_t)snprintf(out + o, cap - o, "%s[%d] ", hits ? " " : "", hits + 1);
        while (*c && *c != '"' && o + 2 < cap) {
            if (*c == '\\' && c[1]) c++;
            out[o++] = *c++;
        }
        out[o] = 0;
        hits++;
        if (o + 8 >= cap) break;
    }
    fclose(f);
    return hits;
}

/* ---- ingested-notes recall, preferred over invented residual ----------
 *
 * kb_recall() matches the whole topic string as a substring, which is right for
 * an explicit "what do you know about X" but useless for ordinary chat. For the
 * open-chat path we need overlap: how many significant words of the question
 * appear in a stored note.
 *
 * This is what stops CNET inventing Bitcoin for "gold hash" when someone
 * already told it what a gold hash is here. Notes are NOT CERT -- the reply is
 * SOURCE INFO, miss=1, claimed_cert=0 -- but a note we were actually given
 * beats a fluent guess.
 *
 * The threshold matters more than the matching. A weak overlap that shadows
 * open chat with an irrelevant note is worse than the guess it replaced, so a
 * hit needs either two distinct significant words or one long (>=7 char)
 * specific term.
 */
#define KB_MAX_TOK 12
#define KB_MIN_TOK_LEN 4
#define KB_SPECIFIC_LEN 7

static int kb_tokens(const char *q, char tok[KB_MAX_TOK][64]) {
    static const char *stop[] = {"what", "when", "where", "which", "about",
                                 "does", "your", "they", "them", "this", "that",
                                 "with", "from", "have", "know", "tell", "please",
                                 "there", "their", "would", "could", "should",
                                 "like", "made", "make", "just", "really", "also",
                                 "been", "being", "very", "much", "such", "into",
                                 "over", "only", "even", "some", "more", "than",
                                 "then", "want", "need", "well", "will", "were",
                                 "youre", NULL};
    int n = 0, k;
    size_t i = 0;
    while (q[i] && n < KB_MAX_TOK) {
        size_t s;
        int isstop = 0;
        if (!isalnum((unsigned char)q[i])) { i++; continue; }
        s = i;
        while (q[i] && (isalnum((unsigned char)q[i]) || q[i] == '-')) i++;
        if (i - s < KB_MIN_TOK_LEN) continue;
        if (i - s > 63) continue;
        {
            size_t j;
            for (j = 0; j < i - s; j++)
                tok[n][j] = (char)tolower((unsigned char)q[s + j]);
            tok[n][i - s] = 0;
        }
        for (k = 0; stop[k]; k++)
            if (!strcmp(tok[n], stop[k])) { isstop = 1; break; }
        if (!isstop) {
            int dup = 0;
            for (k = 0; k < n; k++)
                if (!strcmp(tok[k], tok[n])) {
                    dup = 1;
                    break;
                }
            if (!dup) n++;
        }
    }
    return n;
}

/* Best-scoring notes for q. Returns the winning score (0 = no usable hit). */
static int kb_recall_overlap(const char *q, char *out, size_t cap) {
    char tok[KB_MAX_TOK][64];
    char best[3][900];
    int bscore[3] = {0, 0, 0};
    int ntok, i, j, nbest = 0, top = 0;
    FILE *f;
    char line[4096];
    if (out && cap) out[0] = 0;
    ntok = kb_tokens(q, tok);
    if (ntok == 0) return 0;
    f = fopen(kb_path_live(), "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        const char *c = strstr(line, "\"chunk\":\"");
        char chunk[900];
        size_t o = 0;
        int score = 0, specific = 0;
        if (!c) continue;
        c += 9;
        while (*c && *c != '"' && o + 2 < sizeof chunk) {
            if (*c == '\\' && c[1]) c++;
            chunk[o++] = *c++;
        }
        chunk[o] = 0;
        for (i = 0; i < ntok; i++) {
            if (!contains_word_ci(chunk, tok[i])) continue;
            score++;
            if (strlen(tok[i]) >= KB_SPECIFIC_LEN) specific = 1;
        }
        /* Threshold.
         *
         * The old rule -- two words OR one >=7-char word -- was too loose. A
         * stored note is often several hundred characters, so it contains a lot
         * of ordinary long words by chance: "good morning" matched a note about
         * the present simple tense purely because the note's example sentence
         * said "every morning", and the greeting came back as a grammar
         * lecture. Length is not rarity.
         *
         * Now: at least two distinct query words must appear, AND they must
         * cover at least half of the question's significant words. That keeps
         * "what is a gold hash in CNET" (3/3) and drops "good morning" (1/2)
         * and "I don't understand" (1/1 common word). A single-word query no
         * longer pulls notes at all -- the explicit "what do you know about X"
         * verb still serves that case, and deliberately.
         */
        (void)specific;
        if (score < 2) continue;
        if (score * 2 < ntok) continue;
        /* A distinctive query term (name / >=7) must appear in the note.
         * "Mokason made CNET" must not retrieve an English-grammar dump
         * that merely mentions CNET. */
        {
            int need_spec = 0, got_spec = 0;
            for (i = 0; i < ntok; i++) {
                if (strlen(tok[i]) < KB_SPECIFIC_LEN) continue;
                need_spec = 1;
                if (contains_word_ci(chunk, tok[i])) got_spec = 1;
            }
            if (need_spec && !got_spec) continue;
        }
        for (j = 0; j < 3; j++) {
            if (score > bscore[j]) {
                int k2;
                for (k2 = 2; k2 > j; k2--) {
                    bscore[k2] = bscore[k2 - 1];
                    memcpy(best[k2], best[k2 - 1], sizeof best[0]);
                }
                bscore[j] = score;
                snprintf(best[j], sizeof best[j], "%s", chunk);
                if (nbest < 3) nbest++;
                break;
            }
        }
    }
    fclose(f);
    if (nbest == 0) return 0;
    top = bscore[0];
    if (out && cap) {
        size_t o = 0;
        for (i = 0; i < nbest && bscore[i] > 0 && o + 16 < cap; i++)
            o += (size_t)snprintf(out + o, cap - o, "%s[%d] %.400s",
                                  i ? " " : "", i + 1, best[i]);
    }
    return top;
}

/* Repeat organic demand that we already have notes for is a promote candidate.
 * Count prior hits for this query; at N, drop a pending proposal. Never seals:
 * auto_cert=false, status pending_verify -- a human or the gold path decides. */
static int kb_note_hit(CdState *S, const char *q, const char *notes) {
    char dir[CD_PATH], hits[CD_PATH], line[1200], nq[256];
    FILE *f;
    int count = 1;
    int need = 3;
    const char *nenv = getenv("CNET_KB_PROPOSE_N");
    size_t i = 0, o = 0;
    if (nenv && nenv[0]) need = atoi(nenv);
    if (need < 1) need = 3;
    while (q[i] && o + 1 < sizeof nq) {
        if (isalnum((unsigned char)q[i]) || q[i] == ' ')
            nq[o++] = (char)tolower((unsigned char)q[i]);
        i++;
    }
    nq[o] = 0;
    snprintf(dir, sizeof dir, "%s", kb_path_live());
    { char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else snprintf(dir, sizeof dir, "."); }
    snprintf(hits, sizeof hits, "%.400s/kb_hits.jsonl", dir);
    f = fopen(hits, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            const char *p = strstr(line, "\"nq\":\"");
            if (!p) continue;
            if (!strncmp(p + 6, nq, strlen(nq)) && p[6 + strlen(nq)] == '"') count++;
        }
        fclose(f);
    }
    f = fopen(hits, "a");
    if (f) {
        fprintf(f, "{\"ts\":%ld,\"nq\":\"%.200s\",\"count\":%d}\n",
                (long)time(NULL), nq, count);
        fclose(f);
    }
    /* Exactly at the crossing, not every hit after it: >= would mint a fresh
     * proposal dir on every repeat ask (3 asks produced 3 identical dirs). */
    if (count != need) return count;
    /* enough repeat demand + we have notes -> propose (still not CERT) */
    {
        char pdir[CD_PATH], pj[CD_PATH];
        const char *root = getenv("CNET_MINIMAL_ROOT");
        snprintf(pdir, sizeof pdir, "%s/var/capsule_inbox/kbnote-%ld",
                 root && root[0] ? root : ".", (long)time(NULL));
        if (mkdir_p(pdir) != 0) return count;
        snprintf(pj, sizeof pj, "%.400s/PROPOSE.json", pdir);
        f = fopen(pj, "w");
        if (f) {
            fprintf(f,
                    "{\n  \"kind\": \"kb_note_promote\",\n"
                    "  \"query\": \"%.300s\",\n"
                    "  \"hits\": %d,\n  \"need\": %d,\n"
                    "  \"notes_preview\": \"%.300s\",\n"
                    "  \"auto_cert\": false,\n"
                    "  \"claimed_cert\": 0,\n"
                    "  \"status\": \"pending_verify\",\n"
                    "  \"law\": \"ingest_is_not_cert_propose_only\",\n"
                    "  \"ts\": %ld\n}\n",
                    nq, count, need, notes ? notes : "", (long)time(NULL));
            fclose(f);
        }
        (void)S;
    }
    return count;
}

static const char *cd_note_after(const char *q) {
    static const char *marks[] = {"remember that ", "remember this ",
                                  "please remember ", "remember ", NULL};
    return cd_after_mark(q, marks);
}

static const char *cd_learn_after(const char *q) {
    static const char *marks[] = {"learn this:", "learn this ", "learn:",
                                  "ingest this:", "ingest this ", NULL};
    return cd_after_mark(q, marks);
}

static const char *cd_know_after(const char *q) {
    static const char *marks[] = {"what do you know about ", "what do you know of ",
                                  NULL};
    return cd_after_mark(q, marks);
}

static void cd_action_reply(CdReply *out, const char *q, const char *skill,
                            const char *answer) {
    snprintf(out->source, sizeof out->source, "ACTION");
    snprintf(out->domain, sizeof out->domain, "ACTION");
    snprintf(out->skill, sizeof out->skill, "%s", skill);
    snprintf(out->action, sizeof out->action, "%s", skill);
    cd_scopy(out->prepared, sizeof out->prepared, q);
    snprintf(out->answer, sizeof out->answer, "%s", answer);
    snprintf(out->utterance, sizeof out->utterance, "%s", answer);
    out->verified = 0;   /* an action is done, not certified */
    out->miss = 0;
    out->may_voice = 1;
    out->tokens = 0;
}

/* Gold-file kick: skip the 30min timer and skip reviewer (gold_file already
 * skips it). Default ON; CNET_GOLD_KICK_EVOLVE=0 disables. Never admits. */
static int cd_gold_kick(void) {
    const char *en = getenv("CNET_GOLD_KICK_EVOLVE");
    pid_t pid;
    if (en && en[0] == '0') return 127;
    pid = fork();
    if (pid == 0) {
        execl("./bin/roe_evolve_tick", "roe_evolve_tick", "--gold-only",
              "--max-promotes", "1", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int st = 0;
        (void)waitpid(pid, &st, 0);
        return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    return -1;
}

static int cd_count_dirents(const char *path) {
    DIR *d;
    struct dirent *e;
    int n = 0;
    if (!path || !path[0]) return 0;
    d = opendir(path);
    if (!d) return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        n++;
    }
    closedir(d);
    return n;
}

/* Live inventory from bank/packs/capsule inbox. Not an I-can-X FAQ.
 * Tags are a SCAN of live bricks in cnet_serve_global() (sorted, at most
 * CD_CANDO_TAGS listed, rest folds to "+N more"). Never a hardcoded organ
 * list. Whole line stays under 500 chars so answer/utterance never cut it. */
#define CD_CANDO_TAGS 8
#define CD_CANDO_TAGBUF 200
#define CD_CANDO_LINE_MAX 500
static void cd_cando_tags(const CnetServeBank *b, char *out, size_t cap,
                          int *listed, int *live) {
    int idx[CNET_SERVE_MAX_BRICKS];
    int n = 0, i, j;
    size_t len = 0;
    *listed = 0;
    *live = 0;
    out[0] = 0;
    if (!b) return;
    for (i = 0; i < b->n && i < CNET_SERVE_MAX_BRICKS; i++) {
        if (!b->bricks[i].live || !b->bricks[i].tag[0]) continue;
        idx[n++] = i;
    }
    *live = n;
    /* readdir order is not stable across reloads; sort for a steady line */
    for (i = 1; i < n; i++) {
        int k = idx[i];
        j = i - 1;
        while (j >= 0 && strcmp(b->bricks[idx[j]].tag, b->bricks[k].tag) > 0) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = k;
    }
    for (i = 0; i < n && *listed < CD_CANDO_TAGS; i++) {
        const char *t = b->bricks[idx[i]].tag;
        size_t tl = strlen(t);
        size_t need = tl + (*listed ? 2u : 0u);
        if (len + need + 1 > cap) break;
        if (*listed) {
            memcpy(out + len, ", ", 2);
            len += 2;
        }
        memcpy(out + len, t, tl);
        len += tl;
        out[len] = 0;
        (*listed)++;
    }
}

static void cd_cando_line(CdState *S, char *out, size_t cap) {
    CnetServeBank *b = cnet_serve_global();
    int ncap = 0, listed = 0, live = 0;
    const char *min = getenv("CNET_MINIMAL_ROOT");
    char inbox[CD_PATH];
    char tags[CD_CANDO_TAGBUF];
    char more[24];
    if (!out || !cap) return;
    if (min && min[0]) {
        char var[CD_PATH];
        if (path_join2(var, sizeof var, min, "var") == 0 &&
            path_join2(inbox, sizeof inbox, var, "capsule_inbox") == 0)
            ncap = cd_count_dirents(inbox);
    }
    cd_cando_tags(b, tags, sizeof tags, &listed, &live);
    more[0] = 0;
    if (live > listed)
        snprintf(more, sizeof more, " +%d more", live - listed);
    {
        int nib = 0, comp = 0, oth = 0, i;
        if (b) {
            for (i = 0; i < b->n && i < CNET_SERVE_MAX_BRICKS; i++) {
                if (!b->bricks[i].live || !b->bricks[i].tag[0]) continue;
                if (!strncmp(b->bricks[i].tag, "q1_27", 5) ||
                    !strncmp(b->bricks[i].tag, "q1_add", 6) ||
                    !strncmp(b->bricks[i].tag, "q1_xor", 6) ||
                    !strncmp(b->bricks[i].tag, "q1_comp", 7))
                    nib++;
                else if (!strncmp(b->bricks[i].tag, "q1_p", 4))
                    comp++;
                else
                    oth++;
            }
        }
        if (cap > CD_CANDO_LINE_MAX) cap = CD_CANDO_LINE_MAX;
        if (listed > 0)
            snprintf(out, cap,
                     "Loaded %d/%d raw LUT bricks (%d nibble, %d compose, %d other): %s%s. %d packs on. "
                     "%d proposal inbox entries (not CERT). Raw tables are not certified capsules. "
                     "Try: convert N INPUT_TAG to OUTPUT_TAG. Unsupported inputs are refused.",
                     live, CNET_SERVE_MAX_BRICKS, nib, comp, oth, tags, more,
                     S ? S->n_always : 0, ncap);
        else
            snprintf(out, cap,
                     "Loaded 0/%d raw LUT bricks: none live. %d packs on. "
                     "%d proposal inbox entries (not CERT). This is inventory, not proof of capability. "
                     "Try: convert N INPUT_TAG to OUTPUT_TAG. Unsupported inputs are refused.",
                     CNET_SERVE_MAX_BRICKS, S ? S->n_always : 0, ncap);
    }
}

static int cd_action(CdState *S, const char *q, CdReply *out) {
    CnetSrAction act = cnet_sr_detect_action(q);
    const char *peer = S->dialog.peer_name[0] ? S->dialog.peer_name : "-";
    CnetFfiCmd fcmd;
    CnetFfiObs fobs;
    char ttag[64];
    unsigned tin = 0, tout = 0;

    /* The legacy preparation buffers are 512 bytes. Never answer a clipped
     * prefix. Explicit bounded MCP reads run before this legacy front door. */
    {
        char expanded[CNET_QA_OUT * 2];
        cnet_query_normalize(q, expanded, sizeof expanded);
        if (strlen(q) >= CNET_QA_OUT || strlen(expanded) >= CNET_QA_OUT) {
            cd_action_reply(out, q, "request_length_refusal_v1",
                "Please send a shorter request (under 512 bytes, including expanded contractions), "
                "or split it into separate questions. I will not answer a truncated question.");
            out->miss = 1;
            return 1;
        }
    }

    /* Closed read-only help: no learning launch or capability certification. */
    if (cnet_query_phrase_is_whole(q, "can you use those skills") ||
        cnet_query_phrase_is_whole(q, "how do i use your skills") ||
        cnet_query_phrase_is_whole(q, "what are you able to calculate")) {
        cd_action_reply(out, q, "skill_usage_v1",
            "Use: convert N INPUT_TAG to OUTPUT_TAG (for example, convert 173 bytes to bits). "
            "The loaded capsule must cover that input and conversion; otherwise I refuse. "
            "Ask 'what can you do' for the current inventory.");
        return 1;
    }
    if (cnet_query_phrase_is_whole(q, "can you learn") ||
        cnet_query_phrase_is_whole(q, "can you learn new things") ||
        cnet_query_phrase_is_whole(q, "how do you learn")) {
        cd_action_reply(out, q, "learning_help_v1",
            "New capsule knowledge needs external examples and independent verification before admission. "
            "My own answers are not training evidence. This reply does not start training or confirm a running learner. "
            "Name one specific skill and its inputs and outputs to define a learning task.");
        return 1;
    }
    if (cnet_query_phrase_is_whole(q, "any improvements") ||
        cnet_query_phrase_is_whole(q, "have you improved")) {
        char inventory[CD_CANDO_LINE_MAX];
        char msg[CNET_UTTER_TEXT];
        cd_cando_line(S, inventory, sizeof inventory);
        snprintf(msg, sizeof msg, "I cannot confirm improvement without a before/after evaluation. Current inventory: %s", inventory);
        cd_action_reply(out, q, "improvement_status_v1", msg);
        return 1;
    }
    if (cnet_query_phrase_is_whole(q, "still here") ||
        cnet_query_phrase_is_whole(q, "are you still here") ||
        cnet_query_phrase_is_whole(q, "are you there")) {
        cd_action_reply(out, q, "presence_check_v1", "Yes, I'm here. What would you like to work on?");
        return 1;
    }

    if (cnet_live_parse_teach(q, ttag, sizeof ttag, &tin, &tout) == 0) {
        int pairs = 0;
        float _lut[16];
        char msg[CNET_SR_TEXT];
        if (S->miss_log[0])
            (void)cnet_live_miss_append(S->miss_log, ttag, tin, 1, tout);
        if (S->miss_log[0])
            pairs = cnet_live_miss_domain_pairs(S->miss_log, ttag, _lut);
        if (pairs >= 16) {
            setenv("CNET_CORE_AUTO_EVOLVE", "1", 0);
            (void)core_evolve_run(S, 1);
            core_serve_try_reload();
            {
                char turn[96];
                snprintf(turn, sizeof turn, "%s %u", ttag, tin);
                if (core_try_serve(turn, out) == 0) return 1;
            }
        }
        snprintf(msg, sizeof msg,
                 "Taught %s %u->%u (%d/16). Not CERT until 16/16.", ttag, tin,
                 tout, pairs);
        snprintf(out->source, sizeof out->source, "CNET");
        snprintf(out->domain, sizeof out->domain, "TEACH");
        snprintf(out->skill, sizeof out->skill, "typed_teach_pending");
        out->action[0] = 0;
        cd_scopy(out->prepared, sizeof out->prepared, q);
        snprintf(out->answer, sizeof out->answer, "%s", msg);
        snprintf(out->utterance, sizeof out->utterance, "%.767s", msg);
        out->verified = 0;
        out->miss = 1;
        out->may_voice = 1;
        out->tokens = 0;
        return 1;
    }

    /* Live mood: feeling word from neuromod, not meter dump. */
    {
        FILE *nf = fopen("logs/governor/neuromod_state.json", "r");
        if (nf) {
            char nbuf[4096];
            size_t nn = fread(nbuf, 1, sizeof nbuf - 1, nf);
            fclose(nf);
            if (nn) {
                nbuf[nn] = 0;
                (void)cnet_sr_ingest_neuromod(&S->show, nbuf, nn);
            }
        }
        cnet_sr_session_overlay(&S->show);
    }
    if (cnet_sr_mood_query(q)) {
        char line[CNET_SR_TEXT];
        cnet_sr_mood_line(&S->show, line, sizeof line);
        snprintf(out->source, sizeof out->source, "LOCAL");
        snprintf(out->domain, sizeof out->domain, "PRESENCE");
        snprintf(out->skill, sizeof out->skill, "mood_now_v1");
        out->action[0] = 0;
        cd_scopy(out->prepared, sizeof out->prepared, q);
        snprintf(out->answer, sizeof out->answer, "%s", line);
        snprintf(out->utterance, sizeof out->utterance, "%s", line);
        out->verified = 0;
        out->miss = 0;
        out->may_voice = 1;
        out->tokens = 0;
        return 1;
    }
    if (cnet_sr_mood_follow_query(q)) {
        char line[CNET_SR_TEXT];
        cnet_sr_mood_follow_line(&S->show, line, sizeof line);
        snprintf(out->source, sizeof out->source, "LOCAL");
        snprintf(out->domain, sizeof out->domain, "PRESENCE");
        snprintf(out->skill, sizeof out->skill, "mood_now_v1");
        out->action[0] = 0;
        cd_scopy(out->prepared, sizeof out->prepared, q);
        snprintf(out->answer, sizeof out->answer, "%s", line);
        snprintf(out->utterance, sizeof out->utterance, "%s", line);
        out->verified = 0;
        out->miss = 0;
        out->may_voice = 1;
        out->tokens = 0;
        return 1;
    }
    if (cnet_sr_mood_why_query(q)) {
        char line[CNET_SR_TEXT];
        cnet_sr_mood_why_line(&S->show, line, sizeof line);
        snprintf(out->source, sizeof out->source, "LOCAL");
        snprintf(out->domain, sizeof out->domain, "PRESENCE");
        snprintf(out->skill, sizeof out->skill, "mood_why_v1");
        out->action[0] = 0;
        cd_scopy(out->prepared, sizeof out->prepared, q);
        snprintf(out->answer, sizeof out->answer, "%s", line);
        snprintf(out->utterance, sizeof out->utterance, "%s", line);
        out->verified = 0;
        out->miss = 0;
        out->may_voice = 1;
        out->tokens = 0;
        return 1;
    }
    if (cnet_sr_want_query(q)) {
        char line[CNET_SR_TEXT];
        cnet_sr_want_line(&S->show, NULL, line, sizeof line);
        snprintf(out->source, sizeof out->source, "LOCAL");
        snprintf(out->domain, sizeof out->domain, "PRESENCE");
        snprintf(out->skill, sizeof out->skill, "want_now_v1");
        out->action[0] = 0;
        cd_scopy(out->prepared, sizeof out->prepared, q);
        snprintf(out->answer, sizeof out->answer, "%s", line);
        snprintf(out->utterance, sizeof out->utterance, "%s", line);
        out->verified = 0;
        out->miss = 0;
        out->may_voice = 1;
        out->tokens = 0;
        return 1;
    }
    if (cnet_sr_cando_query(q)) {
        char line[CNET_SR_TEXT];
        cd_cando_line(S, line, sizeof line);
        snprintf(out->source, sizeof out->source, "LOCAL");
        snprintf(out->domain, sizeof out->domain, "PRESENCE");
        snprintf(out->skill, sizeof out->skill, "can_do_v1");
        out->action[0] = 0;
        cd_scopy(out->prepared, sizeof out->prepared, q);
        snprintf(out->answer, sizeof out->answer, "%s", line);
        snprintf(out->utterance, sizeof out->utterance, "%s", line);
        out->verified = 0;
        out->miss = 0;
        out->may_voice = 1;
        out->tokens = 0;
        return 1;
    }
    {
        unsigned sec = 0;
        char text[CNET_SR_TEXT];
        if (cnet_sr_remind_parse(q, &sec, text, sizeof text) == 0) {
            char msg[CNET_SR_TEXT];
            if (cnet_sr_remind_add(S->remind_path, (unsigned long)time(NULL), sec,
                                  text) != 0)
                snprintf(msg, sizeof msg, "Could not schedule.");
            else
                snprintf(msg, sizeof msg, "Scheduled in %us: %.200s", sec, text);
            cd_action_reply(out, q, "commit_clock_v1", msg);
            return 1;
        }
    }
    if (strcmp(q, "reminders") == 0 || strcmp(q, "what reminders") == 0 ||
        strcmp(q, "any reminders") == 0 || strcmp(q, "due reminders") == 0) {
        char line[CNET_SR_TEXT];
        unsigned long now = (unsigned long)time(NULL);
        if (strcmp(q, "due reminders") == 0) {
            if (cnet_sr_remind_due(S->remind_path, now, line, sizeof line) != 0)
                snprintf(line, sizeof line, "Nothing due.");
            else {
                char due[CNET_SR_TEXT];
                snprintf(due, sizeof due, "Due: %.200s", line);
                snprintf(line, sizeof line, "%s", due);
            }
        } else {
            (void)cnet_sr_remind_list(S->remind_path, now, line, sizeof line);
        }
        cd_action_reply(out, q, "commit_clock_v1", line);
        return 1;
    }
    {
        const char *rest = NULL;
        if (strncmp(q, "look up ", 8) == 0) rest = q + 8;
        else if (strncmp(q, "lookup ", 7) == 0) rest = q + 7;
        else if (strncmp(q, "wiki ", 5) == 0) rest = q + 5;
        if (rest && rest[0]) {
            CnetSkillLaneResult lane;
            memset(&lane, 0, sizeof lane);
            if (cnet_ood_try_wiki(rest, 0, &lane) == 0 &&
                (lane.spoken[0] || lane.value[0])) {
                const char *sp = lane.spoken[0] ? lane.spoken : lane.value;
                snprintf(out->source, sizeof out->source, "INFO");
                snprintf(out->domain, sizeof out->domain, "LOOKUP");
                snprintf(out->skill, sizeof out->skill, "lookup_hop_v1");
                out->action[0] = 0;
                cd_scopy(out->prepared, sizeof out->prepared, q);
                snprintf(out->answer, sizeof out->answer, "%.767s", sp);
                snprintf(out->utterance, sizeof out->utterance, "%.767s", sp);
                out->verified = 0;
                out->miss = 0;
                out->may_voice = 1;
                out->tokens = 0;
                return 1;
            }
            cd_action_reply(out, q, "lookup_hop_v1",
                            "Lookup missed. Not sealed.");
            return 1;
        }
    }

    /* Presence greet/ack. Register once; not soul_who / leftover teach.
     * Do not color_utterance — that doubles Sharp/Heard onto the line. */
    if (cnet_sr_greet_query(q) || cnet_sr_ack_query(q)) {
        char line[CNET_SR_TEXT];
        if (cnet_sr_greet_query(q))
            cnet_sr_greet_line(&S->show, line, sizeof line);
        else
            cnet_sr_ack_line(&S->show, line, sizeof line);
        snprintf(out->source, sizeof out->source, "LOCAL");
        /* Presence is a hit, not a claim: DOMAIN PRESENCE, verified 0,
         * so CLAIMED_CERT is 0 without the "DOMAIN CERT + CERT 0" ambiguity. */
        snprintf(out->domain, sizeof out->domain, "PRESENCE");
        snprintf(out->skill, sizeof out->skill,
                 cnet_sr_greet_query(q) ? "presence_greet_v1" : "presence_ack_v1");
        out->action[0] = 0;
        cd_scopy(out->prepared, sizeof out->prepared, q);
        snprintf(out->answer, sizeof out->answer, "%s", line);
        snprintf(out->utterance, sizeof out->utterance, "%s", line);
        out->verified = 0;
        out->miss = 0;
        out->may_voice = 1;
        out->tokens = 0;
        return 1;
    }

    /* Sealed identity slot — cheap front door. Pack walk was ~3s. */
    {
        char nrm[CNET_QA_OUT], tmp[CNET_QA_OUT];
        CnetQueryPrepareMeta am;
        int who = 0, op = 0;
        cnet_query_normalize(q, nrm, sizeof nrm);
        if (cnet_query_phrase_is_whole(q, "who are you") || cnet_query_identity_bot(q))
            who = 1;
        if (cnet_query_phrase_is_whole(q, "who made you") ||
            cnet_query_phrase_is_whole(q, "who created you") ||
            cnet_query_phrase_is_whole(q, "who is your operator"))
            op = 1;
        memset(&am, 0, sizeof am);
        cnet_query_prepare(&S->aliases, q, tmp, sizeof tmp, &am);
        if (strlen(q) < CNET_QA_OUT && am.alias_hit &&
            cnet_query_phrase_is_whole(tmp, "who are you"))
            who = 1;
        if (strlen(q) < CNET_QA_OUT && am.alias_hit &&
            cnet_query_phrase_is_whole(tmp, "who is your operator"))
            op = 1;
        if (strlen(q) >= CNET_QA_OUT) op = 0;
        if (op) {
            static const char *oper = "Mokason.";
            snprintf(out->source, sizeof out->source, "LOCAL");
            snprintf(out->domain, sizeof out->domain, "CERT");
            snprintf(out->skill, sizeof out->skill, "soul_operator");
            out->action[0] = 0;
            cd_scopy(out->prepared, sizeof out->prepared, q);
            snprintf(out->answer, sizeof out->answer, "%s", oper);
            snprintf(out->utterance, sizeof out->utterance, "%s", oper);
            out->verified = 1;
            out->miss = 0;
            out->may_voice = 1;
            out->tokens = 0;
            return 1;
        }
        if (who) {
            static const char *who_line =
                "I am Marble. Peer on this machine. Not Jarvis.";
            snprintf(out->source, sizeof out->source, "LOCAL");
            snprintf(out->domain, sizeof out->domain, "CERT");
            snprintf(out->skill, sizeof out->skill, "soul_who");
            out->action[0] = 0;
            cd_scopy(out->prepared, sizeof out->prepared, q);
            snprintf(out->answer, sizeof out->answer, "%s", who_line);
            snprintf(out->utterance, sizeof out->utterance, "%s", who_line);
            out->verified = 1;
            out->miss = 0;
            out->may_voice = 1;
            out->tokens = 0;
            return 1;
        }
    }

    /* New-kind domain table (not another nibble LUT). */
    {
        char nrm[CNET_QA_OUT];
        cnet_query_normalize(q, nrm, sizeof nrm);
        if (strncmp(nrm, "room ", 5) == 0 || strcmp(nrm, "room") == 0) {
            const char *rid = (nrm[0] && nrm[4] == ' ') ? nrm + 5 : "";
            static const struct {
                const char *k;
                const char *v;
            } R[] = {{"start", "0"}, {"gate", "1"}, {"hall", "2"}, {NULL, NULL}};
            int i, hit = 0;
            const char *val = "outside_table_abstain";
            for (i = 0; R[i].k; i++) {
                if (strcmp(rid, R[i].k) == 0) {
                    val = R[i].v;
                    hit = 1;
                    break;
                }
            }
            snprintf(out->source, sizeof out->source, "LOCAL");
            snprintf(out->domain, sizeof out->domain, hit ? "CERT" : "ABSTAIN");
            snprintf(out->skill, sizeof out->skill, "room_id_v1");
            out->action[0] = 0;
            cd_scopy(out->prepared, sizeof out->prepared, q);
            snprintf(out->answer, sizeof out->answer, "%s", val);
            snprintf(out->utterance, sizeof out->utterance, "%s", val);
            out->verified = hit ? 1 : 0;
            out->miss = hit ? 0 : 1;
            out->may_voice = 1;
            out->tokens = 0;
            return 1;
        }
    }

    /* Path-4 live LUT compose: compose lut TAGA TAGB [as TAGOUT] */
    if (strncmp(q, "compose lut ", 12) == 0 ||
        strncmp(q, "compose bricks ", 15) == 0) {
        char a[32], btag[32], outtag[32];
        const char *p = q;
        CnetServeBank *bank;
        int n;
        a[0] = btag[0] = 0;
        snprintf(outtag, sizeof outtag, "q1_comp");
        if (strncmp(p, "compose lut ", 12) == 0)
            p += 12;
        else
            p += 15;
        n = sscanf(p, "%31s %31s as %31s", a, btag, outtag);
        if (n < 2)
            n = sscanf(p, "%31s %31s", a, btag);
        if (n < 2 || !a[0] || !btag[0]) {
            cd_action_reply(out, q, "compose_lut",
                            "Usage: compose lut TAGA TAGB [as TAGOUT]");
            return 1;
        }
        (void)cnet_serve_global_load_env();
        bank = cnet_serve_global();
        if (!bank || cnet_serve_compose_tags(bank, a, btag, outtag) != 0) {
            cd_action_reply(out, q, "compose_lut",
                            "Compose failed (need two live .lut tags). Not CERT.");
            return 1;
        }
        {
            char msg[CNET_SR_TEXT];
            snprintf(msg, sizeof msg,
                     "Composed %s then %s -> %s (lut_b[lut_a[i]]). Ask %s n. Not leftover.",
                     a, btag, outtag, outtag);
            cd_action_reply(out, q, "compose_lut", msg);
        }
        return 1;
    }

    /* Closed FFI verbs. SOURCE ACTION, never CERT. */
    if (cnet_ffi_parse(q, &fcmd, &fobs)) {
        char msg[CNET_SR_TEXT];
        if (fcmd == CNET_FFI_CMD_RESET) {
            cnet_ffi_init_ports(&S->ffi);
            cd_action_reply(out, q, "ffi_reset",
                            "FFI converter reset. Residual, admitted=0.");
            return 1;
        }
        if (fcmd == CNET_FFI_CMD_STATUS) {
            (void)cnet_ffi_complete(&S->ffi);
            cnet_ffi_status_line(&S->ffi, msg, sizeof msg);
            cd_action_reply(out, q, "ffi_status", msg);
            return 1;
        }
        if (fcmd == CNET_FFI_CMD_NOTE) {
            if (!fobs.site[0] || !fobs.sent[0]) {
                cd_action_reply(out, q, "ffi_note",
                                "Usage: ffi note site=<site> sent=<lemma> [got=<form>]");
                return 1;
            }
            if (cnet_ffi_note(&S->ffi, &fobs) != 0) {
                snprintf(msg, sizeof msg, "FFI note refused (%s). Not sealed.",
                         S->ffi.reason);
                cd_action_reply(out, q, "ffi_note", msg);
                return 1;
            }
            if (cnet_ffi_complete(&S->ffi))
                snprintf(msg, sizeof msg,
                         "FFI complete for %s -> %s. Propose with: ffi propose. "
                         "Not sealed.",
                         fobs.sent, fobs.got[0] ? fobs.got : "-");
            else
                snprintf(msg, sizeof msg,
                         "FFI noted residual %s (incomplete). Fill got= to complete. "
                         "Not sealed.",
                         fobs.sent);
            cd_action_reply(out, q, "ffi_note", msg);
            return 1;
        }
        if (fcmd == CNET_FFI_CMD_PROPOSE) {
            char inbox[CD_PATH], dir[CD_PATH];
            const char *min = getenv("CNET_MINIMAL_ROOT");
            if (!cnet_ffi_complete(&S->ffi)) {
                snprintf(msg, sizeof msg,
                         "FFI incomplete (%s). Convert refused. Not sealed.",
                         S->ffi.reason[0] ? S->ffi.reason : "no_obs");
                cd_action_reply(out, q, "ffi_propose", msg);
                return 1;
            }
            if (min && min[0])
                snprintf(inbox, sizeof inbox, "%s/var/capsule_inbox", min);
            else
                snprintf(inbox, sizeof inbox, "var/capsule_inbox");
            (void)mkdir_p(inbox);
            {
                char leaf[40];
                int nleaf = snprintf(leaf, sizeof leaf, "ffi-%ld", (long)time(NULL));
                size_t ilen = strlen(inbox);
                if (nleaf < 0 || (size_t)nleaf >= sizeof leaf ||
                    ilen + 1 + (size_t)nleaf + 1 > sizeof dir) {
                    cd_action_reply(out, q, "ffi_propose",
                                    "FFI propose path too long. Not sealed.");
                    return 1;
                }
                memcpy(dir, inbox, ilen);
                dir[ilen] = '/';
                memcpy(dir + ilen + 1, leaf, (size_t)nleaf + 1);
            }
            if (cnet_ffi_propose(&S->ffi, dir) != 0) {
                cd_action_reply(out, q, "ffi_propose",
                                "FFI propose failed. Not sealed.");
                return 1;
            }
            cd_action_reply(out, q, "ffi_propose",
                            "Proposed FFI row pending_verify auto_cert=0 admitted=0. "
                            "I proposed it; I did not seal it.");
            return 1;
        }
        if (fcmd == CNET_FFI_CMD_GOLD) {
            char gp[CD_PATH];
            if (!cnet_ffi_complete(&S->ffi)) {
                snprintf(msg, sizeof msg,
                         "FFI incomplete (%s). Gold refused. Not sealed.",
                         S->ffi.reason[0] ? S->ffi.reason : "no_obs");
                cd_action_reply(out, q, "ffi_gold", msg);
                return 1;
            }
            gp[0] = 0;
            if (cnet_ffi_write_gold(&S->ffi, NULL, gp, sizeof gp) != 0) {
                snprintf(msg, sizeof msg, "FFI gold refused (%s). Not sealed.",
                         S->ffi.reason);
                cd_action_reply(out, q, "ffi_gold", msg);
                return 1;
            }
            cd_action_reply(out, q, "ffi_gold",
                            "Gold file written for evolve gold_file path. "
                            "auto_cert=0 admitted=0. I did not seal it.");
            return 1;
        }
        return 0;
    }

    {
        CnetGoldReq gr;
        if (cnet_gold_parse(q, &gr)) {
            char gq[CNET_GOLD_Q], gp[CNET_GOLD_PATH], why[CNET_GOLD_REASON];
            char msg[CNET_SR_TEXT];
            const char *bl = getenv("CNET_BLOCKLIST");
            char blbuf[CD_PATH];
            gq[0] = 0;
            if (!bl || !bl[0]) {
                snprintf(blbuf, sizeof blbuf, "config/promote_blocklist.txt");
                bl = blbuf;
            }
            if (gr.cmd == CNET_GOLD_CMD_LAST) {
                if (!S->last_miss_q[0]) {
                    cd_action_reply(out, q, "gold",
                                    "No last organic miss this session. "
                                    "Ask first, then gold last <answer>. Not sealed.");
                    return 1;
                }
                snprintf(gq, sizeof gq, "%s", S->last_miss_q);
            } else {
                snprintf(gq, sizeof gq, "%s", gr.query);
            }
            if (cnet_gold_write(S->root, gq, gr.answer, bl, 0, gp, sizeof gp, why,
                                sizeof why) != 0) {
                snprintf(msg, sizeof msg, "Gold refused (%s). Not sealed.",
                         why[0] ? why : "write");
                cd_action_reply(out, q, "gold", msg);
                return 1;
            }
            {
                char ov[CD_PATH];
                int ovn = 0;
                const char *gfn = "en_irregular_gold.tsv";
                CnetTeResult te;
                if (cnet_te_ask(gq, &te) == 0 && te.grammar_hit && te.n_cand >= 1) {
                    if (te.cand[0].kind == CNET_TE_PLURAL)
                        gfn = "en_plurals_gold.tsv";
                    else if (te.cand[0].kind == CNET_TE_ART)
                        gfn = "en_articles_gold.tsv";
                    else if (te.cand[0].kind == CNET_TE_GERUND)
                        gfn = "en_gerunds_gold.tsv";
                    else if (te.cand[0].kind == CNET_TE_S3)
                        gfn = "en_3sg_gold.tsv";
                    else if (te.cand[0].kind == CNET_TE_COMP)
                        gfn = "en_comparatives_gold.tsv";
                    else if (te.cand[0].kind == CNET_TE_SUPER)
                        gfn = "en_superlatives_gold.tsv";
                    else if (te.cand[0].kind == CNET_TE_ADV)
                        gfn = "en_adverbs_gold.tsv";
                }
                if (path_join2(ov, sizeof ov, S->root, gfn) == 0)
                    ovn = cnet_te_gold_apply(gq, gr.answer, ov);
                if (ovn != 1 &&
                    path_join2(ov, sizeof ov, S->root, "en_numerals.tsv") == 0)
                    ovn = cnet_ood_gold_numeral(gq, gr.answer, ov);
                if (ovn == 1) {
                    snprintf(msg, sizeof msg,
                             "Gold written for '%s'. Typed overlay applied. "
                             "auto_cert=0. This ACTION is not CERT.",
                             gq);
                } else {
                    int krc = cd_gold_kick();
                    if (krc == 0)
                        snprintf(msg, sizeof msg,
                                 "Gold written for '%s'. Kicked evolve --gold-only. "
                                 "auto_cert=0. This ACTION is not CERT.",
                                 gq);
                    else
                        snprintf(msg, sizeof msg,
                                 "Gold written for '%s'. Kick rc=%d. "
                                 "Run bin/roe_evolve_tick --gold-only. Not sealed.",
                                 gq, krc);
                }
            }
            cd_action_reply(out, q, "gold", msg);
            return 1;
        }
    }

    if (act == CNET_SR_ACT_REMEMBER) {
        const char *note = cd_note_after(q);
        char msg[CNET_SR_NOTE + 96];
        char mdpath[CNET_MD_MEM_PATH];
        if (!note) return 0;
        if (cnet_sr_remember(&S->show, peer, note, 0) != 0) return 0;
        (void)cnet_md_mem_store(peer, note);
        mdpath[0] = 0;
        (void)cnet_md_mem_path(mdpath, sizeof mdpath);
        snprintf(msg, sizeof msg,
                 "Noted (not CERT). Stored in markdown. I will keep: %.160s",
                 note);
        cd_action_reply(out, q, "remember_note", msg);
        return 1;
    }
    if (act == CNET_SR_ACT_RECALL) {
        char hits[5][CNET_SR_NOTE];
        char mdhit[CNET_MD_MEM_NOTE];
        char msg[CNET_SR_TEXT];
        int n = cnet_sr_recall(&S->show, NULL, hits, 5), i;
        size_t o = 0;
        if (cnet_md_mem_recall(NULL, mdhit, sizeof mdhit) == 0) {
            snprintf(msg, sizeof msg, "From memory.md (not CERT): %.400s", mdhit);
            cd_action_reply(out, q, "recall_notes", msg);
            return 1;
        }
        if (n <= 0) {
            cd_action_reply(out, q, "recall_notes",
                            "Nothing remembered yet. Teach with: remember that ...");
            return 1;
        }
        o = (size_t)snprintf(msg, sizeof msg, "I remember %d:", n);
        for (i = 0; i < n && o + 4 < sizeof msg; i++)
            o += (size_t)snprintf(msg + o, sizeof msg - o, " (%d) %.160s", i + 1, hits[i]);
        cd_action_reply(out, q, "recall_notes", msg);
        return 1;
    }
    if (act == CNET_SR_ACT_PROPOSE_CAPSULE) {
        char unit[CNET_ML_UNIT], sum[CNET_ML_TEXT], msg[CNET_SR_TEXT];
        if (cnet_ml_extract_unit(q, unit, sizeof unit) != 0) {
            cd_action_reply(out, q, "propose_capsule",
                            "Name the unit: \"propose capsule for <name>\".");
            return 1;
        }
        if (cnet_ml_propose_capsule(unit, sum, sizeof sum) != 0) {
            snprintf(msg, sizeof msg,
                     "Could not stage a proposal for %s.", unit);
            cd_action_reply(out, q, "propose_capsule", msg);
            return 1;
        }
        snprintf(msg, sizeof msg,
                 "Proposed capsule %s - pending verify, auto_cert=0. "
                 "I proposed it; I did not seal it. %.300s",
                 unit, sum);
        cd_action_reply(out, q, "propose_capsule", msg);
        return 1;
    }
    if (act == CNET_SR_ACT_LEARN) {
        const char *body = cd_learn_after(q);
        char msg[CNET_SR_TEXT];
        if (!body) return 0;
        if (kb_ingest(peer, body) != 0) {
            cd_action_reply(out, q, "learn_info",
                            "Could not store that information.");
            return 1;
        }
        (void)cnet_md_mem_store(peer, body);
        snprintf(msg, sizeof msg,
                 "Ingested (not certified). I can recall it; I will not fake a seal. "
                 "Noted: %.240s",
                 body);
        cd_action_reply(out, q, "learn_info", msg);
        return 1;
    }
    if (act == CNET_SR_ACT_KNOW) {
        const char *topic = cd_know_after(q);
        char found[CNET_SR_TEXT];
        char msg[CNET_SR_TEXT];
        int n;
        if (!topic) topic = "";
        if (cnet_md_mem_recall(topic[0] ? topic : NULL, found, sizeof found) == 0) {
            snprintf(msg, sizeof msg, "From memory.md (not CERT): %.400s", found);
            cd_action_reply(out, q, "know_about", msg);
            return 1;
        }
        n = kb_recall(topic, found, sizeof found);
        if (n <= 0) {
            snprintf(msg, sizeof msg,
                     "No ingested notes on \"%s\" yet. Teach me with: learn this: ...",
                     topic[0] ? topic : "that");
            cd_action_reply(out, q, "know_about", msg);
            return 1;
        }
        snprintf(msg, sizeof msg, "From ingested notes (not CERT): %.400s", found);
        cd_action_reply(out, q, "know_about", msg);
        return 1;
    }
    if (act == CNET_SR_ACT_GAPS) {
        char found[CNET_SR_TEXT];
        char msg[CNET_SR_TEXT];
        if (cnet_md_mem_list_gaps(found, sizeof found) != 0) {
            cd_action_reply(out, q, "list_gaps",
                            "No open gaps logged. Unknowns get logged when I miss.");
            return 1;
        }
        snprintf(msg, sizeof msg, "Open gaps (not CERT): %.400s", found);
        cd_action_reply(out, q, "list_gaps", msg);
        return 1;
    }
    return 0;   /* identity / status / none -> CERT path owns it */
}

/* Queue leftover miss for the gpt-sol improver. Classifier in
 * scripts/cnet_gpt_sol_improve.sh drops encyclopedia / presence.
 * Never CERT. Never gold. */
static void cd_gpt_sol_enqueue(CdState *S, const char *q, const CdReply *rep) {
    FILE *f;
    char esc[400];
    size_t i = 0;
    const unsigned char *s;
    if (!S || !S->gpt_sol_jobs[0] || !q || !q[0] || !rep) return;
    if (!rep->miss) return;
    for (s = (const unsigned char *)q; *s && i + 2 < sizeof esc; s++) {
        if (*s == '"' || *s == '\\') {
            if (i + 3 >= sizeof esc) break;
            esc[i++] = '\\';
        }
        if (*s == '\n' || *s == '\r') continue;
        esc[i++] = (char)*s;
    }
    esc[i] = 0;
    f = fopen(S->gpt_sol_jobs, "a");
    if (!f) return;
    fprintf(f, "{\"ts\":%ld,\"query\":\"%s\",\"source\":\"%s\"}\n",
            (long)time(NULL), esc, rep->source[0] ? rep->source : "MISS");
    fclose(f);
}

/* Nanny kick: leftover is a hole for gpt-sol, not a user teach-TAG prompt.
 * Double-fork so Discord turn never waits on Codex. 10 min cooldown. */
static void cd_gpt_sol_kick(void) {
    static time_t last;
    time_t now = time(NULL);
    pid_t pid;
    if (last && (now - last) < 600) return;
    last = now;
    pid = fork();
    if (pid == 0) {
        pid_t g = fork();
        if (g != 0) _exit(0);
        (void)setsid();
        execl("/usr/bin/systemctl", "systemctl", "--user", "start",
              "--no-block", "cnet-gpt-sol-improve.service", (char *)0);
        _exit(127);
    }
    if (pid > 0) (void)waitpid(pid, NULL, 0);
}

static int cd_nanny_shape(const char *q) {
    if (!q || !q[0]) return 0;
    if (cnet_sr_cando_query(q) || cnet_sr_mood_query(q) ||
        cnet_sr_mood_follow_query(q) || cnet_sr_want_query(q) ||
        cnet_sr_greet_query(q) || cnet_sr_ack_query(q))
        return 0;
    return contains_ci(q, "lut") || contains_ci(q, "tensor") ||
           contains_ci(q, "brick") || contains_ci(q, "capsule") ||
           contains_ci(q, "compose") || contains_ci(q, "organ") ||
           contains_ci(q, "q1_");
}

/* Cheap identity gate on a PLAY draft. Full-length, case-insensitive scan
 * (contains_ci truncates at 512). No HTTP, no allocation. */
static int cd_strcasestr(const char *hay, const char *needle) {
    size_t nlen, i, j;
    if (!hay || !needle || !needle[0]) return 0;
    nlen = strlen(needle);
    for (i = 0; hay[i]; i++) {
        for (j = 0; j < nlen; j++) {
            if (!hay[i + j] ||
                tolower((unsigned char)hay[i + j]) !=
                    tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/* Identity is sealed (soul_who, law door). A draft that improvises who it is
 * would unseal identity in practice; drop it and say "Not sealed." instead. */
static int cd_stage_identity_leak(const char *draft) {
    if (!draft || !draft[0]) return 0;
    return cd_strcasestr(draft, "I am ") ||
           cd_strcasestr(draft, "I'm a language model") ||
           cd_strcasestr(draft, "I'm an AI");
}

/* PLAY-door bouncer. The INVERSE of cd_skip_teacher():
 *   law door  -- front-owned (probe/slot/alias/sealed pack/lut/arith/identity)
 *                may reach the teacher; a miss there is a DEFECT, never play;
 *   play door -- everything else that is not factory vocabulary (nanny shape)
 *                and not an explicit fact request (look up/wiki) MAY be
 *                improvised on, in its own field (stage/utterance).
 * Order is cheapest-first so a closed door (player SKU, stage off, cooldown)
 * never pays for cd_front_owns; leftover_fast stays <5ms. */
static int cd_play_may_speak(const CdState *S, const char *q) {
    if (!S || !q || !q[0]) return 0;
    if (cd_player_sku()) return 0;
    if (!cnet_ml_stage_enabled()) return 0;
    if (!cnet_sr_cooldown_ok(&S->show, (unsigned)time(NULL))) return 0;
    if (cd_nanny_shape(q)) return 0;
    if (cd_explicit_lookup(q)) return 0;
    if (cd_front_owns(S, q)) return 0;
    /* PLAY is English riff, not a color organ. Preference/choice asks may STAGE.
     * Encyclopedia / tokyo / json / learn-think stay leftover_fast. */
    if (contains_ci(q, "what do you think") || contains_ci(q, "how does that feel") ||
        contains_ci(q, "tell me a story") || contains_ci(q, "sing me") ||
        contains_ci(q, "a poem") || contains_ci(q, "imagine ") ||
        contains_ci(q, "a joke") || contains_ci(q, "riff ") ||
        contains_ci(q, "would you pick") || contains_ci(q, "what would you pick") ||
        contains_ci(q, "pick between") || contains_ci(q, "which would you") ||
        contains_ci(q, "would you rather") || contains_ci(q, "red or blue"))
        return 1;
    return 0;
}

/* Miss that is leftover (STAGE/CORE/LLM) is a known unknown. Log the gap.
 * Lookup draft stays this-turn mouth. Never stored as a fact. Never CERT. */
static void cd_gap_learn(CdState *S, const char *q, const CdReply *rep) {
    if (!q || !q[0] || !rep) return;
    if (rep->capsule_handled || !rep->miss || rep->shortcircuit || rep->typed_en_hit ||
        rep->arith_hit || rep->brick_hit || rep->solver_hit)
        return;
    if (strcmp(rep->source, "LOCAL") == 0 || strcmp(rep->source, "ACTION") == 0 ||
        strcmp(rep->source, "INFO") == 0 || strcmp(rep->source, "MCP") == 0)
        return;
    if (cnet_probe_match(&S->probes, q, NULL, 0)) return;
    (void)cnet_md_mem_gap(S->dialog.peer_name, q, NULL);
    if (cd_nanny_shape(q)) {
        cd_gpt_sol_enqueue(S, q, rep);
        cd_gpt_sol_kick();
    }
}

/* Leftover STAGE/CORE/LLM: ANSWER is never an encyclopedia. Two-door: the
 * draft lives in its own field. It is KEPT for SOURCE STAGE (and the
 * in-process OPEN_CHAT plane under CORE) -- ANSWER is starved to "Not sealed."
 * while stage/utterance carry the draft. It is CLEARED for SOURCE LLM: the
 * teacher is never the mouth, on either door. */
static void cd_starve_leftover(CdState *S, const char *q, CdReply *rep) {
    int keep_draft;
    if (!rep || !rep->miss) return;
    if (strcmp(rep->source, "STAGE") != 0 && strcmp(rep->source, "CORE") != 0 &&
        strcmp(rep->source, "LLM") != 0)
        return;
    keep_draft = rep->stage_draft && rep->stage[0] &&
                 strcmp(rep->source, "LLM") != 0 &&
                 !cd_stage_identity_leak(rep->stage);
    if (!keep_draft) {
        rep->stage[0] = 0;
        rep->stage_draft = 0;
    }
    if (cd_nanny_shape(q))
        snprintf(rep->answer, sizeof rep->answer,
                 "Not sealed. Logged for improve.");
    else
        snprintf(rep->answer, sizeof rep->answer, "Not sealed.");
    if (keep_draft)
        snprintf(rep->utterance, sizeof rep->utterance, "%.767s", rep->stage);
    else
        snprintf(rep->utterance, sizeof rep->utterance,
                 "I don't have a verified answer for that request. "
                 "Specify the skill and inputs, or provide a source to check. "
                 "Ask 'what can you do' for the current inventory.");
    (void)S;
}


/* Pull chunk N's text out of a packed context block ("[c1] note ...\n"). */
static int cd_ctx_chunk(const char *ctx, int n, char *out, size_t cap) {
    char tag[16];
    const char *p, *e;
    size_t len;
    out[0] = 0;
    if (!ctx || n < 1) return -1;
    snprintf(tag, sizeof tag, "[c%d] ", n);
    p = strstr(ctx, tag);
    if (!p) return -1;
    p += strlen(tag);
    e = strchr(p, '\n');
    len = e ? (size_t)(e - p) : strlen(p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return 0;
}

/* Does the sentence carrying [cN] share any significant word with chunk N? */
static int cd_cite_supported(const char *sentence, const char *chunk) {
    char tok[64];
    size_t i = 0, t = 0;
    if (!chunk || !chunk[0]) return 0;
    while (sentence[i]) {
        if (isalnum((unsigned char)sentence[i])) {
            if (t + 1 < sizeof tok) tok[t++] = sentence[i];
        } else {
            tok[t] = 0;
            if (t >= 4 && contains_ci(chunk, tok)) return 1;
            t = 0;
        }
        i++;
    }
    tok[t] = 0;
    return t >= 4 && contains_ci(chunk, tok);
}

/* Citation post-check.
 *
 * Strip any [cN] the context cannot back: a bad number, or a valid number whose
 * chunk shares nothing with the sentence citing it.
 *
 * A dangling or unearned [cN] is ungrounded text wearing the costume of
 * grounding -- the same failure class as claiming CERT, just in punctuation.
 * Observed: "thanks" came back as "You're welcome. [c1]", and it kept doing so
 * after the system prompt explicitly forbade citing a thank-you. Prompting did
 * not hold, so the check lives in C, which is the component we can trust to
 * enforce it. (That the prompt failed is itself evidence for whether
 * prompt-level discipline is sufficient, or whether SFT is needed.)
 */
static void cd_check_citations(char *s, const char *ctx, int n_chunks) {
    char *r = s;
    while ((r = strstr(r, "[c")) != NULL) {
        char *d = r + 2;
        int v = 0, ok = 0;
        while (isdigit((unsigned char)*d)) { v = v * 10 + (*d - '0'); d++; }
        if (*d != ']' || d == r + 2) { r += 2; continue; }
        if (v >= 1 && v <= n_chunks) {
            char chunk[900], sent[512];
            char *b = r;
            size_t n;
            while (b > s && b[-1] != '.' && b[-1] != '!' && b[-1] != '?') b--;
            n = (size_t)(r - b);
            if (n >= sizeof sent) n = sizeof sent - 1;
            memcpy(sent, b, n);
            sent[n] = 0;
            if (cd_ctx_chunk(ctx, v, chunk, sizeof chunk) == 0 &&
                cd_cite_supported(sent, chunk))
                ok = 1;
        }
        if (ok) { r = d + 1; continue; }
        {
            char *w = r, *q = d + 1;
            while (*q == ' ') q++;
            if (w > s && w[-1] == ' ' && (*q == 0 || *q == '.' || *q == ',')) w--;
            memmove(w, q, strlen(q) + 1);
            r = w;
        }
    }
    { size_t l = strlen(s); while (l && (s[l-1] == ' ' || s[l-1] == '\t')) s[--l] = 0; }
}

static void handle_client(int cfd, CdState *S) {
    char line[CNETD_REQUEST_MAX];
    char parse_error[128];
    CnetdRequest request;
    CnetdReadResult read_result;

    read_result = cnetd_read_request(cfd, line, sizeof line,
                                     client_read_timeout_ms());
    if (read_result != CNETD_READ_OK) {
        const char *reply = NULL;
        if (read_result == CNETD_READ_TIMEOUT)
            reply = "ERR request_timeout\n";
        else if (read_result == CNETD_READ_TOO_LONG)
            reply = "ERR request_too_long\n";
        else if (read_result == CNETD_READ_INCOMPLETE)
            reply = "ERR incomplete_request\n";
        else if (read_result == CNETD_READ_ERROR && !g_stop)
            reply = "ERR request_read_failed\n";
        if (reply) (void)full_write(cfd, reply, strlen(reply));
        return;
    }
    if (cnetd_parse_request(line, &request,
                            parse_error, sizeof parse_error) != 0) {
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '{') {
            const char *reply =
                "{\"ok\":false,\"error\":\"invalid_request\"}\n";
            (void)full_write(cfd, reply, strlen(reply));
        } else {
            (void)full_write(cfd, "ERR invalid_request\n", 20);
        }
        if (getenv("CNETD_PROTOCOL_LOG"))
            fprintf(stderr, "cnetd: invalid request: %s\n", parse_error);
        return;
    }

    if (request.kind == CNETD_REQUEST_QUIT) return;
    if (request.kind == CNETD_REQUEST_PING) {
        (void)full_write(cfd, "PONG\n", 5);
        return;
    }
    if (request.kind == CNETD_REQUEST_STATUS) {
        char buf[256];
        snprintf(buf, sizeof buf, "OK cnetd routes=%d probes=%d root=%.180s\n",
                 S->n_routes, S->probes.n, S->root);
        (void)full_write(cfd, buf, strlen(buf));
        return;
    }

    {
        CdReply rep;
        const char *q = request.query;
        int json = request.json;
        char frame_q[CNET_DC_Q];

        /* Peer identity is request-local. It is informational only: persona
         * and reply metadata may mention it; routing and CERT never read it. */
        cnet_dialog_ctx_set_peer(&S->dialog,
                                 request.peer[0] ? request.peer : NULL);
        if (cnet_dialog_repeat_query(q) && S->dialog.last_query[0]) {
            snprintf(frame_q, sizeof frame_q, "%s", S->dialog.last_query);
            q = frame_q;
        }
        if (request.peer[0] && getenv("CNET_PEER_LOG"))
            fprintf(stderr, "cnetd: peer=%s q=%.120s\n", request.peer, q);
        /* Allowlisted chat actions run before CERT and answer as SOURCE ACTION.
         * Anything they do not claim falls through to the sealed path. */
        memset(&rep, 0, sizeof rep);
        /* Explicit read requests are a terminal permission boundary BEFORE
         * legacy actions, retrieval and residuals. Even a refusal cannot route
         * into the older unrestricted lookup path. Web evidence is display
         * data, never a local answer or an acquisition/training label. */
        int mcp_read_handled = cnet_mcp_read_brick_ask(q, NULL, 0);
        if (mcp_read_handled) {
            char *evidence = calloc(262144, 1);
            if (!evidence || !cnet_mcp_read_brick_ask(q, evidence, 262144) ||
                !cnet_mcp_read_summary(evidence, rep.answer, sizeof rep.answer)) {
                snprintf(rep.answer, sizeof rep.answer,
                    "MCP read refused (not CERT). Check the enabled policy, dispatch capsule and MCP endpoint.");
                snprintf(rep.utterance, sizeof rep.utterance, "%s",
                    "MCP read refused (not CERT). Check the read policy, capsule and endpoint.");
            } else if (!cnet_mcp_read_summary(evidence, rep.utterance, sizeof rep.utterance)) {
                snprintf(rep.utterance, sizeof rep.utterance,
                    "Untrusted web evidence (not CERT); see the answer for its source.");
            }
            free(evidence);
            snprintf(rep.source, sizeof rep.source, "MCP_READ");
            snprintf(rep.skill, sizeof rep.skill, "mcp_read_dispatch_v1");
            rep.miss = 1;
            rep.verified = 0;
            goto kb_answered;
        }
        if (!cd_action(S, q, &rep)) {
            if (cd_ask(S, q, &rep) != 0) {
                (void)full_write(cfd, "{\"ok\":false,\"error\":\"ask_failed\"}\n", 34);
                return;
            }
            if (rep.capsule_handled) goto kb_answered;
            /* Residual STAGE: generative conversation on CERT miss.
             * NEVER becomes LOCAL/CERT. Speaks as STAGE with claimed_cert=0.
             * Hardcoded utter_self templates are not conversation — stage is. */
            /* Prefer information we were GIVEN over information the residual
             * would invent. A note someone ingested beats a fluent guess -- it
             * is why "gold hash" must not come back as Bitcoin. Notes are not
             * CERT: SOURCE INFO, miss stays 1, claimed_cert 0. Only when no
             * note matches does the stage get to draft. */
            if (rep.miss && !rep.shortcircuit && !rep.typed_en_hit &&
                !rep.arith_hit && !rep.brick_hit && !rep.solver_hit) {
                char kbn[1400];
                if (kb_recall_overlap(q, kbn, sizeof kbn) > 0) {
                    snprintf(rep.answer, sizeof rep.answer,
                             "From what I was told (not certified): %.1800s", kbn);
                    snprintf(rep.utterance, sizeof rep.utterance, "%.760s", rep.answer);
                    snprintf(rep.source, sizeof rep.source, "INFO");
                    snprintf(rep.skill, sizeof rep.skill, "kb_recall");
                    rep.verified = 0;
                    rep.miss = 1;
                    rep.stage_draft = 0;
                    (void)kb_note_hit(S, q, kbn);
                    goto kb_answered;
                }
            }
            /* Factory MCP on closed intents only. Shared sock — no second
             * SoulHost. Never CERT: SOURCE MCP, miss stays 1. */
            if (rep.miss && !rep.shortcircuit && !rep.typed_en_hit &&
                !rep.arith_hit && !rep.brick_hit && !rep.solver_hit &&
                !cd_skip_teacher(S, q)) {
                char mcp[1400];
                if (cnet_mcp_factory_ask(q, mcp, sizeof mcp) > 0) {
                    snprintf(rep.answer, sizeof rep.answer, "%.1390s", mcp);
                    snprintf(rep.utterance, sizeof rep.utterance, "%.760s", rep.answer);
                    snprintf(rep.source, sizeof rep.source, "MCP");
                    snprintf(rep.skill, sizeof rep.skill, "mcp_factory");
                    rep.verified = 0;
                    rep.miss = 1;
                    rep.stage_draft = 0;
                    goto kb_answered;
                }
            }
            /* PLAY door. Runs under cd_play_may_speak(), the inverse of the
             * law-door gate: front-owned / nanny / explicit-lookup / player-SKU
             * misses never reach the stage. When the stage drafts, the draft
             * rides UTTERANCE and STAGE only. ANSWER stays "Not sealed." --
             * never the draft. SOURCE STAGE, DOMAIN PLAY, STAGE_DRAFT 1, so
             * CLAIMED_CERT is 0 by the write_text_reply formula. Never LOCAL,
             * never CORE (CORE means a CORE plane bound the turn; an HTTP
             * residual draft is not that). */
            if (rep.miss && !rep.stage_draft && !rep.shortcircuit &&
                !rep.typed_en_hit && !rep.arith_hit && !rep.brick_hit &&
                !rep.solver_hit &&
                cd_play_may_speak(S, q)) {
                char ctx[1400];
                int nc = cnet_ml_context_pack(q, ctx, sizeof ctx);
                if (cnet_ml_stage_draft_ctx(q, ctx[0] ? ctx : NULL, rep.stage,
                                           sizeof rep.stage)) {
                    /* A citation the context pack cannot back is stripped. */
                    cd_check_citations(rep.stage, ctx, nc);
                    if (cd_stage_identity_leak(rep.stage)) {
                        /* Identity improvised -> draft dropped, not shown. */
                        rep.stage[0] = 0;
                        rep.stage_draft = 0;
                        snprintf(rep.answer, sizeof rep.answer, "Not sealed.");
                        snprintf(rep.utterance, sizeof rep.utterance,
                                 "Not sealed.");
                    } else {
                        rep.stage_draft = 1;
                        snprintf(rep.answer, sizeof rep.answer, "Not sealed.");
                        snprintf(rep.utterance, sizeof rep.utterance, "%.760s",
                                 rep.stage);
                        snprintf(rep.source, sizeof rep.source, "STAGE");
                        snprintf(rep.skill, sizeof rep.skill, "stage_residual");
                        snprintf(rep.domain, sizeof rep.domain, "PLAY");
                        rep.verified = 0;
                        rep.miss = 1;
                        {
                            const char *mv = getenv("CNET_STAGE_MAY_VOICE");
                            rep.may_voice = (mv && mv[0] == '1') ? 1 : 0;
                        }
                        if (S->miss_log[0]) {
                            FILE *mf = fopen(S->miss_log, "a");
                            if (mf) {
                                fprintf(mf, "{\"q\":\"");
                                {
                                    const char *p = q;
                                    for (; p && *p; p++) {
                                        if (*p == '"' || *p == '\\') fputc('\\', mf);
                                        if (*p != '\n' && *p != '\r') fputc(*p, mf);
                                    }
                                }
                                fprintf(mf, "\",\"answer\":\"");
                                {
                                    const char *p = rep.stage;
                                    for (; p && *p; p++) {
                                        if (*p == '"' || *p == '\\') fputc('\\', mf);
                                        if (*p != '\n' && *p != '\r') fputc(*p, mf);
                                    }
                                }
                                /* WRITE-TIME PROVENANCE.
                                 * This is our own mouth on the PLAY door. Tag
                                 * it at the point of writing so the evolve
                                 * tick excludes it structurally: plane PLAY,
                                 * self_authored, auto_cert false. multi_stable
                                 * must never count a play row; no FAQ gold,
                                 * ever, from a play turn. */
                                fprintf(mf,
                                        "\",\"via\":\"stage_residual\","
                                        "\"plane\":\"PLAY\",\"claimed_cert\":0,"
                                        "\"auto_cert\":false,\"learnable\":true,"
                                        "\"self_authored\":true,"
                                        "\"source\":\"%s\"}\n",
                                        rep.source);
                                fclose(mf);
                            }
                        }
                    }
                }
            }
        }
    kb_answered:
        /* Surface who asked. Set after cd_ask so the answer cannot depend on it. */
        snprintf(rep.peer, sizeof rep.peer, "%s", S->dialog.peer_name);
        if (!mcp_read_handled) {
            cd_gap_learn(S, q, &rep);
            cd_starve_leftover(S, q, &rep);
        }
        cnet_dialog_ctx_update(&S->dialog, q, rep.skill, "-", !rep.miss);
        /* Showrunner sees every turn: mood, cooldown, last-N ring. */
        cnet_sr_on_turn(&S->show, rep.peer[0] ? rep.peer : "-", q, rep.answer,
                        rep.skill, rep.source, rep.miss, rep.may_voice);
        if (json)
            write_json_reply(cfd, &rep);
        else
            write_text_reply(cfd, &rep);
    }
}

static int serve(CdState *S, const char *sock_path) {
    int sfd, cfd;
    struct sockaddr_un addr;
    char dir[CD_PATH];
    char *slash;

    if (strlen(sock_path) >= sizeof addr.sun_path) {
        fprintf(stderr, "cnetd: socket path too long\n");
        return 1;
    }
    snprintf(dir, sizeof dir, "%s", sock_path);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdir_p(dir);
    }
    unlink(sock_path);

    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, sock_path, strlen(sock_path) + 1);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind");
        close(sfd);
        return 1;
    }
    chmod(sock_path, 0600);
    if (listen(sfd, 16) != 0) {
        perror("listen");
        close(sfd);
        return 1;
    }
    CnetCapsuleControl *control = NULL;
    if (S->capsule_store) {
        control = cnet_capsule_control_open(S->capsule_store, getenv("CNET_CAPSULE_CONTROL_SOCK"));
        if (!control) { fprintf(stderr, "cnetd: capsule operator socket refused\n"); close(sfd); unlink(sock_path); return 1; }
    }
    fprintf(stderr, "cnetd listening on %s root=%s routes=%d\n", sock_path, S->root,
            S->n_routes);

    while (!g_stop) {
        struct pollfd ready[2] = {{sfd, POLLIN, 0}, {cnet_capsule_control_fd(control), POLLIN, 0}};
        int n = poll(ready, 2, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (ready[1].revents & POLLIN) cnet_capsule_control_accept(control);
        if (!(ready[0].revents & POLLIN)) continue;
        cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_client(cfd, S);
        close(cfd);
    }
    cnet_capsule_control_close(control);
    close(sfd);
    unlink(sock_path);
    return 0;
}

int main(int argc, char **argv) {
    CdState S;
    char root[CD_PATH], sock[CD_SOCK];
    int i, foreground = 1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--daemon")) foreground = 0;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr,
                    "usage: cnetd [--daemon]\n"
                    "  CNET_PACKS_ROOT / CNET_MINIMAL_ROOT / CNET_SOCK\n");
            return 0;
        }
    }

    install_stop_handler(SIGINT);
    install_stop_handler(SIGTERM);
    signal(SIGPIPE, SIG_IGN);

    resolve_paths(root, sizeof root, sock, sizeof sock);
    if (cd_init(&S, root) != 0) {
        fprintf(stderr, "cnetd: failed to init packs root=%s\n", root);
        return 1;
    }
    if (cnet_serve_global_load_env() == 0)
        fprintf(stderr, "{\"event\":\"brick_bank_loaded\",\"count\":%d,"
                        "\"capacity\":%d,\"certified\":0}\n",
                cnet_serve_global()->n, CNET_SERVE_MAX_BRICKS);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (!foreground) {
        pid_t p = fork();
        if (p < 0) return 1;
        if (p > 0) {
            printf("cnetd pid=%d sock=%s\n", (int)p, sock);
            return 0;
        }
        setsid();
    }

    /* Configured capsule authority is resident and terminal even when empty.
     * A malformed durable owner configuration refuses startup, never degrades. */
    const char *sets = getenv("CNET_CAPSULE_SETS_DIR"), *state = getenv("CNET_CAPSULE_STATE_DIR");
    const char *control = getenv("CNET_CAPSULE_CONTROL_SOCK"), *legacy = getenv("CNET_CAPSULES_DIR");
    if (sets || state || control) {
        if (!sets || !state || !control || legacy || !(S.capsule_store = cnet_capsule_store_open(sets, state))) {
            fprintf(stderr, "cnetd: capsule durable configuration/recovery refused\n"); return 1;
        }
        S.capsule_configured = 1;
    } else if (legacy) {
        S.capsule_host = cnet_core_host_open(legacy);
        if (!S.capsule_host) { fprintf(stderr, "cnetd: resident capsule load refused\n"); return 1; }
        S.capsule_configured = 1;
    }
    int rc = serve(&S, sock);
    if (S.capsule_store && cnet_capsule_store_close(S.capsule_store)) rc = 1;
    if (S.capsule_host && cnet_core_host_close(S.capsule_host)) rc = 1;
    return rc;
}
