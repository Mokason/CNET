/* Synthetic registry measurement, not acquired knowledge. The independent
 * integer formula supplies all labels; ordinary certification remains intact. */
#include "cnet_capsule.h"
#include "cnet_capsule_core.h"
#include "cnet_core_host.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { ROWS = 5, LIMIT = 4096, TIMING_REPEATS = 3 };
static void require(int ok, const char *reason) {
    if (!ok) { fprintf(stderr, "CAPSULE_SCALE_REFUSED %s\n", reason); exit(1); }
}
static double now_ms(void) {
    struct timespec t;
    require(!clock_gettime(CLOCK_MONOTONIC, &t), "clock");
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static unsigned number(const char *s) {
    unsigned n = 0;
    if (!*s || (*s == '0' && s[1])) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > LIMIT / 10) return 0;
        n = n * 10 + (unsigned)(*s - '0');
        if (n > LIMIT) return 0;
    }
    return n;
}
static void path_join(char out[1200], const char *root, const char *leaf) {
    int n = snprintf(out, 1200, "%s/%s", root, leaf);
    require(n > 0 && n < 1200, "path_length");
}
static Port port(unsigned i, int output) {
    Port p = {0}; p.family = PORT_BINARY_MSB;
    p.field_width = output ? 16 : 8; p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s%04uq%04u", output ? "target" : "source", i, i);
    return p;
}
static void unit_name(unsigned i, char name[64]) {
    snprintf(name, 64, "scale_unit_%04u", i);
}
static void build_one(const char *root, unsigned i, unsigned count) {
    CnetBase base; HybridAi cov; BinaryTransformNetwork btn = {0};
    Contract ct = {0}; CertifyReport cert = {0}; CnetCapsuleReport report = {0};
    double input[ROWS * 8], output[ROWS * 16]; char name[64], leaf[96], path[1200];
    cnb_init(&base); hybrid_ai_init(&cov); unit_name(i, name);
    Port pin = port(i, 0), pout = port(i, 1);
    require(!btn_init(&btn, 8, 16, ROWS, ROWS, .1, 7) && !btn_set_ports(&btn, pin, pout), "fixture_btn");
    /* Same finite-row detector construction as the ordinary table builder.
     * All ROWS weights are set explicitly, then robust certification checks
     * the supplied labels at its unchanged margin, followed by sealed export. */
    for (unsigned row = 0; row < ROWS; row++) {
        unsigned ones = 0, expected = 8 * i + row;
        for (unsigned bit = 0; bit < 8; bit++) {
            unsigned value = (row >> (7 - bit)) & 1u; ones += value;
            input[row * 8 + bit] = value;
            btn.input_hidden[row * 8 + bit] = value ? 32 : -32;
        }
        btn.hidden_bias[row] = 32 * (.5 - ones);
        for (unsigned bit = 0; bit < 16; bit++) {
            unsigned value = (expected >> (15 - bit)) & 1u;
            output[row * 16 + bit] = value;
            btn.hidden_output_weights[bit * ROWS + row] = value ? 32 : 0;
        }
    }
    for (unsigned bit = 0; bit < 16; bit++) btn.output_bias[bit] = -16;
    require(!contract_init_borrowed(&ct, name, &btn, input, output, ROWS) &&
            !btn_certify_robust(&btn, &ct, .05, &cert), "fixture_certification");
    require(!cnb_add_unit(&base, &btn, &ct, NULL) &&
            !cnb_add_oracle_desc(&base, "synthetic_scale_formula", "verified_tool", pin, pout) &&
            !cnb_set_unit_provenance(&base, name, "synthetic_scale_formula") &&
            !hybrid_coverage_record(&cov, pin, pout, name, input, output, ROWS, 8, 16), "fixture_seal");
    snprintf(leaf, sizeof leaf, "after/%s", name); path_join(path, root, leaf);
    require(!cnet_capsule_export(&base, &cov, name, path, &report), "fixture_export_after");
    require(!strcmp(report.scope, "sampled") && report.coverage_rows == ROWS, "fixture_scope");
    if (i + 1 < count) {
        snprintf(leaf, sizeof leaf, "before/%s", name); path_join(path, root, leaf);
        require(!cnet_capsule_export(&base, &cov, name, path, &report), "fixture_export_before");
    }
    contract_free(&ct); btn_free(&btn); hybrid_ai_free(&cov); cnb_free(&base);
}
static void build(const char *root, unsigned count) {
    double start = now_ms();
    char path[1200];
    path_join(path, root, "before"); require(!mkdir(path, 0700), "new_before_directory");
    path_join(path, root, "after"); require(!mkdir(path, 0700), "new_after_directory");
    for (unsigned i = 0; i < count; i++) build_one(root, i, count);
    printf("{\"event\":\"fixture\",\"status\":\"pass\",\"capsules\":%u,\"synthetic_facts\":%u,\"build_ms\":%.6f}\n", count, count * ROWS, now_ms() - start);
}
static size_t rss_bytes(void) {
    FILE *f = fopen("/proc/self/smaps_rollup", "r"); char line[256]; size_t kib = 0;
    require(f != NULL, "rss_open");
    while (fgets(line, sizeof line, f)) if (sscanf(line, "Rss: %zu kB", &kib) == 1) break;
    require(!ferror(f) && !fclose(f) && kib, "rss_read");
    return kib * 1024;
}
static size_t peak_rss_bytes(void) {
    FILE *f = fopen("/proc/self/status", "r"); char line[256]; size_t kib = 0;
    require(f != NULL, "peak_rss_open");
    while (fgets(line, sizeof line, f)) if (sscanf(line, "VmHWM: %zu kB", &kib) == 1) break;
    require(!ferror(f) && !fclose(f) && kib, "peak_rss_read");
    return kib * 1024;
}
static int hash_order(const void *a, const void *b) { return strcmp(a, b); }
static void identities(CnetCoreHost *host, unsigned expected) {
    CnetCapsuleIdentity *ids = calloc(LIMIT, sizeof *ids);
    char (*hashes)[65] = calloc(LIMIT, sizeof *hashes); size_t count = 0;
    require(ids && hashes && !cnet_core_host_history(host, 0, ids, LIMIT, &count) &&
            count == expected, "identity_count");
    for (unsigned i = 0; i < expected; i++) {
        char name[64]; unit_name(i, name);
        require(!strcmp(name, ids[i].unit) && strlen(ids[i].sha256) == 64, "identity_name_or_hash");
        memcpy(hashes[i], ids[i].sha256, 65);
    }
    qsort(hashes, count, sizeof *hashes, hash_order);
    for (size_t i = 1; i < count; i++) require(strcmp(hashes[i - 1], hashes[i]) != 0, "replicated_identity");
    free(hashes); free(ids);
}
static double ask(CnetCoreLease *lease, unsigned source, unsigned target, unsigned key, int positive) {
    char request[128], name[64]; CnetCapsuleCoreReply reply;
    Port pin = port(source, 0), pout = port(target, 1);
    snprintf(request, sizeof request, "capsule %s %s %u", pin.tag, pout.tag, key);
    double start = now_ms(); int rc = cnet_core_host_ask(lease, request, &reply);
    double elapsed = (now_ms() - start) * 1000.0;
    if (positive) {
        unit_name(source, name);
        require(!rc && reply.verified && reply.value == 8 * source + key &&
                reply.hops == 1 && !strcmp(reply.units, name), "wrong_or_missing_certified_answer");
    } else require(rc && !reply.verified && !reply.value && !reply.hops && !reply.units[0], "wrong_verified_or_partial_refusal");
    return elapsed;
}
static int double_order(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y);
}
static double percentile(const double *sorted, size_t count, unsigned percent) {
    require(count && percent && percent <= 100, "quantile_arguments");
    return sorted[(count * percent + 99) / 100 - 1];
}
static void metric(const char *name, double *values, size_t count) {
    qsort(values, count, sizeof *values, double_order);
    printf(",\"%s\":{\"samples\":%zu,\"p50\":%.6f,\"p95\":%.6f,\"p99\":%.6f,\"max\":%.6f}",
           name, count, percentile(values, count, 50), percentile(values, count, 95),
           percentile(values, count, 99), values[count - 1]);
}
static void measure(const char *root, unsigned count) {
    char before[1200], after[1200]; path_join(before, root, "before"); path_join(after, root, "after");
    size_t baseline = rss_bytes(); double start = now_ms();
    CnetCoreHost *host = cnet_core_host_open(before);
    double load_ms = now_ms() - start; require(host != NULL, "incumbent_load_or_replay");
    size_t loaded = rss_bytes(); identities(host, count - 1);
    CnetCoreLease *old = cnet_core_host_pin(host); require(old != NULL, "old_pin");
    uint64_t old_generation = cnet_core_host_generation(old), old_revision = cnet_core_host_revision(host), id = 0;
    for (unsigned i = 0; i + 1 < count; i++) for (unsigned x = 0; x < ROWS; x++) ask(old, i, i, x, 1);
    ask(old, count - 1, count - 1, 0, 0);
    start = now_ms(); require(!cnet_core_host_stage_registry(host, after, 1, &id), "candidate_load_or_preserve_replay");
    double stage_ms = now_ms() - start; size_t overlap = rss_bytes();
    require(id && id != old_generation && cnet_core_host_revision(host) == old_revision, "staging_lifetime");
    start = now_ms(); require(!cnet_core_host_activate(host, id), "activate");
    double activate_ms = now_ms() - start; size_t activated = rss_bytes();
    require(cnet_core_host_revision(host) == old_revision + 1, "activation_revision");
    identities(host, count);
    CnetCoreLease *active = cnet_core_host_pin(host); require(active && cnet_core_host_generation(active) == id, "active_pin");
    for (unsigned i = 0; i + 1 < count; i++) for (unsigned x = 0; x < ROWS; x++) ask(old, i, i, x, 1);
    ask(old, count - 1, count - 1, 0, 0);
    size_t correct = 0, refused = 0;
    for (unsigned i = 0; i < count; i++) {
        for (unsigned x = 0; x < 256; x++) {
            ask(active, i, i, x, x < ROWS);
            if (x < ROWS) correct++; else refused++;
        }
        ask(active, i, (i + 1) % count, 0, 0);
    }
    /* The exhaustive sweep warms the measured inventory. Time actual host asks,
     * excluding request formatting, the independent checker and sorting. */
    size_t nc = (size_t)count * ROWS * TIMING_REPEATS, no = (size_t)count * 2 * TIMING_REPEATS;
    size_t nx = (size_t)count * TIMING_REPEATS, ci = 0, oi = 0, xi = 0;
    double *covered = calloc(nc, sizeof *covered), *ood = calloc(no, sizeof *ood), *cross = calloc(nx, sizeof *cross);
    require(covered && ood && cross, "samples_allocation");
    for (unsigned rep = 0; rep < TIMING_REPEATS; rep++) for (unsigned i = 0; i < count; i++) {
        for (unsigned x = 0; x < ROWS; x++) covered[ci++] = ask(active, i, i, x, 1);
        ood[oi++] = ask(active, i, i, ROWS, 0); ood[oi++] = ask(active, i, i, 255, 0);
        cross[xi++] = ask(active, i, (i + 1) % count, 0, 0);
    }
    require(ci == nc && oi == no && xi == nx, "samples_count");
    start = now_ms(); require(!cnet_core_host_rollback(host), "rollback");
    double rollback_ms = now_ms() - start;
    CnetCoreLease *rolled = cnet_core_host_pin(host);
    require(rolled && cnet_core_host_generation(rolled) == old_generation &&
            cnet_core_host_revision(host) == old_revision + 2, "rollback_generation");
    ask(rolled, count - 1, count - 1, 0, 0);
    for (unsigned i = 0; i + 1 < count; i++) ask(rolled, i, i, 0, 1);
    ask(active, count - 1, count - 1, 0, 1); /* Pinned N view survives rollback. */
    require(cnet_core_host_close(host) != 0, "close_must_refuse_pins");
    size_t before_close = rss_bytes();
    cnet_core_host_unpin(rolled); cnet_core_host_unpin(active); cnet_core_host_unpin(old);
    require(!cnet_core_host_close(host), "close_after_unpin");
    size_t closed = rss_bytes(), observed_peak = baseline;
    size_t checkpoints[] = { loaded, overlap, activated, before_close, closed };
    for (size_t i = 0; i < sizeof checkpoints / sizeof *checkpoints; i++)
        if (checkpoints[i] > observed_peak) observed_peak = checkpoints[i];
    printf("{\"event\":\"measurement\",\"status\":\"pass\",\"capsules\":%u,\"distinct_identities\":%u,"
           "\"correct\":%zu,\"ood_refused\":%zu,\"cross_port_refused\":%u,\"wrong_verified\":0,"
           "\"old_pinned_correct\":%u,\"old_pinned_new_unit_refused\":true,\"rollback_checked\":true,"
           "\"load_ms\":%.6f,\"stage_ms\":%.6f,\"activate_ms\":%.6f,\"rollback_ms\":%.6f,"
           "\"rss_baseline_bytes\":%zu,\"rss_loaded_bytes\":%zu,\"rss_overlap_bytes\":%zu,"
           "\"rss_activated_bytes\":%zu,\"rss_peak_observed_bytes\":%zu,\"rss_hwm_approx_bytes\":%zu,\"rss_closed_bytes\":%zu",
           count, count, correct, refused, count, ROWS * (count - 1), load_ms, stage_ms, activate_ms, rollback_ms,
           baseline, loaded, overlap, activated, observed_peak, peak_rss_bytes(), closed);
    metric("covered_us", covered, nc); metric("ood_us", ood, no); metric("cross_port_us", cross, nx);
    puts("}"); free(covered); free(ood); free(cross);
}
int main(int argc, char **argv) {
    unsigned count;
    if (argc == 2 && !strcmp(argv[1], "selftest")) {
        double samples[100]; for (size_t i = 0; i < 100; i++) samples[i] = (double)i + 1;
        require(percentile(samples, 100, 50) == 50 && percentile(samples, 100, 95) == 95 &&
                percentile(samples, 100, 99) == 99 && percentile(samples, 1, 99) == 1, "quantile_selftest");
        puts("CAPSULE_SCALE_QUANTILE_PASS"); return 0;
    }
    if (argc != 4 || !(count = number(argv[3])) || count < 2) return 2;
    if (!strcmp(argv[1], "build")) build(argv[2], count);
    else if (!strcmp(argv[1], "measure")) measure(argv[2], count);
    else return 2;
    require(!fflush(stdout) && !ferror(stdout), "output_write");
    return 0;
}
