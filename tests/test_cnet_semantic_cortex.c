#include "cnet_semantic_cortex.h"
#include "cnet_heldout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPABILITY_ID "hybrid_skill_serve"
#define RESIDUAL_QUERY "held out query"

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* The authority word the fixture declares, mapped onto the trust level the
   workspace actually records. "certified" must never be satisfiable here: the
   point of the capability is that a semantic backend cannot self-certify. */
static const char *authority_word(CnetWorkspaceTrust trust) {
    switch (trust) {
        case CNET_WORKSPACE_UNCERTIFIED: return "uncertified";
        case CNET_WORKSPACE_PROVISIONAL: return "provisional";
        case CNET_WORKSPACE_CERTIFIED:   return "certified";
    }
    return "unknown";
}

static int fake_topk(const double *input, double *output,
                     void *context, int k) {
    const size_t width = *(const size_t *)context;
    size_t i;
    (void)input;
    for (i = 0; i < (size_t)k * width; i++) output[i] = 0.0;
    for (i = 0; i < (size_t)k; i++) output[i * width + i + 1] = 1.0;
    return 0;
}

int main(void) {
    CnetSharedWorkspace workspace;
    CnetSemanticCortex cortex;
    CnetWorkspaceEntry entries[8];
    CnetHeldOut heldout;
    size_t proposals = 0, width = 5;
    const int ids[] = {101, 102, 103, 104, 105};
    char hermetic_query[CNET_WORKSPACE_TEXT_MAX];
    char residual_query[CNET_WORKSPACE_TEXT_MAX];
    char want_authority[32];
    int heldout_rc = cnet_heldout_open(&heldout, CAPABILITY_ID);

    if (heldout_rc < 0) {
        fprintf(stderr, "FAIL: declared held-out fixture is unusable\n");
        return 2;
    }
    /* Queries come from the fixture so the certified run is the declared run. */
    (void)cnet_heldout_str(&heldout, "hermetic-uncertified", "query",
                           hermetic_query, sizeof hermetic_query,
                           "adaptive memory consolidation");
    /* The residual arm's query is a compiled-in constant, not a declared
       field. The residual path publishes "residual-token:<id>" chosen by the
       injected top-k callback, so nothing the test can observe is a function of
       the query text -- a declared `query` there would be a field the fixture
       claims to control and does not. The hermetic arm below is different: its
       proposals are built from the query's own tokens, so that one IS bound. */
    snprintf(residual_query, sizeof residual_query, "%s", RESIDUAL_QUERY);

    check(cnet_workspace_init(&workspace, 8) == 0, "workspace initializes");
    check(cnet_semantic_cortex_init_hermetic(&cortex, NULL) == 0,
          "hermetic cortex initializes");
    check(cnet_semantic_cortex_propose(&cortex,
          hermetic_query, 3, 100, &workspace,
          &proposals) == 0, "hermetic proposals succeed");
    check(proposals == 3, "hermetic proposal count is bounded");
    check(cnet_workspace_recent(&workspace, entries, 8) == 3,
          "proposals enter shared workspace");
    check(entries[0].trust == CNET_WORKSPACE_UNCERTIFIED &&
          entries[1].trust == CNET_WORKSPACE_UNCERTIFIED,
          "semantic cortex cannot certify proposals");
    check(strstr(entries[0].source_tag, "semantic_cortex") != NULL,
          "proposal source is explicit");
    {
        const char *saw = authority_word(entries[0].trust);
        char backend[32];
        int ok;
        (void)cnet_heldout_str(&heldout, "hermetic-uncertified",
                               "expected_authority", want_authority,
                               sizeof want_authority, "uncertified");
        (void)cnet_heldout_str(&heldout, "hermetic-uncertified", "backend",
                               backend, sizeof backend, "hermetic");
        if (strcmp(backend, "hermetic") != 0) {
            fprintf(stderr, "FAIL: case hermetic-uncertified declares backend "
                            "%s, this arm exercises hermetic\n", backend);
            failures++;
        }
        /* Binding the query to a proposal DERIVED from the query proves
           nothing: both move together, so any query matches its own output.
           The fixture therefore declares the proposals it expects, and the run
           is compared against that. Mutating either the query or the expected
           proposals now breaks the match. */
        {
            char want_join[CNET_WORKSPACE_TEXT_MAX * 3];
            char got_join[CNET_WORKSPACE_TEXT_MAX * 3];
            size_t k, used = 0;
            (void)cnet_heldout_str(&heldout, "hermetic-uncertified",
                                   "expected_proposals", want_join,
                                   sizeof want_join,
                                   "semantic-candidate:consolidation|"
                                   "semantic-candidate:memory|"
                                   "semantic-candidate:adaptive");
            got_join[0] = '\0';
            for (k = 0; k < 3; k++) {
                int n = snprintf(got_join + used, sizeof got_join - used,
                                 "%s%s", k ? "|" : "",
                                 entries[k].text_or_latent_ref);
                if (n < 0 || (size_t)n >= sizeof got_join - used) break;
                used += (size_t)n;
            }
            if (strcmp(want_join, got_join) != 0) {
                fprintf(stderr, "FAIL: case hermetic-uncertified expected "
                                "proposals %s, run produced %s\n",
                        want_join, got_join);
                failures++;
            }
            ok = (strcmp(saw, want_authority) == 0) &&
                 (strcmp(backend, "hermetic") == 0) &&
                 (strcmp(want_join, got_join) == 0);
        }
        cnet_heldout_verdict(&heldout, "hermetic-uncertified", ok);
    }

    cnet_workspace_clear(&workspace);
    check(cnet_semantic_cortex_init_residual_http(
          &cortex, fake_topk, &width, ids, width, NULL) == 0,
          "residual adapter initializes");
    check(cnet_semantic_cortex_propose(&cortex, residual_query, 2,
          200, &workspace, &proposals) == 0,
          "residual-compatible callback proposes");
    check(proposals == 2, "residual proposal count is correct");
    check(cnet_workspace_recent(&workspace, entries, 8) == 2,
          "residual proposals are published");
    check(strcmp(entries[0].text_or_latent_ref,
                 "residual-token:103") == 0,
          "residual window id is preserved");
    check(entries[0].trust == CNET_WORKSPACE_UNCERTIFIED,
          "residual proposal remains uncertified");
    {
        const char *saw = authority_word(entries[0].trust);
        char backend[32];
        int ok;
        (void)cnet_heldout_str(&heldout, "residual-uncertified",
                               "expected_authority", want_authority,
                               sizeof want_authority, "uncertified");
        (void)cnet_heldout_str(&heldout, "residual-uncertified", "backend",
                               backend, sizeof backend, "residual_compatible");
        if (strcmp(backend, "residual_compatible") != 0) {
            fprintf(stderr, "FAIL: case residual-uncertified declares backend "
                            "%s, this arm exercises residual_compatible\n",
                    backend);
            failures++;
        }
        ok = (strcmp(saw, want_authority) == 0) &&
             (strcmp(backend, "residual_compatible") == 0);
        if (!ok) {
            fprintf(stderr, "FAIL: case residual-uncertified expected "
                            "authority %s, workspace recorded %s\n",
                    want_authority, saw);
            failures++;
        }
        cnet_heldout_verdict(&heldout, "residual-uncertified", ok);
    }

    check(cnet_semantic_cortex_propose(&cortex, "", 2, 0,
          &workspace, &proposals) != 0, "empty query rejected");
    check(cnet_semantic_cortex_propose(&cortex, "query", 9, 0,
          &workspace, &proposals) != 0, "oversized top-k rejected");

    if (cnet_heldout_finish(&heldout) != 0) {
        fprintf(stderr, "FAIL: declared held-out fixture was not honoured\n");
        failures++;
    }
    cnet_heldout_close(&heldout);
    if (failures) return 1;
    puts("SEMANTIC_CORTEX_PASS metric=1.000 authority=cnet");
    return 0;
}
