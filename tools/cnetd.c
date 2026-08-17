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
 *   → multi-line status + ANSWER <text>\nEND\n
 *   PING\n → PONG\n
 *   QUIT\n → close
 *
 *   JSON: {"op":"ask","q":"..."}\n
 *   → {"ok":true,"source":"LOCAL","answer":"...","skill":"...","miss":false,...}\n
 *
 * Env:
 *   CNET_MINIMAL_ROOT, CNET_PACKS_ROOT, CNET_SOCK
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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../include/cnet_dialog_ctx.h"
#include "../include/cnet_json_escape.h"
#include "../include/cnet_domain_route.h"
#include "../include/cnet_probe_shortcircuit.h"
#include "../include/cnet_query_alias.h"
#include "../include/cnet_roe_asi.h"
#include "../include/cnet_slot_extract.h"
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
#include <sys/wait.h>

#define CD_PATH 512
#define CD_SOCK 108
#define CD_MAX_ALWAYS 12
#define CD_MAX_PACKS 8
#define CD_PAT 256
#define CD_ID 64
#define CD_LINE 8192

static volatile sig_atomic_t g_stop = 0;

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
    int alias_hit;
    int dialog_hit;
    int slot_hit;
    int miss;
    int shortcircuit;
    int verified;
    int may_voice;
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

