/* CLI + gates for query_alias, dialog_ctx, slot_extract
 *   ./bin/cnet_query_dialog --test
 *   ./bin/cnet_query_dialog "Introduce yourself"
 *   ./bin/cnet_query_dialog --dialog "show me its status"
 *   ./bin/cnet_query_dialog --slot "is cnet-web active"
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_dialog_ctx.h"
#include "../include/cnet_query_alias.h"
#include "../include/cnet_slot_extract.h"

int main(int argc, char **argv) {
    int i;
    int do_alias_test = 0, do_dialog_test = 0, do_slot_test = 0, do_all = 0;
    const char *q = NULL;
    const char *which = "alias";

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0 || strcmp(argv[i], "--test-all") == 0)
            do_all = 1;
        else if (strcmp(argv[i], "--test-alias") == 0)
            do_alias_test = 1;
        else if (strcmp(argv[i], "--test-dialog") == 0)
            do_dialog_test = 1;
        else if (strcmp(argv[i], "--test-slot") == 0)
            do_slot_test = 1;
        else if (strcmp(argv[i], "--dialog") == 0)
            which = "dialog";
        else if (strcmp(argv[i], "--slot") == 0)
            which = "slot";
        else if (argv[i][0] != '-')
            q = argv[i];
    }

    if (do_all) {
        int rc = 0;
        rc |= cnet_query_alias_selftest();
        rc |= cnet_dialog_ctx_selftest();
        rc |= cnet_slot_extract_selftest();
        if (rc) {
            printf("QUERY_DIALOG_FAIL\n");
            return 1;
        }
        printf("QUERY_DIALOG_PASS\n");
        return 0;
    }
    if (do_alias_test) return cnet_query_alias_selftest();
    if (do_dialog_test) return cnet_dialog_ctx_selftest();
    if (do_slot_test) return cnet_slot_extract_selftest();

    if (!q) {
        fprintf(stderr,
                "usage: %s [--test|--test-alias|--test-dialog|--test-slot] "
                "[--dialog|--slot] [\"query\"]\n",
                argv[0]);
        return 2;
    }

    if (strcmp(which, "dialog") == 0) {
        CnetDialogCtx C;
        CnetDialogResolveMeta m;
        char out[CNET_DC_Q];
        cnet_dialog_ctx_init(&C);
        cnet_dialog_ctx_update(&C, "cnet-marble status", "ops_cnet_marble",
                               "pack_ops_hermes_systemd", 1);
        (void)cnet_dialog_resolve(&C, q, out, sizeof out, &m);
        printf("in=%s\nout=%s\napplied=%d reason=%s entity=%s\n", q, out,
               m.applied, m.reason[0] ? m.reason : "-",
               m.entity_used[0] ? m.entity_used : "-");
        return 0;
    }

    if (strcmp(which, "slot") == 0) {
        CnetSlotMeta m;
        char out[CNET_SLOT_OUT];
        char norm[CNET_QA_OUT];
        cnet_query_normalize(q, norm, sizeof norm);
        (void)cnet_slot_extract_ops(norm, out, sizeof out, &m);
        printf("in=%s\nnorm=%s\nout=%s\napplied=%d unit=%s action=%s reason=%s\n",
               q, norm, out, m.applied, m.unit[0] ? m.unit : "-",
               m.action_name[0] ? m.action_name : "-",
               m.reason[0] ? m.reason : "-");
        return 0;
    }

    {
        CnetQueryAliasTable T;
        CnetQueryPrepareMeta m;
        CnetSlotMeta sm;
        char out[CNET_QA_OUT];
        char slotted[CNET_SLOT_OUT];
        cnet_query_alias_init(&T);
        (void)cnet_query_alias_load_file(&T, "config/query_aliases.tsv");
        cnet_query_prepare(&T, q, out, sizeof out, &m);
        if (cnet_slot_extract_ops(out, slotted, sizeof slotted, &sm) && sm.applied)
            printf("in=%s\nout=%s\nnormalized=%d alias_hit=%d alias=%s canon=%s\n"
                   "slot_hit=1 slot_out=%s unit=%s\n",
                   q, out, m.normalized, m.alias_hit,
                   m.matched_alias[0] ? m.matched_alias : "-",
                   m.canonical[0] ? m.canonical : "-", slotted,
                   sm.unit[0] ? sm.unit : "-");
        else
            printf("in=%s\nout=%s\nnormalized=%d alias_hit=%d alias=%s canon=%s\n"
                   "slot_hit=0\n",
                   q, out, m.normalized, m.alias_hit,
                   m.matched_alias[0] ? m.matched_alias : "-",
                   m.canonical[0] ? m.canonical : "-");
    }
    return 0;
}
