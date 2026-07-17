#include "../include/cnet_eg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef CNET_EG_EPS
#define CNET_EG_EPS 1e-9
#endif

void cnet_eg_compute(const CnetEgSample *s, double baseline_cost,
                     CnetEgResult *out) {
    double work, seals, hours, cost;
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s) return;
    if (baseline_cost <= 0.0) baseline_cost = 32.0;
    work = (double)s->teacher_work;
    seals = (double)s->seals;
    hours = s->hours > CNET_EG_EPS ? s->hours : CNET_EG_EPS;
    cost = work / (seals > 0.0 ? seals : 1.0);
    out->cost_per_seal = cost;
    out->seal_rate = seals / hours;
    out->baseline_cost = baseline_cost;
    out->local_eg = (cost > CNET_EG_EPS) ? (baseline_cost / cost) : 0.0;
    out->ok = 1;
}

int cnet_eg_log_tick(const char *path, uint64_t tick_no, uint64_t examined,
                     uint64_t closed, uint64_t deferred, uint64_t no_oracle,
                     uint64_t units, uint64_t curiosity) {
    FILE *f;
    time_t now;
    if (!path || !path[0]) return -1;
    f = fopen(path, "a");
    if (!f) return -1;
    now = time(NULL);
    fprintf(f,
            "{\"ts\":%lld,\"tick\":%llu,\"examined\":%llu,\"closed\":%llu,"
            "\"deferred\":%llu,\"no_oracle\":%llu,\"units\":%llu,"
            "\"curiosity\":%llu}\n",
            (long long)now, (unsigned long long)tick_no,
            (unsigned long long)examined, (unsigned long long)closed,
            (unsigned long long)deferred, (unsigned long long)no_oracle,
            (unsigned long long)units, (unsigned long long)curiosity);
    fclose(f);
    return 0;
}

int cnet_eg_aggregate_file(const char *path, int64_t t0, int64_t t1,
                           CnetEgSample *out) {
    FILE *f;
    char line[512];
    int64_t first_ts = 0, last_ts = 0;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!path || !path[0]) return -1;
    if (t1 <= 0) t1 = (int64_t)time(NULL);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        long long ts = 0;
        unsigned long long examined = 0, closed = 0, deferred = 0, no_oracle = 0;
        unsigned long long units = 0, curiosity = 0, tick = 0;
        if (sscanf(line,
                   "{\"ts\":%lld,\"tick\":%llu,\"examined\":%llu,\"closed\":%llu,"
                   "\"deferred\":%llu,\"no_oracle\":%llu,\"units\":%llu,"
                   "\"curiosity\":%llu}",
                   &ts, &tick, &examined, &closed, &deferred, &no_oracle, &units,
                   &curiosity) < 2)
            continue;
        if (ts < t0 || ts > t1) continue;
        if (first_ts == 0 || ts < first_ts) first_ts = ts;
        if (ts > last_ts) last_ts = ts;
        out->teacher_work += examined;
        out->seals += closed;
        out->deferred += deferred;
        out->no_oracle += no_oracle;
        out->curiosity_proposed += curiosity;
        if (units > out->units) out->units = units;
        (void)tick;
    }
    fclose(f);
    if (last_ts > first_ts)
        out->hours = (double)(last_ts - first_ts) / 3600.0;
    else if (out->teacher_work || out->seals)
        out->hours = 1.0 / 3600.0; /* single-second burst */
    else
        out->hours = CNET_EG_EPS;
    return 0;
}

int cnet_eg_result_json(const CnetEgResult *r, const CnetEgSample *s, char *buf,
                        size_t cap) {
    if (!r || !buf || cap < 32) return -1;
    snprintf(buf, cap,
             "{\"ok\":%d,\"cost_per_seal\":%.6f,\"seal_rate\":%.6f,"
             "\"local_eg\":%.6f,\"baseline_cost\":%.6f,"
             "\"teacher_work\":%llu,\"seals\":%llu,\"deferred\":%llu,"
             "\"no_oracle\":%llu,\"curiosity\":%llu,\"units\":%llu,"
             "\"hours\":%.6f}",
             r->ok, r->cost_per_seal, r->seal_rate, r->local_eg,
             r->baseline_cost,
             (unsigned long long)(s ? s->teacher_work : 0),
             (unsigned long long)(s ? s->seals : 0),
             (unsigned long long)(s ? s->deferred : 0),
             (unsigned long long)(s ? s->no_oracle : 0),
             (unsigned long long)(s ? s->curiosity_proposed : 0),
             (unsigned long long)(s ? s->units : 0), s ? s->hours : 0.0);
    return 0;
}
