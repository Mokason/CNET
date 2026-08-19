/* AGI-type CORE scenario loop — given info, value, prove/abstain, evolve, compose. */
#include "cnet_agi_scenario.h"

#include "cnet_dc_invent.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || !cap) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int starts_ci(const char *s, const char *pfx) {
    size_t i;
    if (!s || !pfx) return 0;
    for (i = 0; pfx[i]; ++i) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)pfx[i]))
            return 0;
    }
    return 1;
}

void cnet_agi_scenario_init(CnetAgiScenario *S, const char *bricks_dir,
                            const char *miss_path, const char *bonsai) {
    if (!S) return;
    memset(S, 0, sizeof *S);
    cnet_core_bus_init(&S->bus);
    cnet_serve_bank_init(&S->serve);
    copy_text(S->bricks_dir, sizeof S->bricks_dir,
              bricks_dir && bricks_dir[0] ? bricks_dir : "agi_bricks");
    copy_text(S->miss_path, sizeof S->miss_path,
              miss_path && miss_path[0] ? miss_path : "agi_miss.jsonl");
    copy_text(S->bonsai, sizeof S->bonsai,
              bonsai && bonsai[0]
                  ? bonsai
                  : "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf");
    cnet_mkdir(S->bricks_dir, 0755);
    cnet_setenv("CNET_CORE_BUS_BRICKS_DIR", S->bricks_dir, 1);
}

void cnet_agi_scenario_free(CnetAgiScenario *S) {
    if (!S) return;
    cnet_core_bus_free(&S->bus);
    memset(S, 0, sizeof *S);
}

static void sync_serve_from_bus(CnetAgiScenario *S) {
    int i;
    for (i = 0; i < S->bus.n_bricks; ++i) {
        if (!S->bus.bricks[i].live || !S->bus.bricks[i].certified) continue;
        (void)cnet_serve_save_lut(S->bricks_dir, S->bus.bricks[i].domain_tag,
                                  S->bus.bricks[i].name,
                                  S->bus.bricks[i].lut_table);
    }
    (void)cnet_serve_bank_load_dir(&S->serve, S->bricks_dir);
}

int cnet_agi_scenario_boot(CnetAgiScenario *S, int factory_seed) {
    CnetPath2Bench fb;
    CnetPath2Spec specs[2];
    if (!S) return -1;
    (void)cnet_serve_bank_load_dir(&S->serve, S->bricks_dir);
    if (S->serve.n > 0) return 0;
    if (!factory_seed) return 1;
    if (access(S->bonsai, R_OK) != 0) return -1;
    specs[0] =
        (CnetPath2Spec){"blk.0.attn_q.weight", "q1_add16", "agi_q_add", 0};
    specs[1] =
        (CnetPath2Spec){"blk.0.attn_k.weight", "q1_xor16", "agi_k_xor", 1};
    if (cnet_path2_factory_run(&S->bus, S->bonsai, S->bricks_dir, specs, 2,
                               &fb) != 0)
        return -2;
    sync_serve_from_bus(S);
    return 0;
}

static CnetAgiFact *find_fact(CnetAgiScenario *S, const char *key) {
    int i;
    for (i = 0; i < S->n_facts; ++i)
        if (strcmp(S->facts[i].key, key) == 0) return &S->facts[i];
    return NULL;
}

static CnetAgiFact *ensure_fact(CnetAgiScenario *S, const char *key) {
    CnetAgiFact *f = find_fact(S, key);
    if (f) return f;
    if (S->n_facts >= CNET_AGI_MAX_FACTS) return NULL;
    f = &S->facts[S->n_facts++];
    memset(f, 0, sizeof *f);
    copy_text(f->key, sizeof f->key, key);
    return f;
}

