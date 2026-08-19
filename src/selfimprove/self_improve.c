#include "../../include/self_improve.h"
#include "../../include/specialist.h"
#include "../../include/contract/contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int env_is_set(const char *name) {
    const char *v = getenv(name);
    return v && v[0];
}

static int set_if_unset(const char *name, const char *value) {
    if (env_is_set(name)) return 0;
#if defined(_WIN32)
    {
        char buf[256];
        snprintf(buf, sizeof buf, "%s=%s", name, value);
        return _putenv(buf) == 0 ? 1 : 0;
    }
#else
    return setenv(name, value, 0) == 0 ? 1 : 0;
#endif
}

int self_improve_apply_deploy_env(void) {
    int n = 0;
    n += set_if_unset("CNET_ORACLE_INT8", "1");
    n += set_if_unset("CNET_TRAIN_FAST", "1");
    n += set_if_unset("CNET_ACQ_STAGES", "40");
    n += set_if_unset("CNET_ACQ_ADAPTIVE", "1");
    n += set_if_unset("CNET_LANE_PROBE_BATCH", "32");
    n += set_if_unset("CNET_GGUF_MMAP", "1");
    n += set_if_unset("CNET_HEALTH_TICK_SECONDS", "300");
    n += set_if_unset("CNET_LANE_MAX_CLOSURES", "4");
    n += set_if_unset("CNET_TEACHER_IDLE_SEC", "300");
    return n;
}

static int btn_evidence_clear(const BinaryTransformNetwork *p,
                              double threshold, size_t min_evidence) {
    unsigned long ev;
    if (!p) return 0;
    ev = (unsigned long)p->output_successes + (unsigned long)p->output_failures;
    return btn_reliability(p) >= threshold && ev >= min_evidence;
}

int self_improve_distill_route(
    PrimitiveRegistry *reg,
    const RoutePlan *plan,
    const ConsolidateConfig *cfg,
    const LibraryGateConfig *gate,
    CnetResourceGovernor *gov,
    BinaryTransformNetwork **out_chunk,
    SelfImproveReport *report)
{
    ConsolidateConfig local_cfg;
    LibraryGateConfig local_gate;
    BinaryTransformNetwork *student = NULL;
    Contract c;
    Specialist s;
    char name[CONTRACT_NAME_MAX];
    size_t i;
    size_t max_samples;

    if (!reg || !plan) return -1;
    if (plan->length < 2) {
        if (report) report->refused++;
        return 1;
    }

    if (!cfg) {
        consolidate_config_defaults(&local_cfg);
        cfg = &local_cfg;
    }
    if (!gate) {
        library_gate_config_defaults(&local_gate);
        local_gate.enabled = 0; /* off by default for hermetic distills */
        gate = &local_gate;
    }
    max_samples = cfg->max_samples ? cfg->max_samples : 4096;

    if (gate->enabled) {
        for (i = 0; i < plan->length; i++) {
            if (!btn_evidence_clear(plan->steps[i], gate->evidence_threshold,
                                    gate->min_evidence)) {
                if (report) report->refused++;
                return 1;
            }
        }
    }

    if (gov) {
        /* rate-limit mints with the same counter as drain closes */
        if (gov->policy.max_closures_per_drain &&
            gov->usage.closures_this_drain >= gov->policy.max_closures_per_drain) {
            if (report) report->skipped_budget++;
            return 1;
        }
        if (!cnet_gov_note_close(gov)) {
            if (report) report->skipped_budget++;
            return 1;
        }
    }

    student = (BinaryTransformNetwork *)calloc(1, sizeof *student);
    if (!student) return -2;
    if (consolidate_route(plan, cfg, student, NULL) != 0) {
        free(student);
        if (report) report->refused++;
        return 1;
    }

    snprintf(name, sizeof name, "si_chunk_%zu", reg->count);
    memset(&c, 0, sizeof c);
    if (contract_from_route(plan, name, max_samples, &c) != 0) {
        btn_free(student);
        free(student);
        if (report) report->refused++;
        return 1;
    }

    memset(&s, 0, sizeof s);
    /* registry_add stores the name POINTER (src/router/registry.c), so handing
       it this function's stack buffer leaves the admitted entry with a dangling
       name â€” undefined behaviour for find_named, cnb_has_unit and every
       diagnostic that reads it. The registry does not own names, so the copy is
       deliberately never freed; one small allocation per distilled chunk. */
    {
        size_t nlen = strlen(name) + 1;
        char *stable = (char *)malloc(nlen);
        if (!stable) {
            contract_free(&c);
            btn_free(student);
            free(student);
            if (report) report->refused++;
            return -3;
        }
        memcpy(stable, name, nlen);
        if (specialist_wrap_btn(&s, student, stable) != 0 ||
            specialist_admit(reg, &s, &c) != 0) {
            free(stable);
            contract_free(&c);
            btn_free(student);
            free(student);
            if (report) report->refused++;
            return -3;
        }
    }
    contract_free(&c);

    if (out_chunk) *out_chunk = student;
    if (report) report->distilled++;
    return 0;
}

int self_improve_propose_recipe(
    const char *path,
    const char *kind,
    const char *evidence,
    int priority,
    SelfImproveReport *report)
{
    FILE *f;
    time_t now;
    static const char *banned[] = {
        "lower_margin", "lower_wilson", "skip_certify", "force_admit",
        "disable_cert", NULL
    };
    size_t i;
    if (!path || !kind || !kind[0]) return -1;
    for (i = 0; banned[i]; i++) {
        if (strcmp(kind, banned[i]) == 0) return -2;
    }
    if (priority < 0) priority = 0;
    f = fopen(path, "a");
    if (!f) return -3;
    now = time(NULL);
    fprintf(f,
            "{\"id\":\"cnet-recipe-%s\",\"kind\":\"%s\",\"priority\":%d,"
            "\"status\":\"proposed\",\"meta\":{\"actionable\":true,"
            "\"never_lower_cert_bars\":true},\"evidence\":%s,\"ts\":%ld}\n",
            kind, kind, priority,
            evidence && evidence[0] ? evidence : "\"\"",
            (long)now);
    fclose(f);
    if (report) report->proposals_written++;
    return 0;
}
