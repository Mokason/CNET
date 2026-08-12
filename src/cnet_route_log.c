/*
 * cnet_route_log.c — JSONL telemetry for real AICIMO route decisions.
 */
#include "../include/cnet_route_log.h"
#include "../include/cnet_json_escape.h"
#include "../include/cnet_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

double cnet_route_log_cost(uint32_t prompt_tokens, uint32_t generated_tokens) {
    return 1.0 + (double)prompt_tokens + (double)generated_tokens;
}

const char *cnet_route_log_profile_name(int sampling_mode) {
    switch (sampling_mode) {
        case CNET_HARNESS_SAMPLING_AUTO:          return "auto";
        case CNET_HARNESS_SAMPLING_DETERMINISTIC: return "deterministic";
        case CNET_HARNESS_SAMPLING_FOCUSED:       return "focused";
        case CNET_HARNESS_SAMPLING_BALANCED:      return "balanced";
        case CNET_HARNESS_SAMPLING_EXPLORATORY:   return "exploratory";
        default: return "unknown";
    }
}

const char *cnet_route_log_outcome_name(int status) {
    switch (status) {
        case CNET_HARNESS_OK:             return "ok";
        case CNET_HARNESS_ERR_INVALID:    return "err_invalid";
        case CNET_HARNESS_ERR_MODEL_LOAD: return "err_model_load";
        case CNET_HARNESS_ERR_BACKEND:    return "err_backend";
        case CNET_HARNESS_ERR_STATE:      return "err_state";
        case CNET_HARNESS_ERR_INTERNAL:   return "err_internal";
        default: return "err_unknown";
    }
}

const char *cnet_route_log_path_from_env(void) {
    const char *p = getenv("CNET_ROUTE_LOG");
    if (!p || !p[0]) return NULL;
    return p;
}


int cnet_route_log_format_json(const CnetRouteLogEvent *ev, char *buf, size_t cap) {
    char mech[64], role[128], role_c[128], prof[32], outc[32], model[128];
    char event[32], unit[128];
    float entropy;
    if (!ev || !buf || cap < 32) return -1;

    if (cnet_json_escape(ev->mechanism ? ev->mechanism : "", mech, sizeof mech) != 0)
        return -1;
    if (cnet_json_escape(ev->role ? ev->role : "", role, sizeof role) != 0)
        return -1;
    if (cnet_json_escape(ev->role_canonical ? ev->role_canonical : "",
                    role_c, sizeof role_c) != 0)
        return -1;
    if (cnet_json_escape(ev->expert_profile ? ev->expert_profile : "",
                    prof, sizeof prof) != 0)
        return -1;
    if (cnet_json_escape(ev->outcome ? ev->outcome : "", outc, sizeof outc) != 0)
        return -1;
    if (cnet_json_escape(ev->model_id ? ev->model_id : "", model, sizeof model) != 0)
        return -1;
    if (cnet_json_escape(ev->event ? ev->event : "route", event, sizeof event) != 0)
        return -1;
    if (cnet_json_escape(ev->selected_unit ? ev->selected_unit : "",
                    unit, sizeof unit) != 0)
        return -1;

    entropy = ev->entropy;
    if (entropy < 0.0f) entropy = 0.0f;
    if (entropy > 1.0f) entropy = 1.0f;

    /* Fixed field order for simple hermetic parsers / greps. */
    {
        int n = snprintf(buf, cap,
                 "{\"event\":\"%s\",\"mechanism\":\"%s\",\"role\":\"%s\","
                 "\"role_canonical\":\"%s\",\"selected_expert\":%u,"
                 "\"selected_unit\":\"%s\",\"plan_length\":%u,"
                 "\"expert_profile\":\"%s\",\"entropy\":%.6f,"
                 "\"outcome\":\"%s\",\"outcome_code\":%d,"
                 "\"route_latency_ms\":%.3f,\"total_latency_ms\":%.3f,"
                 "\"cost\":%.3f,\"prompt_tokens\":%u,\"generated_tokens\":%u,"
                 "\"aicimo_override\":%d,\"model_id\":\"%s\"}",
                 event, mech, role, role_c,
                 (unsigned)ev->selected_expert, unit,
                 (unsigned)ev->plan_length, prof, (double)entropy,
                 outc, ev->outcome_code,
                 ev->route_latency_ms, ev->total_latency_ms,
                 ev->cost,
                 (unsigned)ev->prompt_tokens, (unsigned)ev->generated_tokens,
                 ev->aicimo_override ? 1 : 0, model);
        if (n < 0 || (size_t)n >= cap) return -1;
    }
    return 0;
}

int cnet_route_log_append(const char *path, const CnetRouteLogEvent *ev) {
    char line[1024];
    char with_ts[1100];
    FILE *f;
    time_t now;
    if (!path || !path[0] || !ev) return -1;
    if (cnet_route_log_format_json(ev, line, sizeof line) != 0) return -1;
    now = time(NULL);
    /* Prefix ts without re-parsing the body. */
    if (snprintf(with_ts, sizeof with_ts, "{\"ts\":%lld,%s",
                 (long long)now, line + 1) < 0) {
        return -1;
    }
    f = fopen(path, "a");
    if (!f) return -1;
    if (fprintf(f, "%s\n", with_ts) < 0) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) return -1;
    return 0;
}