int cnet_agi_scenario_ingest(CnetAgiScenario *S, const char *line) {
    const char *p;
    char tag[32];
    CnetAgiFact *f;
    int i;
    if (!S || !line) return -1;
    p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!starts_ci(p, "given")) return 1; /* not given-info */
    p += 5;
    while (*p == ' ' || *p == '\t') p++;
    if (!starts_ci(p, "domain")) return -1;
    p += 6;
    while (*p == ' ' || *p == '\t') p++;
    i = 0;
    while (*p && *p != ' ' && *p != '\t' && i + 1 < (int)sizeof tag)
        tag[i++] = *p++;
    tag[i] = 0;
    if (!tag[0]) return -1;
    f = ensure_fact(S, tag);
    if (!f) return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (starts_ci(p, "lut=")) {
        p += 4;
        for (i = 0; i < 16; ++i) {
            while (*p == ',' || *p == ' ') p++;
            f->lut[i] = (float)strtod(p, (char **)&p);
        }
        f->has_lut = 1;
        S->given_ingests++;
        return 0;
    }
    if (starts_ci(p, "pairs=")) {
        unsigned seen[16];
        int got = 0;
        memset(seen, 0, sizeof seen);
        p += 6;
        while (*p) {
            unsigned a, b;
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            a = (unsigned)strtoul(p, (char **)&p, 10);
            if (*p == ':') p++;
            b = (unsigned)strtoul(p, (char **)&p, 10);
            if (a < 16 && b < 16) {
                f->lut[a] = (float)b;
                if (!seen[a]) {
                    seen[a] = 1;
                    got++;
                }
            }
        }
        f->has_lut = (got == 16);
        S->given_ingests++;
        return f->has_lut ? 0 : 1;
    }
    return -1;
}

double cnet_agi_scenario_value_miss(const CnetAgiScenario *S, const char *turn) {
    double v = 0.35; /* base: every OOD miss has some value */
    const char *p;
    int has_tag = 0, has_num = 0;
    if (!turn) return 0;
    p = turn;
    while (*p == ' ') p++;
    if (isalpha((unsigned char)*p) || *p == '_') {
        has_tag = 1;
        v += 0.25;
        if (S) {
            char tag[32];
            int i = 0;
            while (*p && *p != ' ' && i + 1 < 32) tag[i++] = *p++;
            tag[i] = 0;
            if (find_fact((CnetAgiScenario *)S, tag) &&
                find_fact((CnetAgiScenario *)S, tag)->has_lut)
                v += 0.30; /* given full table → high value to admit */
        }
    }
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (isdigit((unsigned char)*p)) {
        has_num = 1;
        v += 0.15;
    }
    if (has_tag && has_num) v += 0.10;
    if (v > 1.0) v = 1.0;
    return v;
}

static int try_compose_if_ready(CnetAgiScenario *S, const char *tag_a,
                                const char *tag_b, const char *tag_out) {
    CnetPath4Bench pb;
    memset(&pb, 0, sizeof pb);
    if (cnet_path4_compose_bricks(&S->bus, tag_a, tag_b, tag_out, &pb) != 0)
        return -1;
    sync_serve_from_bus(S);
    S->composes++;
    return 0;
}

static int admit_fact_as_brick(CnetAgiScenario *S, CnetAgiFact *f) {
    char gguf[768], miss[768];
    CnetPath3Bench pb;
    if (!f || !f->has_lut) return -1;
    snprintf(gguf, sizeof gguf, "%s/given_%s.gguf", S->bricks_dir, f->key);
    snprintf(miss, sizeof miss, "%s/given_%s_miss.jsonl", S->bricks_dir, f->key);
    unlink(miss);
    unlink(gguf);
    if (cnet_path3_write_nibble_misslog(miss, f->lut, f->key) != 0) return -1;
    memset(&pb, 0, sizeof pb);
    if (S->bus.state != CNET_CORE_BUS_IDLE) {
        /* force idle for admit */
        if (S->bus.wo.bound) (void)cnet_core_bus_unbind(&S->bus);
        S->bus.state = CNET_CORE_BUS_IDLE;
    }
    if (cnet_path3_miss_to_admit(&S->bus, miss, gguf, f->key, f->key, &pb) != 0)
        return -1;
    sync_serve_from_bus(S);
    S->evolves++;
    return pb.served_ok ? 0 : -1;
}

