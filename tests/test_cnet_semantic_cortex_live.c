/* Live residual HTTP → semantic cortex → shared workspace.
 * Hermetic skip unless CNET_RESIDUAL_HTTP is set.
 * make semantic_cortex_live with CNET_REQUIRE_REAL_RESIDUAL_HTTP=1 fails closed
 * when the residual server is missing.
 */
#include "cnet_semantic_cortex.h"
#include "residual_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    const char *url = getenv("CNET_RESIDUAL_HTTP");
    const char *window = getenv("CNET_RESIDUAL_WINDOW");
    int require = getenv("CNET_REQUIRE_REAL_RESIDUAL_HTTP") &&
                  getenv("CNET_REQUIRE_REAL_RESIDUAL_HTTP")[0] == '1';
    ResidualHttp *residual = NULL;
    CnetSharedWorkspace workspace;
    CnetSemanticCortex cortex;
    CnetWorkspaceEntry entries[8];
    size_t proposals = 0;
    int open_rc;

    printf("== semantic cortex live residual HTTP ==\n");

    if (!url || !url[0]) {
        check(!require, "live residual optional when URL unset");
        if (require) {
            printf("SEMANTIC_CORTEX_LIVE_FAIL reason=url_unset\n");
            return 1;
        }
        printf("SEMANTIC_CORTEX_LIVE_SKIP reason=url_unset\n");
        printf("SEMANTIC_CORTEX_LIVE_PASS status=skipped checks=%d\n", checks);
        return 0;
    }

    if (!window || !window[0])
        window = "english_window_256_bonsai.txt";

    open_rc = residual_http_open(&residual, url, window, 0);
    check(open_rc == 0, "residual_http_open against live server");
    if (open_rc != 0) {
        if (require) {
            printf("SEMANTIC_CORTEX_LIVE_FAIL reason=open_failed\n");
            return 1;
        }
        printf("SEMANTIC_CORTEX_LIVE_SKIP reason=open_failed\n");
        printf("SEMANTIC_CORTEX_LIVE_PASS status=skipped checks=%d\n", checks);
        return 0;
    }

    check(residual_http_ping(residual) == 0, "live residual ping");
    check(residual_http_window_n(residual) > 0, "live residual window non-empty");
    check(cnet_workspace_init(&workspace, 16) == 0, "workspace initializes");
    check(cnet_semantic_cortex_init_residual_http(
              &cortex,
              residual_http_oracle_topk,
              residual,
              residual_http_window_ids(residual),
              (size_t)residual_http_window_n(residual),
              "semantic_cortex/residual_http_live") == 0,
          "cortex binds live residual top-k");
    check(cnet_semantic_cortex_propose(
              &cortex, "water freezes near zero celsius", 3, 1000,
              &workspace, &proposals) == 0,
          "live residual proposes into workspace");
    check(proposals >= 1, "at least one live residual proposal");
    check(cnet_workspace_recent(&workspace, entries, 8) >= 1,
          "proposals landed in shared workspace");
    check(entries[0].trust == CNET_WORKSPACE_UNCERTIFIED,
          "live residual proposals remain uncertified");
    check(strstr(entries[0].text_or_latent_ref, "residual-token:") != NULL,
          "proposal carries residual token ref");

    residual_http_close(residual);

    if (failures) {
        printf("SEMANTIC_CORTEX_LIVE_FAIL failures=%d checks=%d\n",
               failures, checks);
        return 1;
    }
    printf("SEMANTIC_CORTEX_LIVE_PASS status=measured proposals=%zu checks=%d\n",
           proposals, checks);
    return 0;
}
