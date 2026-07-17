#include "../include/cnet_placement.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int cnet_mem_available(uint64_t *avail_out, uint64_t *total_out) {
    FILE *f;
    char key[64];
    unsigned long long v;
    uint64_t avail = 0, total = 0;
    if (avail_out) *avail_out = 0;
    if (total_out) *total_out = 0;
    f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    while (fscanf(f, "%63s %llu kB\n", key, &v) == 2) {
        if (strcmp(key, "MemAvailable:") == 0) avail = v * 1024ull;
        else if (strcmp(key, "MemTotal:") == 0) total = v * 1024ull;
    }
    fclose(f);
    if (avail_out) *avail_out = avail;
    if (total_out) *total_out = total;
    return (avail > 0) ? 0 : -1;
}

uint64_t cnet_file_size(const char *path) {
    struct stat st;
    if (!path || !path[0]) return 0;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    return (uint64_t)st.st_size;
}

static void copy_path(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;
    snprintf(dst, cap, "%s", src);
}

static uint64_t count_window_ids(const char *path) {
    FILE *f;
    char line[64];
    uint64_t n = 0;
    if (!path || !path[0]) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *e;
        long v = strtol(line, &e, 10);
        if (e != line && v >= 0) n++;
    }
    fclose(f);
    return n;
}

/* Working-set envelope: GGUF size * 1.15 + 512 MiB KV/scratch (honest upper). */
static uint64_t peak_for_gguf(uint64_t gguf_bytes) {
    if (gguf_bytes == 0) return 0;
    return (uint64_t)(gguf_bytes * 1.15) + (512ull << 20);
}

int cnet_placement_plan(CnetPlacementPlan *out, const char *cnb_path,
                        const char *residual_path, const char *teacher_path,
                        const char *window_path, int safety_pct) {
    const char *e;
    uint64_t budget;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (safety_pct <= 0 || safety_pct > 100) safety_pct = 70;

    if (!cnb_path || !cnb_path[0]) {
        e = getenv("CNET_BASE_PATH");
        cnb_path = e && e[0] ? e : "soul_gemma4v2_final.cnb";
    }
    if (!residual_path || !residual_path[0]) {
        e = getenv("CNET_RESIDUAL_GGUF");
        residual_path = e && e[0] ? e : "";
    }
    if (!teacher_path || !teacher_path[0]) {
        e = getenv("CNET_PERSONAL_TEACHER");
        if (!e || !e[0]) e = getenv("CNET_MODEL_PATH");
        teacher_path = e && e[0] ? e : "";
    }
    if (!window_path || !window_path[0]) {
        e = getenv("CNET_RESIDUAL_WINDOW");
        if (!e || !e[0]) e = getenv("CNET_WINDOW_FILE");
        window_path = e && e[0] ? e : "english_window_256.txt";
    }

    copy_path(out->cnb_path, sizeof out->cnb_path, cnb_path);
    copy_path(out->residual_path, sizeof out->residual_path, residual_path);
    copy_path(out->teacher_path, sizeof out->teacher_path, teacher_path);
    copy_path(out->window_path, sizeof out->window_path, window_path);

    (void)cnet_mem_available(&out->mem_available_bytes, &out->mem_total_bytes);
    out->cnb_bytes = cnet_file_size(out->cnb_path);
    out->cnb_path_ok = out->cnb_bytes > 0;
    out->residual_gguf_bytes = cnet_file_size(out->residual_path);
    out->residual_path_ok = out->residual_gguf_bytes > 0 || !out->residual_path[0];
    if (out->residual_path[0] && out->residual_gguf_bytes == 0)
        out->residual_path_ok = 0;
    out->teacher_gguf_bytes = cnet_file_size(out->teacher_path);
    out->teacher_path_ok = out->teacher_gguf_bytes > 0 || !out->teacher_path[0];
    if (out->teacher_path[0] && out->teacher_gguf_bytes == 0)
        out->teacher_path_ok = 0;
    out->window_ids = count_window_ids(out->window_path);
    out->window_path_ok = out->window_ids > 0 || !out->window_path[0];

    out->peak_residual_alone = peak_for_gguf(out->residual_gguf_bytes);
    out->peak_teacher_alone = peak_for_gguf(out->teacher_gguf_bytes);
    /* Dual: both models + CNB working set (~CNB + 256 MiB). */
    out->peak_dual = out->peak_residual_alone + out->peak_teacher_alone +
                     out->cnb_bytes + (256ull << 20);

    budget = (out->mem_available_bytes * (uint64_t)safety_pct) / 100ull;
    out->residual_alone_safe =
        out->peak_residual_alone == 0 || out->peak_residual_alone <= budget;
    out->teacher_alone_safe =
        out->peak_teacher_alone == 0 || out->peak_teacher_alone <= budget;
    out->dual_safe = out->peak_dual <= budget ||
                     (out->peak_residual_alone == 0 || out->peak_teacher_alone == 0);
    if (out->peak_residual_alone > 0 && out->peak_teacher_alone > 0)
        out->dual_safe = out->peak_dual <= budget;
    out->prefer_warm_only = !out->dual_safe && out->residual_alone_safe;

    if (!out->cnb_path_ok)
        snprintf(out->advice, sizeof out->advice,
                 "missing CNB base — set CNET_BASE_PATH");
    else if (out->residual_path[0] && !out->residual_path_ok)
        snprintf(out->advice, sizeof out->advice,
                 "CNET_RESIDUAL_GGUF missing on disk");
    else if (!out->dual_safe && out->peak_residual_alone > 0 &&
             out->peak_teacher_alone > 0)
        snprintf(out->advice, sizeof out->advice,
                 "dual residual+teacher exceeds %d%% MemAvailable — prefer "
                 "lazy residual or single-model layout",
                 safety_pct);
    else if (out->dual_safe)
        snprintf(out->advice, sizeof out->advice,
                 "dual residual+teacher within %d%% MemAvailable", safety_pct);
    else
        snprintf(out->advice, sizeof out->advice, "layout ok (single model path)");
    return 0;
}

