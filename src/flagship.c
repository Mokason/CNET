#include "../include/flagship.h"

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
           doubled id: systematic families must clear the Damerau-1 guard */
        snprintf(goal_tag, sizeof goal_tag, "wa%dq%d", t, t);
        /* the drain names acquisitions "acq_<goal tag>" — mirror it exactly
           so the resume check sees what the drain sealed */
        snprintf(name, sizeof name, "acq_%s", goal_tag);
        if (cnb_has_unit(&base, name)) { local.skipped_resume++; continue; }
        memset(&in_p, 0, sizeof in_p);
        in_p.family = PORT_ONEHOT;
        in_p.field_width = cfg->vocab_size;
        in_p.field_count = 1;
        if (port_set_tag(&in_p, "w_cur") != 0) break;
        goal_p = in_p;
        if (port_set_tag(&goal_p, goal_tag) != 0) break;

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

void flagship_print_report(const FlagshipReport *rep, FILE *out) {
    size_t i;
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
    fprintf(out, "  wall %.1fs (slept %.1fs)  max GPU temp %d C  stopped=%s\n",
            rep->wall_seconds, rep->slept_seconds, rep->max_gpu_temp_seen,
            rep->stopped == 0 ? "complete"
            : rep->stopped == 1 ? "wall_budget" : "stop_file");
}
