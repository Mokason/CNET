#include "../include/flagship.h"
#include "../include/contract/conformal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static void fs_sleep_ms(unsigned ms) { Sleep(ms); }
static void fs_lower_priority(void) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
}
#define FS_POPEN _popen
#define FS_PCLOSE _pclose
#else
#include <unistd.h>
static void fs_sleep_ms(unsigned ms) { usleep(ms * 1000u); }
static void fs_lower_priority(void) {}
#define FS_POPEN popen
#define FS_PCLOSE pclose
#endif

/* GPU temperature via nvidia-smi; -1 when unavailable (no GPU / no tool).
   Called at most once per attempt — an attempt costs seconds, the query ~0.1s. */
static int fs_gpu_temp(void) {
    FILE *p = FS_POPEN(
        "nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits",
        "r");
    int t = -1;
    if (!p) return -1;
    if (fscanf(p, "%d", &t) != 1) t = -1;
    FS_PCLOSE(p);
    return t;
}

static double fs_now(void) { return (double)time(NULL); }

void flagship_config_defaults(FlagshipConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->checkpoint_every = 4;
    cfg->gpu_temp_limit_c = 80;
    cfg->duty_fraction = 0.75;
    cfg->cooldown_ms = 5000;
    cfg->max_wall_seconds = 0.0;
    cfg->below_normal_priority = 1;
    cfg->task = FLAGSHIP_TASK_ARGMAX;
    cfg->topk = 3;
    cfg->conformal_alpha = 0.05;
    cfg->conformal_n = 256;
    acquire_config_defaults(&cfg->acq);
}

static void report_tally_defer(FlagshipReport *rep, const char *reason) {
    size_t i;
    rep->deferred++;
    for (i = 0; i < rep->reason_kinds; ++i) {
        if (strcmp(rep->reasons[i], reason) == 0) {
            rep->reason_counts[i]++;
            return;
        }
    }
    if (rep->reason_kinds < FLAGSHIP_MAX_REASONS) {
        snprintf(rep->reasons[rep->reason_kinds], ACQUIRE_REASON_MAX, "%s",
                 reason);
        rep->reason_counts[rep->reason_kinds] = 1;
        rep->reason_kinds++;
    }
}