int cnet_agi_scenario_evolve_tick(CnetAgiScenario *S) {
    CnetPath3Bench pb;
    char gguf[768];
    int i;
    if (!S) return -1;
    /* 1) High-value given facts with full LUT → admit */
    for (i = 0; i < S->n_facts; ++i) {
        if (S->facts[i].has_lut) {
            /* skip if already served */
            CnetServeResult sr;
            char probe[64];
            snprintf(probe, sizeof probe, "%s 0", S->facts[i].key);
            if (cnet_serve_result(&S->serve, probe, &sr) == 0 && sr.proved)
                continue;
            if (admit_fact_as_brick(S, &S->facts[i]) == 0) return 0;
        }
    }
    /* 2) Live miss path if complete table */
    if (access(S->miss_path, R_OK) == 0) {
        snprintf(gguf, sizeof gguf, "%s/tick_miss.gguf", S->bricks_dir);
        memset(&pb, 0, sizeof pb);
        if (S->bus.state != CNET_CORE_BUS_IDLE) {
            if (S->bus.wo.bound) (void)cnet_core_bus_unbind(&S->bus);
            S->bus.state = CNET_CORE_BUS_IDLE;
        }
        if (cnet_path3_miss_to_admit(&S->bus, S->miss_path, gguf, "tick_miss",
                                     "tick_nibble", &pb) == 0 &&
            pb.served_ok) {
            sync_serve_from_bus(S);
            S->evolves++;
            return 0;
        }
    }
    /* 3) Compose if two base bricks exist and compose missing */
    {
        CnetServeResult sr;
        if (cnet_serve_result(&S->serve, "q1_add16 0", &sr) == 0 &&
            cnet_serve_result(&S->serve, "q1_xor16 0", &sr) == 0) {
            if (cnet_serve_result(&S->serve, "q1_comp 0", &sr) != 0) {
                if (try_compose_if_ready(S, "q1_add16", "q1_xor16", "q1_comp") ==
                    0)
                    return 0;
            }
        }
    }
    return 1; /* nothing to do */
}

static void log_turn(CnetAgiScenario *S, const char *turn, int proved,
                     int abstained, int evolved, int composed, int parrot,
                     double value, const char *detail) {
    CnetAgiTurnLog *L;
    if (!S || S->n_turns >= CNET_AGI_MAX_TURNS) return;
    L = &S->turns[S->n_turns++];
    memset(L, 0, sizeof *L);
    copy_text(L->text, sizeof L->text, turn);
    L->proved = proved;
    L->abstained = abstained;
    L->evolved = evolved;
    L->composed = composed;
    L->parrot_blocked = parrot;
    L->value = value;
    copy_text(L->detail, sizeof L->detail, detail ? detail : "");
}