int cnet_placement_doctor(const CnetPlacementPlan *plan, char *report,
                          size_t report_cap) {
    int rc = 0;
    size_t n = 0;
    if (!plan || !report || report_cap < 8) return 2;
    report[0] = '\0';
#define APP(...)                                                               \
    do {                                                                       \
        int w = snprintf(report + n, report_cap - n, __VA_ARGS__);             \
        if (w > 0) n += (size_t)w;                                             \
        if (n >= report_cap) n = report_cap - 1;                               \
    } while (0)
    APP("cnet doctor\n");
    APP("  mem_available=%.2f GiB  mem_total=%.2f GiB\n",
        plan->mem_available_bytes / (1024.0 * 1024.0 * 1024.0),
        plan->mem_total_bytes / (1024.0 * 1024.0 * 1024.0));
    APP("  cnb=%s %s (%.1f MiB)\n", plan->cnb_path,
        plan->cnb_path_ok ? "ok" : "MISSING",
        plan->cnb_bytes / (1024.0 * 1024.0));
    APP("  residual=%s %s (%.1f GiB) peak≈%.1f GiB %s\n",
        plan->residual_path[0] ? plan->residual_path : "(unset)",
        plan->residual_path_ok ? "ok" : "MISSING",
        plan->residual_gguf_bytes / (1024.0 * 1024.0 * 1024.0),
        plan->peak_residual_alone / (1024.0 * 1024.0 * 1024.0),
        plan->residual_alone_safe ? "safe" : "OVER");
    APP("  teacher=%s %s (%.1f GiB) peak≈%.1f GiB %s\n",
        plan->teacher_path[0] ? plan->teacher_path : "(unset)",
        plan->teacher_path_ok ? "ok" : "MISSING",
        plan->teacher_gguf_bytes / (1024.0 * 1024.0 * 1024.0),
        plan->peak_teacher_alone / (1024.0 * 1024.0 * 1024.0),
        plan->teacher_alone_safe ? "safe" : "OVER");
    APP("  dual_peak≈%.1f GiB dual=%s prefer_warm_only=%d\n",
        plan->peak_dual / (1024.0 * 1024.0 * 1024.0),
        plan->dual_safe ? "safe" : "UNSAFE", plan->prefer_warm_only);
    APP("  window=%s ids=%llu %s\n", plan->window_path,
        (unsigned long long)plan->window_ids,
        plan->window_path_ok ? "ok" : "missing");
    APP("  advice: %s\n", plan->advice);
    if (!plan->cnb_path_ok) rc = 1;
    if (plan->residual_path[0] && !plan->residual_path_ok) rc = 1;
    if (!plan->dual_safe && plan->peak_residual_alone > 0 &&
        plan->peak_teacher_alone > 0)
        rc = 1; /* warn as non-zero for automation */
    APP("  result=%s\n", rc == 0 ? "OK" : "NEEDS_ATTENTION");
#undef APP
    return rc;
}

int cnet_placement_json(const CnetPlacementPlan *plan, char *out,
                        size_t out_cap) {
    if (!plan || !out || out_cap < 32) return -1;
    snprintf(out, out_cap,
             "{\"mem_available\":%llu,\"mem_total\":%llu,\"cnb_bytes\":%llu,"
             "\"residual_bytes\":%llu,\"teacher_bytes\":%llu,"
             "\"peak_residual\":%llu,\"peak_teacher\":%llu,\"peak_dual\":%llu,"
             "\"dual_safe\":%d,\"residual_alone_safe\":%d,"
             "\"teacher_alone_safe\":%d,\"prefer_warm_only\":%d,"
             "\"window_ids\":%llu,\"advice\":\"%s\"}",
             (unsigned long long)plan->mem_available_bytes,
             (unsigned long long)plan->mem_total_bytes,
             (unsigned long long)plan->cnb_bytes,
             (unsigned long long)plan->residual_gguf_bytes,
             (unsigned long long)plan->teacher_gguf_bytes,
             (unsigned long long)plan->peak_residual_alone,
             (unsigned long long)plan->peak_teacher_alone,
             (unsigned long long)plan->peak_dual, plan->dual_safe,
             plan->residual_alone_safe, plan->teacher_alone_safe,
             plan->prefer_warm_only, (unsigned long long)plan->window_ids,
             plan->advice);
    return 0;
}