static int stop_file_present(const char *base_path) {
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s.stop", base_path);
    f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void checkpoint(const CnetBase *base, const AcquireLedger *led,
                       const FlagshipConfig *cfg) {
    cnb_save(base, cfg->base_path);
    if (cfg->ledger_path) acquire_ledger_save(led, cfg->ledger_path);
}

/* Per-query calibrated abstention probe (report-only). Mines calibration and
   test strides from the oracle at phases 1/3 and 2/3 between the domain's
   stride points (deterministic; disjoint from the drain's phase-0 stride up
   to rounding — an approximation, stated in the spec), calibrates
   split-conformal on a FRESH copy of the sealed unit, and measures
   answered / abstained / wrong on the test stride. Aggregates into rep. */
static void conformal_probe(const FlagshipConfig *cfg, const CnetBase *base,
                            const char *unit_name, const FlagshipOracle *orc,
                            Port in_p, Port goal_p, FlagshipReport *rep) {
    BinaryTransformNetwork btn;
    Contract c, tc;
    size_t card, n = cfg->conformal_n, i, m = 0;
    size_t in_total = in_p.field_width * in_p.field_count;
    size_t out_total = goal_p.field_width * goal_p.field_count;
    double *inputs = NULL, *raw = NULL;
    size_t *truth = NULL;
    ConformalCalibrator cal;
    int have_unit = 0;

    if (goal_p.field_count != 1) return;   /* classifier outputs only */
    if (cnb_get_unit(base, unit_name, &btn, &c) != 0) return;
    have_unit = 1;

    memset(&tc, 0, sizeof tc);
    snprintf(tc.name, CONTRACT_NAME_MAX, "conf_tmp");
    tc.input_ports[0] = in_p;
    tc.input_port_count = 1;
    tc.output_ports[0] = goal_p;
    tc.output_port_count = 1;
    if (!contract_domain_cardinality(&tc, &card) || card < 3 * n) goto out;

    inputs = (double *)malloc(n * in_total * sizeof *inputs);
    raw = (double *)malloc(out_total * sizeof *raw);
    truth = (size_t *)malloc(n * sizeof *truth);
    if (!inputs || !raw || !truth) goto out;

    /* calibration stride (phase 1/3) */
    for (i = 0; i < n; ++i) {
        size_t idx = ((3 * i + 1) * card) / (3 * n);
        double *row = inputs + m * in_total;
        size_t a;
        if (contract_encode_domain_point(&tc, idx, row) != 0) continue;
        if (orc->fn(row, raw, orc->ctx) != 0) continue;
        if (!port_validate(goal_p, raw)) continue;
        for (a = 1, truth[m] = 0; a < goal_p.field_width; ++a)
            if (raw[a] > raw[truth[m]]) truth[m] = a;
        m++;
    }
    if (m < 8 ||
        conformal_calibrate_btn(&cal, &btn, &c, inputs, truth, m,
                                cfg->conformal_alpha) != 0) {
        goto out;
    }

    /* test stride (phase 2/3) */
    for (i = 0; i < n; ++i) {
        size_t idx = ((3 * i + 2) * card) / (3 * n);
        double row[4096];
        size_t a, tru = 0;
        int pred;
        if (in_total > 4096) break;
        if (contract_encode_domain_point(&tc, idx, row) != 0) continue;
        if (orc->fn(row, raw, orc->ctx) != 0) continue;
        if (!port_validate(goal_p, raw)) continue;
        for (a = 1; a < goal_p.field_width; ++a)
            if (raw[a] > raw[tru]) tru = a;
        pred = conformal_classify_or_abstain(&cal, &btn, &c, row);
        if (pred < 0) {
            rep->conf_abstained++;
        } else {
            rep->conf_answered++;
            if ((size_t)pred != tru) rep->conf_wrong++;
        }
    }
    rep->conf_units++;

out:
    free(inputs);
    free(raw);
    free(truth);
    if (have_unit) {
        btn_free(&btn);
        contract_free(&c);
    }
}

int flagship_run(FlagshipConfig *cfg, FlagshipOracleMaker maker,
                 void *maker_ctx, FlagshipReport *rep) {
    CnetBase base;
    AcquireLedger led;
    PrimitiveRegistry reg;
    size_t k, n_tokens, since_checkpoint = 0;
    double t_start;
    FlagshipReport local;

    if (!cfg || !maker || !cfg->vocab_tokens || cfg->vocab_size == 0 ||
        !cfg->base_path || cfg->duty_fraction <= 0.0 ||
        cfg->duty_fraction > 1.0 ||
        cfg->vocab_size < cfg->acq.min_evidence) {
        return -1;
    }
    if (cfg->vocab_size > 4096) return -1;   /* ONEHOT width sanity bound */

    memset(&local, 0, sizeof local);
    local.max_gpu_temp_seen = -1;

    if (cfg->below_normal_priority) fs_lower_priority();

    /* resume: the base is the checkpoint */
    cnb_init(&base);
    (void)cnb_load(&base, cfg->base_path);   /* absent file = fresh run */
    acquire_ledger_init(&led);
    if (cfg->ledger_path) (void)acquire_ledger_load(&led, cfg->ledger_path);

    /* prior units join the planner (certify-on-load; failures counted) */
    registry_init(&reg);
    if (cnb_load_registry(&base, &reg, &local.registry_skipped) != 0) {
        cnb_free(&base);
        acquire_ledger_free(&led);
        registry_free(&reg);
        return -1;
    }

    cfg->acq.base = &base;

    n_tokens = cfg->vocab_size;
    if (cfg->max_units && cfg->max_units < n_tokens) n_tokens = cfg->max_units;

    t_start = fs_now();

    for (k = 0; k < n_tokens; ++k) {
        int t = cfg->vocab_tokens[k];
        char name[ACQUIRE_NAME_MAX];
        char goal_tag[PORT_TAG_MAX];
        Port in_p, goal_p;
        FlagshipOracle orc_fn;
        OracleRegistry orc;
        AcquireReport arep;

        /* ---- governor gate (before each attempt) ---- */
        if (stop_file_present(cfg->base_path)) { local.stopped = 2; break; }
        if (cfg->max_wall_seconds > 0.0 &&
            fs_now() - t_start >= cfg->max_wall_seconds) {
            local.stopped = 1;
            break;
        }
        if (cfg->gpu_temp_limit_c > 0) {
            int temp = fs_gpu_temp();
            if (temp > local.max_gpu_temp_seen) local.max_gpu_temp_seen = temp;
            while (temp >= 0 && temp > cfg->gpu_temp_limit_c) {
                fs_sleep_ms(cfg->cooldown_ms);
                local.slept_seconds += (double)cfg->cooldown_ms / 1000.0;
                if (stop_file_present(cfg->base_path)) break;
                temp = fs_gpu_temp();
                if (temp > local.max_gpu_temp_seen)
                    local.max_gpu_temp_seen = temp;
            }
        }
        if (cfg->duty_fraction < 1.0) {
            /* keep work/wall <= duty: sleep off the excess */
            double wall = fs_now() - t_start;
            double work = wall - local.slept_seconds;
            if (wall > 0.0 && work / cfg->duty_fraction > wall) {
                double need = work / cfg->duty_fraction - wall;
                if (need > 60.0) need = 60.0;   /* bounded quantum */
                fs_sleep_ms((unsigned)(need * 1000.0));
                local.slept_seconds += need;
            }
        }

        /* ---- one conditioning token = one candidate unit ----
           doubled id: systematic families must clear the Damerau-1 guard;
           family prefixes (wa/pr/tk) are pairwise Damerau-spaced >= 2 */
        memset(&in_p, 0, sizeof in_p);
        in_p.family = PORT_ONEHOT;
        in_p.field_width = cfg->vocab_size;
        in_p.field_count = 1;
        goal_p = in_p;
        switch (cfg->task) {
        case FLAGSHIP_TASK_PAIR:
            snprintf(goal_tag, sizeof goal_tag, "pr%dq%d", t, t);
            in_p.field_count = 2;                 /* (w_prev, w_cur) */
            if (port_set_tag(&in_p, "w_pair") != 0) k = n_tokens;
            break;
        case FLAGSHIP_TASK_TOPK:
            snprintf(goal_tag, sizeof goal_tag, "tk%dq%d", t, t);
            if (port_set_tag(&in_p, "w_cur") != 0) k = n_tokens;
            goal_p.field_count = cfg->topk;       /* ranked choices, in order */
            break;
        default:
            snprintf(goal_tag, sizeof goal_tag, "wa%dq%d", t, t);
            if (port_set_tag(&in_p, "w_cur") != 0) k = n_tokens;
            break;
        }
        if (k >= n_tokens) break;                 /* tag failure: abort sweep */
        if (port_set_tag(&goal_p, goal_tag) != 0) break;
        /* the drain names acquisitions "acq_<goal tag>" — mirror it exactly
           so the resume check sees what the drain sealed */
        snprintf(name, sizeof name, "acq_%s", goal_tag);
        if (cnb_has_unit(&base, name)) { local.skipped_resume++; continue; }

        if (maker(maker_ctx, k, t, &orc_fn) != 0 || orc_fn.fn == NULL) {
            local.no_oracle++;
            continue;
        }
        memset(&orc, 0, sizeof orc);
        if (acquire_oracle_register(&orc, "cce_cond_next", in_p, goal_p,
                                    orc_fn.fn, orc_fn.ctx) != 0) {
            local.no_oracle++;
            continue;
        }

        local.attempted++;
        memset(&arep, 0, sizeof arep);
        if (acquire_now(&reg, &led, &orc, &cfg->acq, in_p, goal_p, &arep) == 0) {
            local.acquired++;
            if (arep.last_verdict == CERT_PROVEN) {
                local.proof_count++;
            } else {
                local.sampled_count++;
                if (local.bound_count < 1024)
                    local.bounds[local.bound_count++] = arep.last_bound;
            }
            if (local.margin_count < 1024)
                local.margins[local.margin_count++] = arep.last_min_margin;
            /* per-query calibrated abstention, PAIR + SAMPLED only */
            if (cfg->task == FLAGSHIP_TASK_PAIR &&
                arep.last_verdict == CERT_SAMPLED &&
                cfg->conformal_alpha > 0.0) {
                conformal_probe(cfg, &base, name, &orc_fn, in_p, goal_p,
                                &local);
            }
        } else {
            report_tally_defer(&local, arep.last_defer_reason[0]
                                           ? arep.last_defer_reason
                                           : "no_oracle");
        }

        if (++since_checkpoint >= cfg->checkpoint_every) {
            checkpoint(&base, &led, cfg);
            since_checkpoint = 0;
        }
    }

    checkpoint(&base, &led, cfg);
    local.wall_seconds = fs_now() - t_start;

    registry_free(&reg);
    acquire_ledger_free(&led);
    cnb_free(&base);

    if (rep) *rep = local;
    return 0;
}

static int fs_cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* min/median/max of an UNSORTED sample (sorts a scratch copy). */
static void fs_dist(const double *v, size_t n, double *mn, double *md,
                    double *mx) {
    double tmp[1024];
    if (n == 0 || n > 1024) { *mn = *md = *mx = 0.0; return; }
    memcpy(tmp, v, n * sizeof *tmp);
    qsort(tmp, n, sizeof *tmp, fs_cmp_double);
    *mn = tmp[0];
    *md = tmp[n / 2];
    *mx = tmp[n - 1];
}

void flagship_print_report(const FlagshipReport *rep, FILE *out) {
    size_t i;
    double mn, md, mx;
    if (!rep || !out) return;
    fprintf(out, "flagship: attempted %lu, acquired %lu, deferred %lu, "
                 "resume-skips %lu, no-oracle %lu, load-skips %lu\n",
            (unsigned long)rep->attempted, (unsigned long)rep->acquired,
            (unsigned long)rep->deferred, (unsigned long)rep->skipped_resume,
            (unsigned long)rep->no_oracle, (unsigned long)rep->registry_skipped);
    for (i = 0; i < rep->reason_kinds; ++i) {
        fprintf(out, "  defer %-24s %lu\n", rep->reasons[i],
                (unsigned long)rep->reason_counts[i]);
    }
    fprintf(out, "  tiers: PROOF %lu | SAMPLED %lu",
            (unsigned long)rep->proof_count, (unsigned long)rep->sampled_count);
    if (rep->bound_count > 0) {
        fs_dist(rep->bounds, rep->bound_count, &mn, &md, &mx);
        fprintf(out, " (wilson floor min/med/max %.4f/%.4f/%.4f)", mn, md, mx);
    }
    fprintf(out, "\n");
    if (rep->margin_count > 0) {
        fs_dist(rep->margins, rep->margin_count, &mn, &md, &mx);
        fprintf(out, "  certified min-margin min/med/max %.4f/%.4f/%.4f\n",
                mn, md, mx);
    }
    if (rep->conf_units > 0) {
        size_t total = rep->conf_answered + rep->conf_abstained;
        fprintf(out, "  conformal: %lu units, answered %lu/%lu (%.1f%%), "
                     "abstained %lu, empirical risk %.4f\n",
                (unsigned long)rep->conf_units,
                (unsigned long)rep->conf_answered, (unsigned long)total,
                total ? 100.0 * (double)rep->conf_answered / (double)total : 0.0,
                (unsigned long)rep->conf_abstained,
                rep->conf_answered
                    ? (double)rep->conf_wrong / (double)rep->conf_answered
                    : 0.0);
    }
    fprintf(out, "  wall %.1fs (slept %.1fs)  max GPU temp %d C  stopped=%s\n",
            rep->wall_seconds, rep->slept_seconds, rep->max_gpu_temp_seen,
            rep->stopped == 0 ? "complete"
            : rep->stopped == 1 ? "wall_budget" : "stop_file");
}
