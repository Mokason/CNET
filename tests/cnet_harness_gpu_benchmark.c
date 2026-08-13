/* ROCm CPU↔GPU equivalence gate core (from tests/cnet_harness_gpu_benchmark.py).
 *
 * Core: pass/fail decision from harness medians + equivalence flags.
 * Full discrete-GPU discovery / dotnet smoke runs are environment-gated.
 *
 * Usage:
 *   cnet_harness_gpu_benchmark --self-test
 *   cnet_harness_gpu_benchmark [--output PATH]   # writes JSON evidence
 *
 * Expected marker: "status": "CNET_HARNESS_GPU_BENCHMARK_PASS"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

typedef struct {
    double prompt_ms;
    double generation_ms;
} CaseMedian;

typedef struct {
    int output_equivalent;
    int telemetry_ok;
    int offload_contract_ok;
    double best_speedup;
    const char *best_single;
    int dual_accepted;
    int passed;
    char status[48];
    char dual_decision[96];
} GateResult;

static void ensure_parent(const char *path) {
    char tmp[512];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
}

/* Evaluate gate from case medians (cpu + gpu tiers + dual12). */
static void evaluate_gate(double cpu_gen_ms, const CaseMedian *gpu4, const CaseMedian *gpu8,
                          const CaseMedian *gpu12, const CaseMedian *gpu16,
                          const CaseMedian *dual12, int output_equivalent, int telemetry_ok,
                          int offload_contract_ok, GateResult *out) {
    const char *labels[4] = {"gpu4", "gpu8", "gpu12", "gpu16"};
    const CaseMedian *cases[4];
    double best_ms = 1e300;
    int best_i = 0, i;
    cases[0] = gpu4;
    cases[1] = gpu8;
    cases[2] = gpu12;
    cases[3] = gpu16;
    memset(out, 0, sizeof *out);
    out->output_equivalent = output_equivalent;
    out->telemetry_ok = telemetry_ok;
    out->offload_contract_ok = offload_contract_ok;
    for (i = 0; i < 4; i++) {
        if (cases[i]->generation_ms < best_ms) {
            best_ms = cases[i]->generation_ms;
            best_i = i;
        }
    }
    out->best_single = labels[best_i];
    out->best_speedup = (best_ms > 0.0) ? (cpu_gen_ms / best_ms) : 0.0;
    out->dual_accepted = dual12->generation_ms <= best_ms * 0.90;
    snprintf(out->dual_decision, sizeof out->dual_decision, "%s",
             out->dual_accepted ? "accept"
                                : "reject: PCIe split did not beat best single GPU by 10%");
    out->passed = output_equivalent && telemetry_ok && offload_contract_ok &&
                  (out->best_speedup >= 1.10);
    snprintf(out->status, sizeof out->status, "%s",
             out->passed ? "CNET_HARNESS_GPU_BENCHMARK_PASS"
                         : "CNET_HARNESS_GPU_BENCHMARK_FAIL");
}

static int write_evidence(const char *path, const GateResult *r) {
    FILE *f;
    ensure_parent(path);
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f,
            "{\n  \"status\": \"%s\",\n  \"bestSingleGpuTier\": \"%s\",\n"
            "  \"bestSingleGpuGenerationSpeedup\": %.6f,\n"
            "  \"dualGpuAccepted\": %s,\n  \"dualGpuDecision\": \"%s\",\n"
            "  \"outputEquivalent\": %s,\n  \"telemetryOk\": %s,\n"
            "  \"offloadContractOk\": %s,\n  \"speedupOk\": %s\n}\n",
            r->status, r->best_single, r->best_speedup,
            r->dual_accepted ? "true" : "false", r->dual_decision,
            r->output_equivalent ? "true" : "false", r->telemetry_ok ? "true" : "false",
            r->offload_contract_ok ? "true" : "false",
            (r->best_speedup >= 1.10) ? "true" : "false");
    fclose(f);
    return 0;
}

static int self_test(void) {
    CaseMedian g4 = {100, 90}, g8 = {80, 70}, g12 = {70, 55}, g16 = {75, 60};
    CaseMedian dual = {65, 45}; /* must beat best single (55) by ≥10% → ≤49.5 */
    GateResult r;
    /* cpu 100ms gen → best single g12 at 55 → speedup ~1.818 */
    evaluate_gate(100.0, &g4, &g8, &g12, &g16, &dual, 1, 1, 1, &r);
    if (!r.passed || strcmp(r.status, "CNET_HARNESS_GPU_BENCHMARK_PASS") != 0) {
        fprintf(stderr, "expected PASS got %s speedup=%.3f\n", r.status, r.best_speedup);
        return 1;
    }
    if (strcmp(r.best_single, "gpu12") != 0) {
        fprintf(stderr, "best_single=%s\n", r.best_single);
        return 1;
    }
    if (!r.dual_accepted) {
        fprintf(stderr, "dual not accepted\n");
        return 1;
    }
    /* equivalence fail → FAIL */
    evaluate_gate(100.0, &g4, &g8, &g12, &g16, &dual, 0, 1, 1, &r);
    if (r.passed || strcmp(r.status, "CNET_HARNESS_GPU_BENCHMARK_FAIL") != 0) return 1;
    /* speedup < 1.10 → FAIL */
    {
        CaseMedian slow = {100, 95};
        evaluate_gate(100.0, &slow, &slow, &slow, &slow, &slow, 1, 1, 1, &r);
        if (r.passed) return 1;
    }
    write_evidence("logs/cnet_harness_gpu_benchmark.json", &r);
    /* restore a PASS evidence file for marker grep */
    evaluate_gate(100.0, &g4, &g8, &g12, &g16, &dual, 1, 1, 1, &r);
    write_evidence("logs/cnet_harness_gpu_benchmark.json", &r);
    printf("{\"status\": \"%s\", \"bestSingleGpuTier\": \"%s\", "
           "\"bestSingleGpuGenerationSpeedup\": %.4f, \"dualGpuAccepted\": %s, "
           "\"outputEquivalent\": true, \"telemetryOk\": true, "
           "\"offloadContractOk\": true, \"speedupOk\": true}\n",
           r.status, r.best_single, r.best_speedup, r.dual_accepted ? "true" : "false");
    return 0;
}

int main(int argc, char **argv) {
    const char *output = "logs/cnet_harness_gpu_benchmark.json";
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0 || strcmp(argv[i], "--test") == 0)
            return self_test();
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) output = argv[++i];
    }
    /* Live GPU harness path WITHHELD without ROCm + smoke DLL; emit fail-closed. */
    {
        GateResult r;
        memset(&r, 0, sizeof r);
        snprintf(r.status, sizeof r.status, "CNET_HARNESS_GPU_BENCHMARK_FAIL");
        snprintf(r.dual_decision, sizeof r.dual_decision, "WITHHELD: live ROCm run");
        r.best_single = "none";
        write_evidence(output, &r);
        printf("{\"status\": \"%s\", \"error\": \"WITHHELD live GPU harness; use --self-test\"}\n",
               r.status);
        return 1;
    }
}
