/* CLI: pattern runtime
 *
 *   bin/cnet_pattern status
 *   bin/cnet_pattern bootstrap
 *   bin/cnet_pattern save
 *   bin/cnet_pattern callout "query" [role]
 *   bin/cnet_pattern promote
 *   bin/cnet_pattern import-units [max_n]
 *   bin/cnet_pattern propose <kind> <text...>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_pattern.h"

static void print_status(const CnetPatternRuntime *rt) {
    size_t i;
    printf("edges=%zu fluid=%zu improving=%zu frozen=%zu demoted=%zu hot=%d/%d clock=%u\n",
           cnet_pattern_count(rt),
           cnet_pattern_count_state(rt, CNET_PAT_STATE_FLUID),
           cnet_pattern_count_state(rt, CNET_PAT_STATE_IMPROVING),
           cnet_pattern_count_state(rt, CNET_PAT_STATE_FROZEN),
           cnet_pattern_count_state(rt, CNET_PAT_STATE_DEMOTED), rt->hot_count,
           rt->hot_cap, rt->clock);
    printf("store=%s base=%s soul=%s\n", rt->store_path,
           rt->base_path[0] ? rt->base_path : "(none)",
           rt->soul_host ? "bound" : "unbound");
    for (i = 0; i < rt->edge_count && i < 32; i++) {
        const CnetPatternEdge *e = &rt->edges[i];
        printf("  [%zu] %s state=%s body=%s ref=%s rel=%.2f succ=%u fail=%u res=%d\n",
               i, e->name, cnet_pattern_state_name(e->state),
               cnet_pattern_body_name(e->body_kind), e->body_ref, e->reliability,
               e->successes, e->failures, e->resident);
    }
    if (rt->edge_count > 32) printf("  … %zu more\n", rt->edge_count - 32);
}

int main(int argc, char **argv) {
    CnetPatternRuntime rt;
    const char *cmd = argc > 1 ? argv[1] : "status";

    cnet_pattern_runtime_from_env(&rt);
    cnet_pattern_runtime_load(&rt, rt.store_path);
    if (cnet_pattern_count(&rt) == 0)
        cnet_pattern_bootstrap_defaults(&rt);

    if (strcmp(cmd, "status") == 0) {
        print_status(&rt);
        printf("CNET_PATTERN_OK\n");
        return 0;
    }
    if (strcmp(cmd, "bootstrap") == 0) {
        int n = cnet_pattern_bootstrap_defaults(&rt);
        cnet_pattern_runtime_save(&rt, rt.store_path);
        printf("bootstrap n=%d\n", n);
        print_status(&rt);
        printf("CNET_PATTERN_OK\n");
        return 0;
    }
    if (strcmp(cmd, "save") == 0) {
        if (cnet_pattern_runtime_save(&rt, rt.store_path) != 0) {
            fprintf(stderr, "save failed\n");
            return 1;
        }
        printf("saved %s edges=%zu\n", rt.store_path, rt.edge_count);
        printf("CNET_PATTERN_OK\n");
        return 0;
    }
    if (strcmp(cmd, "promote") == 0) {
        int n = cnet_pattern_promote_all(&rt);
        cnet_pattern_runtime_save(&rt, rt.store_path);
        printf("promoted=%d frozen_now=%zu\n", n,
               cnet_pattern_count_state(&rt, CNET_PAT_STATE_FROZEN));
        print_status(&rt);
        printf("CNET_PATTERN_PROMOTE_OK n=%d\n", n);
        return 0;
    }
    if (strcmp(cmd, "import-units") == 0) {
        int max_n = argc > 2 ? atoi(argv[2]) : 64;
        int n;
        if (!rt.base_path[0]) {
            fprintf(stderr, "import-units: set CNET_BASE_PATH to a .cnb\n");
            return 2;
        }
        n = cnet_pattern_import_units(&rt, max_n);
        if (n < 0) {
            fprintf(stderr,
                    "import-units failed rc=%d (need readable CNET_BASE_PATH .cnb and "
                    "cnet.so via CNET_SO_PATH or ./cnet.so)\n",
                    n);
            return 1;
        }
        cnet_pattern_runtime_save(&rt, rt.store_path);
        printf("imported_units=%d edges=%zu\n", n, rt.edge_count);
        print_status(&rt);
        printf("CNET_PATTERN_IMPORT_OK n=%d\n", n);
        cnet_pattern_unbind_soul(&rt);
        return 0;
    }
    if (strcmp(cmd, "propose") == 0) {
        const char *kind = argc > 2 ? argv[2] : "curriculum";
        char text[512];
        int i, idx;
        text[0] = '\0';
        for (i = 3; i < argc; i++) {
            if (text[0]) strncat(text, " ", sizeof text - strlen(text) - 1);
            strncat(text, argv[i], sizeof text - strlen(text) - 1);
        }
        if (!text[0] && argc > 2) {
            /* propose kind is text */
            snprintf(text, sizeof text, "%s", kind);
            kind = "curriculum";
        }
        if (!text[0]) {
            fprintf(stderr, "usage: %s propose <kind> <text...>\n", argv[0]);
            return 2;
        }
        idx = cnet_pattern_propose_curriculum(&rt, kind, text);
        if (idx < 0) {
            fprintf(stderr, "propose failed\n");
            return 1;
        }
        cnet_pattern_runtime_save(&rt, rt.store_path);
        printf("proposed idx=%d name=%s kind=%s\n", idx, rt.edges[idx].name, kind);
        printf("CNET_PATTERN_PROPOSE_OK idx=%d\n", idx);
        return 0;
    }
    if (strcmp(cmd, "callout") == 0) {
        CnetPatternCalloutReport rep;
        const char *q = argc > 2 ? argv[2] : NULL;
        const char *role = argc > 3 ? argv[3] : "query";
        if (!q) {
            fprintf(stderr, "usage: %s callout \"query\" [role]\n", argv[0]);
            return 2;
        }
        if (cnet_pattern_callout(&rt, q, role, &rep) != 0) {
            fprintf(stderr, "callout failed: %s\n", rep.detail);
            cnet_pattern_runtime_save(&rt, rt.store_path);
            return 1;
        }
        printf("state=%s body=%s ref=%s loaded=%d froze=%d rel=%.3f\n",
               cnet_pattern_state_name(rep.state), cnet_pattern_body_name(rep.body_kind),
               rep.body_ref, rep.loaded, rep.froze, rep.reliability);
        printf("answer=%s\n", rep.answer);
        printf("detail=%s\n", rep.detail);
        cnet_pattern_runtime_save(&rt, rt.store_path);
        printf("CNET_PATTERN_OK\n");
        return 0;
    }

    fprintf(stderr,
            "usage: %s status|bootstrap|save|promote|import-units [N]|propose <kind> "
            "<text>|callout \"query\" [role]\n",
            argv[0]);
    return 2;
}