static int cd_ask(CdState *S, const char *q, CdReply *out) {
    RoeAsi *R;
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

    if (!S || !q || !out || !S->ready) return -1;
    memset(out, 0, sizeof *out);
    memset(&rt, 0, sizeof rt);

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
        if (sb && cnet_serve_result(sb, q, &sr) == 0 && sr.proved) {
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
                     strncmp(hr.skill, "add_", 4) == 0 ||
                     strncmp(hr.skill, "sub_", 4) == 0 ||
                     strncmp(hr.skill, "mul_", 4) == 0))
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

        cnet_rlm_policy_default(&rpol);
        memset(&rr, 0, sizeof rr);
        if (cnet_rlm_ask(q, &rpol, &rr) == 0 && rr.final.bound) {
            const CnetHemiResult *hr = &rr.final;
            const char *spoken;
            int arith = 0;
            /* OPEN_CHAT answers are product-illegal. Cut the path. */
            if (hr->plane == CNET_CORE_PLANE_OPEN_CHAT || hr->open_chat) {
                snprintf(out->answer, sizeof out->answer, "open_chat_answer_killed");
                snprintf(out->utterance, sizeof out->utterance, "open_chat_answer_killed");
                snprintf(out->source, sizeof out->source, "CNET");
                snprintf(out->skill, sizeof out->skill, "open_chat_killed");
                cd_scopy(out->prepared, sizeof out->prepared, q);
                out->verified = 0;
                out->miss = 1;
                out->may_voice = 0;
                out->tokens = 0;
                return 0;
            }
            /* Wiki/residual claiming CERT must not beat pack ROE. Arithmetic OK. */
            if (hr->skill[0] &&
                (strcmp(hr->skill, "add_u32_v1") == 0 ||
                 strcmp(hr->skill, "sub_u32_v1") == 0 ||
                 strcmp(hr->skill, "mul_u32_v1") == 0 ||
                 strncmp(hr->skill, "add_", 4) == 0 ||
                 strncmp(hr->skill, "sub_", 4) == 0 ||
                 strncmp(hr->skill, "mul_", 4) == 0))
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
    roe_turn(R, q_use, &rep);

    snprintf(out->source, sizeof out->source, "%s",
             rep.source_name[0] ? rep.source_name : "UNKNOWN");
    snprintf(out->skill, sizeof out->skill, "%s", rep.skill_id);
    snprintf(out->answer, sizeof out->answer, "%s", rep.answer);
    out->verified = rep.verified ? 1 : 0;
    out->tokens = (unsigned long long)rep.tokens_est;
    out->miss = (rep.source != ROE_SRC_LOCAL) ? 1 : 0;

    /* Warm dialog ctx AFTER turn — entities from prepared CERT-shaped query. */
    if (!out->shortcircuit) {
        const char *pack_hint = rt.pack[0] ? rt.pack : dd.pack_or_skill;
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
        snprintf(U.source, sizeof U.source, "%s", out->source);
        snprintf(U.skill, sizeof U.skill, "%s", out->skill);
        snprintf(U.domain, sizeof U.domain, "%s", out->domain);
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

        /*
         * Teach path: LLM answer stays as user-facing answer (untrusted draft).
         * Self-answer only when no CERT and no Teacher draft (teacher off/fail/probe).
         */
        if (out->miss && strcmp(out->source, "LLM") != 0 && self_answer) {
            snprintf(out->answer, sizeof out->answer, "%.2047s", out->utterance);
            snprintf(out->source, sizeof out->source, "CNET");
            snprintf(out->skill, sizeof out->skill, "%s",
                     out->shortcircuit ? "utter_probe" : "utter_self");
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
        gmtime_r(&t, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
        cnet_json_escape(q, qesc, sizeof qesc);
        cnet_json_escape(out->answer, aesc, sizeof aesc);
        if (var_miss && var_miss[0] && strcmp(var_miss, S->miss_log) != 0)
            f2 = fopen(var_miss, "a");
        {
            char line[ROE_ANSWER_MAX * 2 + 768];
            int is_llm = (strcmp(out->source, "LLM") == 0);
            int is_cnet = (strcmp(out->source, "CNET") == 0);
            if (out->shortcircuit)
                snprintf(line, sizeof line,
                         "{\"ts\":\"%s\",\"query\":\"%s\",\"source\":\"%s\","
                         "\"answer\":\"%s\",\"shortcircuit\":true,\"probe_pat\":\"%s\","
                         "\"teacher\":false,\"learnable\":false,\"self_answer\":true,"
                         "\"via\":\"cnetd\",\"tokens_est\":%llu}\n",
                         ts, qesc, out->source, aesc, out->probe_pat, out->tokens);
            else
                snprintf(line, sizeof line,
                         "{\"ts\":\"%s\",\"query\":\"%s\",\"source\":\"%s\","
                         "\"answer\":\"%s\",\"shortcircuit\":false,"
                         "\"teacher\":%s,\"learnable\":%s,\"open_chat\":%s,"
                         "\"self_answer\":%s,\"auto_cert\":false,\"via\":\"cnetd\","
                         "\"tokens_est\":%llu}\n",
                         ts, qesc, out->source, aesc, is_llm ? "true" : "false",
                         (is_llm || is_cnet) ? "true" : "false",
                         is_llm ? "true" : "false", is_cnet ? "true" : "false",
                         out->tokens);
            if (f) {
                fputs(line, f);
                fclose(f);
            }
            if (f2) {
                fputs(line, f2);
                fclose(f2);
            }
        }
    }
    free(R);
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
    char buf[sizeof aesc + sizeof uesc + sizeof src_e + sizeof skill_e +
             sizeof dom_e + sizeof probe_e + sizeof prep_e + sizeof alias_e +
             sizeof dreason_e + sizeof dent_e + sizeof sunit_e +
             sizeof sreason_e + 768];
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
             "\"self_answer\":%s}\n",
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
                                                                                : "false");
    (void)full_write(fd, buf, strlen(buf));
}

static void write_text_reply(int fd, const CdReply *r) {
    char head[768];
    char body[ROE_ANSWER_MAX + 64];
    char utter[CNET_UTTER_TEXT + 64];
    int n;
    n = snprintf(head, sizeof head,
                 "SOURCE %s\nSKILL %s\nDOMAIN %s\nMISS %d\nSHORTCIRCUIT %d\n"
                 "ALIAS_HIT %d\nDIALOG_HIT %d\nSLOT_HIT %d\nPREPARED %.400s\n"
                 "MAY_VOICE %d\n",
                 r->source, r->skill[0] ? r->skill : "-", r->domain, r->miss,
                 r->shortcircuit, r->alias_hit, r->dialog_hit, r->slot_hit,
                 r->prepared[0] ? r->prepared : "-", r->may_voice);
    if (n > 0) (void)full_write(fd, head, (size_t)n);
    n = snprintf(utter, sizeof utter, "UTTERANCE %s\n",
                 r->utterance[0] ? r->utterance : "-");
    if (n > 0) (void)full_write(fd, utter, (size_t)n);
    n = snprintf(body, sizeof body, "ANSWER %s\nEND\n", r->answer);
    if (n > 0) (void)full_write(fd, body, (size_t)n);
}

static void handle_client(int cfd, CdState *S) {
    char line[CD_LINE];
    size_t n = 0;
    ssize_t k;
    while (!g_stop) {
        k = read(cfd, line + n, sizeof line - n - 1);
        if (k <= 0) break;
        n += (size_t)k;
        line[n] = 0;
        if (memchr(line, '\n', n)) break;
        if (n >= sizeof line - 1) break;
    }
    /* trim */
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;

    if (!line[0] || strcmp(line, "QUIT") == 0) return;
    if (strcmp(line, "PING") == 0) {
        (void)full_write(cfd, "PONG\n", 5);
        return;
    }
    if (strcmp(line, "STATUS") == 0) {
        char buf[256];
        snprintf(buf, sizeof buf, "OK cnetd routes=%d probes=%d root=%.180s\n",
                 S->n_routes, S->probes.n, S->root);
        (void)full_write(cfd, buf, strlen(buf));
        return;
    }

    {
        CdReply rep;
        const char *q = line;
        int json = 0;
        if (strncmp(line, "ASK ", 4) == 0) q = line + 4;
        else if (line[0] == '{') {
            /* {"op":"ask","q":"..."} or pretty {"q": "..."} */
            char *p = strstr(line, "\"q\"");
            json = 1;
            if (p) {
                static char qq[CD_LINE];
                size_t i = 0;
                p = strchr(p + 3, ':');
                if (p) {
                    p++;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p == '"') {
                        p++;
                        while (*p && *p != '"' && i + 1 < sizeof qq) {
                            if (*p == '\\' && p[1]) {
                                p++;
                                qq[i++] = *p++;
                            } else {
                                qq[i++] = *p++;
                            }
                        }
                        qq[i] = 0;
                        q = qq;
                    }
                }
            }
        }
        if (cd_ask(S, q, &rep) != 0) {
            (void)full_write(cfd, "{\"ok\":false,\"error\":\"ask_failed\"}\n", 34);
            return;
        }
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
    fprintf(stderr, "cnetd listening on %s root=%s routes=%d\n", sock_path, S->root,
            S->n_routes);

    while (!g_stop) {
        cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_client(cfd, S);
        close(cfd);
    }
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
        fprintf(stderr, "cnetd: core serve bricks loaded\n");
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

    return serve(&S, sock);
}