int cnet_agi_scenario_turn(CnetAgiScenario *S, const char *turn) {
    CnetServeResult sr;
    double v;
    int rc;
    if (!S || !turn) return -1;

    /* Parrot / open-chat style asks: refuse mouth */
    if (starts_ci(turn, "chat ") || starts_ci(turn, "say anything") ||
        starts_ci(turn, "roleplay")) {
        S->parrot_blocks++;
        log_turn(S, turn, 0, 1, 0, 0, 1, 0.0, "parrot_blocked");
        return 1;
    }

    if (starts_ci(turn, "given")) {
        rc = cnet_agi_scenario_ingest(S, turn);
        log_turn(S, turn, 0, 0, 0, 0, 0, 0.0,
                 rc == 0 ? "given_ok" : "given_partial");
        /* high-value given table → evolve immediately */
        if (rc == 0) (void)cnet_agi_scenario_evolve_tick(S);
        return rc == 0 ? 0 : 1;
    }

    if (starts_ci(turn, "compose ")) {
        /* compose TAGA TAGB AS TAGOUT */
        char a[32], b[32], o[32];
        if (sscanf(turn + 8, "%31s %31s as %31s", a, b, o) == 3) {
            if (try_compose_if_ready(S, a, b, o) == 0) {
                log_turn(S, turn, 0, 0, 0, 1, 0, 0.0, "composed");
                return 0;
            }
        }
        log_turn(S, turn, 0, 1, 0, 0, 0, 0.2, "compose_fail");
        S->abstains++;
        return 1;
    }

    if (starts_ci(turn, "evolve")) {
        rc = cnet_agi_scenario_evolve_tick(S);
        log_turn(S, turn, 0, 0, rc == 0 ? 1 : 0, 0, 0, 0.0,
                 rc == 0 ? "evolved" : "evolve_noop");
        return rc == 0 ? 0 : 1;
    }

    /* CERT serve (given-info domains + factory bricks) */
    (void)cnet_serve_bank_load_dir(&S->serve, S->bricks_dir);
    if (cnet_serve_result(&S->serve, turn, &sr) == 0 && sr.proved) {
        S->cert_answers++;
        log_turn(S, turn, 1, 0, 0, 0, 0, 0.0, sr.brick);
        return 0;
    }

    /* Also try bus RESULT (in-memory bricks) */
    {
        CnetCoreBusResult br;
        if (cnet_core_bus_result(&S->bus, turn, &br) == 0 && br.proved) {
            S->cert_answers++;
            log_turn(S, turn, 1, 0, 0, 0, 0, 0.0, br.brick);
            return 0;
        }
    }

    /* Miss: value + log + maybe evolve */
    v = cnet_agi_scenario_value_miss(S, turn);
    S->value_sum += v;
    S->abstains++;
    {
        CnetDcMissRow row;
        memset(&row, 0, sizeof row);
        row.certified = 0;
        snprintf(row.goal_type, sizeof row.goal_type, "agi_turn");
        snprintf(row.abstain_reason, sizeof row.abstain_reason, "outside_table");
        snprintf(row.trace_id, sizeof row.trace_id, "t%d", S->n_turns);
        copy_text(row.term, sizeof row.term, turn);
        (void)cnet_dc_misslog_append(S->miss_path, &row);
    }
    if (v >= 0.7) (void)cnet_agi_scenario_evolve_tick(S);
    log_turn(S, turn, 0, 1, 0, 0, 0, v, "outside_table_abstain");
    return 1;
}

int cnet_agi_scenario_run_episode(CnetAgiScenario *S, CnetAgiScenarioBench *B) {
    static const char *script[] = {
        /* boot uses factory; then given info creates a new domain without Bonsai */
        "given domain user_pref pairs=0:1,1:2,2:3,3:4,4:5,5:6,6:7,7:8,8:9,9:10,10:11,11:12,12:13,13:14,14:15,15:0",
        "evolve",
        "user_pref 0",
        "user_pref 7",
        "user_pref 15",
        "q1_add16 3",
        "q1_xor16 3",
        "compose q1_add16 q1_xor16 as q1_comp",
        "q1_comp 3",
        "chat say anything wild",
        "roleplay you are god",
        "what is the meaning of life zz99",
        "given domain task_v lut=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0",
        "task_v 0",
        "task_v 15",
        NULL};
    double t0;
    int i, ans;
    if (!S || !B) return -1;
    memset(B, 0, sizeof *B);
    t0 = now_ms();
    unlink(S->miss_path);
    if (cnet_agi_scenario_boot(S, 1) != 0) return -1;
    for (i = 0; script[i]; ++i)
        (void)cnet_agi_scenario_turn(S, script[i]);
    B->ms_total = now_ms() - t0;
    B->turns = S->n_turns;
    B->cert_answers = S->cert_answers;
    B->abstains = S->abstains;
    B->evolves = S->evolves;
    B->composes = S->composes;
    B->parrot_blocks = S->parrot_blocks;
    B->given_ingests = S->given_ingests;
    B->bricks_end = S->serve.n > 0 ? S->serve.n : S->bus.n_bricks;
    ans = B->cert_answers + B->abstains;
    B->cert_rate = ans > 0 ? (double)B->cert_answers / (double)ans : 0.0;
    B->honesty =
        B->turns > 0 ? 1.0 - (double)B->parrot_blocks / (double)B->turns : 1.0;
    /* Pass bar: some cert, some honest abstain, parrot blocked, given used,
       compose or multi-brick, evolve happened */
    B->pass = (B->cert_answers >= 4 && B->abstains >= 1 && B->parrot_blocks >= 1 &&
               B->given_ingests >= 1 && B->bricks_end >= 2 && B->honesty >= 0.8 &&
               (B->composes >= 1 || B->evolves >= 1))
                  ? 1
                  : 0;
    return B->pass ? 0 : -1;
}
