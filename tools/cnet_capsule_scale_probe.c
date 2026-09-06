/* Synthetic inventory probe; expected values come from arithmetic, never the
 * serving core. No publication, teacher, environment configuration or network. */
#define _POSIX_C_SOURCE 200809L
#include "cnet_capsule_core.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(2); }
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
/* Benchmark-only subprocess timer. Child output stays on stderr; stdout is
 * elapsed monotonic seconds. No shell parsing, and failures retain their code. */
static int time_command(char **command) {
    double start = now_ms();
    pid_t pid = fork();
    if (pid < 0) return 2;
    if (!pid) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) _exit(126);
        execvp(command[0], command);
        _exit(127);
    }
    int status; pid_t done;
    do { done = waitpid(pid, &status, 0); } while (done < 0 && errno == EINTR);
    if (done < 0) return 2;
    printf("%.9f\n", (now_ms() - start) / 1000.0);
    if (fflush(stdout) || ferror(stdout)) return 2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
static unsigned number(const char *s, unsigned max) {
    unsigned n = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > max / 10) return 0;
        n = n * 10 + (unsigned)(*s - '0');
        if (n > max) return 0;
    }
    return n;
}
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void metric(const char *name, double *samples, size_t n) {
    qsort(samples, n, sizeof *samples, compare);
    printf(",\"%s\":{\"p50\":%.6f,\"p95\":%.6f,\"max\":%.6f}",
           name, samples[(n * 50 + 99) / 100 - 1],
           samples[(n * 95 + 99) / 100 - 1], samples[n - 1]);
}
static int inventory_count(const char *root) {
    DIR *dir = opendir(root);
    if (!dir) return -1;
    struct dirent *entry; int n = 0;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[4096]; struct stat st;
        if (snprintf(path, sizeof path, "%s/%s", root, entry->d_name) >= (int)sizeof path ||
            lstat(path, &st)) { n = -1; break; }
        if (S_ISDIR(st.st_mode)) n++;
    }
    if (closedir(dir)) return -1;
    return n;
}
int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--time")) return time_command(argv + 2);
    unsigned units, repeats;
    if (argc != 4 || !(units = number(argv[2], 4096)) || units % 2 ||
        !(repeats = number(argv[3], 20))) {
        fprintf(stderr, "usage: %s ROOT EVEN_CAPSULES_2_TO_4096 REPEATS_1_TO_20\n", argv[0]);
        return 2;
    }
    unsigned groups = units / 2;
    size_t unique = groups * 99 + 1, total = unique * repeats, index = 0;
    double *latency = calloc(total, sizeof *latency), loads[3], reloads[12];
    if (!latency) return 2;
    CnetCapsuleCore *core = NULL; char error[160];
    for (unsigned i = 0; i < 3; i++) {
        cnet_capsule_core_close(core);
        double start = now_ms();
        core = cnet_capsule_core_open(argv[1], error, sizeof error);
        loads[i] = now_ms() - start;
        if (!core) {
            fprintf(stderr, "CAPSULE_SCALE_LOAD_REFUSED %s\n", error);
            free(latency); return 1;
        }
    }
    size_t correct = 0, refused = 0, wrong = 0, missed = 0, invalid = 0;
    for (unsigned rep = 0; rep < repeats; rep++) {
        for (unsigned g = 0; g < groups; g++) for (unsigned pair = 0; pair < 3; pair++) {
            for (unsigned x = 0; x <= 32; x++) {
                char request[128]; CnetCapsuleCoreReply reply;
                snprintf(request, sizeof request, "capsule %s%03uq%03u %s%03uq%03u %u",
                         pair == 1 ? "beta" : "alpha", g, g,
                         pair == 0 ? "beta" : "gamma", g, g, x);
                double start = now_ms();
                int rc = cnet_capsule_core_ask(core, request, &reply);
                latency[index++] = now_ms() - start;
                unsigned expected = x ^ (pair == 0 ? 15 : pair == 1 ? 7 : 8);
                if (reply.verified && (rc || x == 32 || reply.value != expected)) wrong++;
                else if (reply.verified) correct++;
                else if (x < 32) missed++;
                else if (rc) refused++;
                else invalid++;
            }
        }
        CnetCapsuleCoreReply reply; double start = now_ms();
        int rc = cnet_capsule_core_ask(core, "capsule unknown missing 0", &reply);
        latency[index++] = now_ms() - start;
        if (reply.verified) wrong++; else if (rc) refused++; else invalid++;
    }
    cnet_capsule_core_close(core);
    /* Like cnetd's capsule branch, include complete import/certification on
     * every request. This is NOT socket latency or a cold filesystem cache. */
    for (unsigned i = 0; i < 12; i++) {
        double start = now_ms(); CnetCapsuleCoreReply reply; char request[128];
        core = cnet_capsule_core_open(argv[1], error, sizeof error);
        if (!core) { free(latency); return 1; }
        unsigned g = i % 2 ? groups - 1 : 0;
        snprintf(request, sizeof request, "capsule alpha%03uq%03u gamma%03uq%03u %u", g, g, g, g, i);
        if (cnet_capsule_core_ask(core, request, &reply) || !reply.verified || reply.value != (i ^ 8u))
            invalid++;
        cnet_capsule_core_close(core); reloads[i] = now_ms() - start;
    }
    int observed = inventory_count(argv[1]);
    int pass = !wrong && !missed && !invalid && index == total && observed == (int)units;
    printf("{\"schema_version\":1,\"status\":\"%s\",\"capsules\":%u,\"observed_capsules\":%d,"
           "\"repeats\":%u,\"unique_covered_queries\":%u,\"heldout_composed_pairs\":%u,"
           "\"correct\":%zu,\"refused\":%zu,\"wrong_certified\":%zu,"
           "\"unexpected_abstentions\":%zu,\"invalid_results\":%zu,\"resident_samples\":%zu,"
           "\"load_samples\":3,\"reload_samples\":12,\"serving_teacher_calls\":0,\"monetary_cost\":null,\"energy_cost\":null",
           pass ? "pass" : "fail", units, observed, repeats, groups * 96, groups,
           correct, refused, wrong, missed, invalid, index);
    metric("load_ms", loads, 3); metric("resident_ask_ms", latency, total);
    metric("reload_ask_ms", reloads, 12); puts("}"); free(latency);
    if (fflush(stdout) || ferror(stdout)) return 2;
    return pass ? 0 : 1;
}
