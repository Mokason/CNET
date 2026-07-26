/* own-learning health check — refuse dangerous unattended configurations.
 *
 * "Conditionally safe" is only worth anything if the conditions are checked by
 * something other than a human reading four bullet points. This inspects the
 * deployed profile and the on-disk state and exits non-zero when the
 * combination would let mined units answer outside what they were certified
 * for.
 *
 * Usage: cnet_own_learning_health [--env-file config/personal-ai.env]
 *                                 [--base <path.cnb>]
 * Prints one JSON object. Exit 0 = safe, 1 = dangerous, 2 = usage/IO error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/base.h"
#include "../include/hybrid_ai.h"

#define MAX_DANGER 8

/* A tiny KEY=VALUE reader for the deployed EnvironmentFile: last assignment
   wins and commented lines are ignored, matching systemd's own reading, so a
   knob re-set further down the file is what we report. */
static int env_file_get(const char *path, const char *key, char *out,
                        size_t cap) {
    FILE *fp = fopen(path, "r");
    char line[1024];
    size_t klen = strlen(key);
    int found = 0;
    if (!fp) return -1;
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        if (strncmp(p, key, klen) != 0 || p[klen] != '=') continue;
        {
            char *v = p + klen + 1, *e;
            e = v + strlen(v);
            while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
                e--;
            *e = '\0';
            snprintf(out, cap, "%s", v);
            found = 1;
        }
    }
    fclose(fp);
    return found ? 0 : 1;
}

/* Process env wins over the file, since that is what the service actually
   runs with when an operator overrides a knob. */
static void knob(const char *envfile, const char *key, char *out, size_t cap) {
    const char *e = getenv(key);
    out[0] = '\0';
    if (e && e[0]) {
        snprintf(out, cap, "%s", e);
        return;
    }
    if (envfile) (void)env_file_get(envfile, key, out, cap);
}

static int truthy_one(const char *v) { return v && v[0] == '1' && v[1] == '\0'; }
static int explicit_zero(const char *v) {
    return v && v[0] == '0' && v[1] == '\0';
}

int main(int argc, char **argv) {
    const char *envfile = NULL;
    const char *base_path = NULL;
    char mine_on_serve[64], coverage_abstain[64], res_http[256], res_gguf[256];
    char cov_path[576];
    CnetBase base;
    HybridAi h;
    const char *danger[MAX_DANGER];
    size_t ndanger = 0, i;
    size_t mined = 0, unguarded = 0, records = 0;
    const char *cov_state = "missing";
    int base_ok = 0, mine_flag, gate_off, residual_cfg;
    int a;

    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--env-file") && a + 1 < argc) envfile = argv[++a];
        else if (!strcmp(argv[a], "--base") && a + 1 < argc) base_path = argv[++a];
        else {
            fprintf(stderr, "usage: %s [--env-file F] [--base B]\n", argv[0]);
            return 2;
        }
    }
    if (!base_path) base_path = getenv("CNET_BASE_PATH");

    knob(envfile, "CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE", mine_on_serve,
         sizeof mine_on_serve);
    knob(envfile, "CNET_COVERAGE_ABSTAIN", coverage_abstain,
         sizeof coverage_abstain);
    knob(envfile, "CNET_RESIDUAL_HTTP", res_http, sizeof res_http);
    knob(envfile, "CNET_RESIDUAL_GGUF", res_gguf, sizeof res_gguf);

    mine_flag = truthy_one(mine_on_serve);
    gate_off = explicit_zero(coverage_abstain);
    residual_cfg = (res_http[0] != '\0') || (res_gguf[0] != '\0');

    hybrid_ai_init(&h);
    cnb_init(&base);
    if (base_path && base_path[0]) {
        if (cnb_load(&base, base_path) == 0) {
            base_ok = 1;
            snprintf(cov_path, sizeof cov_path, "%s.coverage", base_path);
            {
                FILE *fp = fopen(cov_path, "r");
                if (!fp) {
                    cov_state = "missing";
                } else {
                    fclose(fp);
                    cov_state = hybrid_coverage_load(&h, cov_path) < 0
                                    ? "unreadable"
                                    : "ok";
                }
            }
            records = hybrid_coverage_count(&h);
            for (i = 0; i < base.unit_count; i++) {
                const char *nm = base.units[i].name;
                if (!hybrid_unit_is_mined(nm)) continue;
                mined++;
                if (!hybrid_coverage_has_unit(&h, nm)) unguarded++;
            }
        }
    }

    /* The one-env-typo failure: mining on, guard off. */
    if (mine_flag && gate_off && ndanger < MAX_DANGER)
        danger[ndanger++] = "mine_on_serve_with_coverage_gate_off";
    /* Mining enabled with nothing to learn from — silently a no-op. */
    if (mine_flag && !residual_cfg && ndanger < MAX_DANGER)
        danger[ndanger++] = "mine_on_serve_without_residual";
    /* Sealed mined units whose guard is gone: serve refuses them (fail-closed),
       but the library is degraded and an operator must re-mine. */
    if (unguarded > 0 && ndanger < MAX_DANGER)
        danger[ndanger++] = "mined_units_without_coverage";
    if (base_ok && strcmp(cov_state, "unreadable") == 0 &&
        ndanger < MAX_DANGER)
        danger[ndanger++] = "coverage_file_unreadable";

    printf("{\"base\":\"%s\",\"base_loaded\":%d,\"mined_units\":%zu,"
           "\"coverage_records\":%zu,\"unguarded_mined\":%zu,"
           "\"coverage_file\":\"%s\",\"mine_on_serve\":%d,"
           "\"coverage_gate\":\"%s\",\"residual_configured\":%d,"
           "\"dangerous\":[",
           base_path ? base_path : "", base_ok, mined, records, unguarded,
           cov_state, mine_flag, gate_off ? "off" : "on", residual_cfg);
    for (i = 0; i < ndanger; i++)
        printf("%s\"%s\"", i ? "," : "", danger[i]);
    printf("],\"status\":\"%s\"}\n", ndanger ? "danger" : "ok");

    if (ndanger == 0) printf("OWN_LEARNING_HEALTH_PASS\n");
    else printf("OWN_LEARNING_HEALTH_FAIL count=%zu\n", ndanger);

    hybrid_ai_free(&h);
    cnb_free(&base);
    return ndanger ? 1 : 0;
}
