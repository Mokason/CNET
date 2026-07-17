/* CLI for local EG / hill-climb log.
 * Build: make cnet_eg_cli → bin/cnet_eg
 *   bin/cnet_eg compute --work 64 --seals 2 [--baseline 32]
 *   bin/cnet_eg report --log path.jsonl --since unix [--baseline 32]
 *   bin/cnet_eg log --path path.jsonl --tick N --examined E --closed C ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_eg.h"

static void usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  cnet_eg compute --work W --seals S [--baseline B]\n"
            "  cnet_eg report --log FILE [--since UNIX] [--baseline B]\n"
            "  cnet_eg log --path FILE --tick N --examined E --closed C "
            "[--deferred D] [--no_oracle N] [--units U] [--curiosity C]\n");
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "";
    if (strcmp(cmd, "compute") == 0) {
        CnetEgSample s;
        CnetEgResult r;
        char buf[512];
        double baseline = 32.0;
        int i;
        memset(&s, 0, sizeof s);
        s.hours = 1.0;
        for (i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--work") && i + 1 < argc)
                s.teacher_work = strtoull(argv[++i], NULL, 10);
            else if (!strcmp(argv[i], "--seals") && i + 1 < argc)
                s.seals = strtoull(argv[++i], NULL, 10);
            else if (!strcmp(argv[i], "--baseline") && i + 1 < argc)
                baseline = atof(argv[++i]);
            else if (!strcmp(argv[i], "--hours") && i + 1 < argc)
                s.hours = atof(argv[++i]);
        }
        cnet_eg_compute(&s, baseline, &r);
        cnet_eg_result_json(&r, &s, buf, sizeof buf);
        printf("%s\n", buf);
        return 0;
    }
    if (strcmp(cmd, "report") == 0) {
        CnetEgSample s;
        CnetEgResult r;
        char buf[768];
        const char *log = NULL;
        int64_t since = 0;
        double baseline = 32.0;
        int i;
        for (i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
            else if (!strcmp(argv[i], "--since") && i + 1 < argc)
                since = (int64_t)atoll(argv[++i]);
            else if (!strcmp(argv[i], "--baseline") && i + 1 < argc)
                baseline = atof(argv[++i]);
        }
        if (!log) {
            usage();
            return 2;
        }
        if (cnet_eg_aggregate_file(log, since, 0, &s) != 0) {
            fprintf(stderr, "cnet_eg: cannot read %s\n", log);
            return 1;
        }
        cnet_eg_compute(&s, baseline, &r);
        printf("  teacher_work(examined)=%llu\n",
               (unsigned long long)s.teacher_work);
        printf("  seals(closed)=%llu\n", (unsigned long long)s.seals);
        printf("  deferred=%llu no_oracle=%llu curiosity=%llu\n",
               (unsigned long long)s.deferred, (unsigned long long)s.no_oracle,
               (unsigned long long)s.curiosity_proposed);
        printf("  units_end=%llu hours=%.3f\n", (unsigned long long)s.units,
               s.hours);
        printf("  cost_per_seal=%.4f  (lower better)\n", r.cost_per_seal);
        printf("  seal_rate=%.4f/h\n", r.seal_rate);
        printf("  local_eg=%.4f  (>1 better than baseline %.1f)\n", r.local_eg,
               r.baseline_cost);
        if (s.seals == 0)
            printf("  trend: no seals in window\n");
        else if (r.local_eg > 1.0)
            printf("  trend: climbing — %.2fx vs baseline\n", r.local_eg);
        else
            printf("  trend: below baseline efficiency\n");
        cnet_eg_result_json(&r, &s, buf, sizeof buf);
        printf("%s\n", buf);
        return 0;
    }
    if (strcmp(cmd, "log") == 0) {
        const char *path = NULL;
        uint64_t tick = 0, examined = 0, closed = 0, deferred = 0, no_oracle = 0;
        uint64_t units = 0, curiosity = 0;
        int i;
        for (i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--path") && i + 1 < argc) path = argv[++i];
            else if (!strcmp(argv[i], "--tick") && i + 1 < argc)
                tick = strtoull(argv[++i], NULL, 10);
            else if (!strcmp(argv[i], "--examined") && i + 1 < argc)
                examined = strtoull(argv[++i], NULL, 10);
            else if (!strcmp(argv[i], "--closed") && i + 1 < argc)
                closed = strtoull(argv[++i], NULL, 10);
            else if (!strcmp(argv[i], "--deferred") && i + 1 < argc)
                deferred = strtoull(argv[++i], NULL, 10);
            else if (!strcmp(argv[i], "--no_oracle") && i + 1 < argc)
                no_oracle = strtoull(argv[++i], NULL, 10);
            else if (!strcmp(argv[i], "--units") && i + 1 < argc)
                units = strtoull(argv[++i], NULL, 10);
            else if (!strcmp(argv[i], "--curiosity") && i + 1 < argc)
                curiosity = strtoull(argv[++i], NULL, 10);
        }
        if (!path) {
            usage();
            return 2;
        }
        return cnet_eg_log_tick(path, tick, examined, closed, deferred,
                                no_oracle, units, curiosity) == 0
                   ? 0
                   : 1;
    }
    usage();
    return 2;
}
