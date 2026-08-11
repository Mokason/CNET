/* Domain route CLI + gate
 *   ./bin/roe_domain_route "who are you"
 *   ./bin/roe_domain_route --test
 *   ./bin/roe_domain_route --file config/domain_routes.tsv "query"
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_domain_route.h"

int main(int argc, char **argv) {
    CnetDomainRouter R;
    CnetDomainDecision d;
    const char *q = NULL;
    const char *file = NULL;
    int i, do_test = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0)
            do_test = 1;
        else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
            file = argv[++i];
        else if (argv[i][0] != '-')
            q = argv[i];
    }
    if (do_test) return cnet_domain_route_selftest();

    cnet_domain_route_init(&R);
    if (file) (void)cnet_domain_route_load_file(&R, file);
    else (void)cnet_domain_route_load_file(&R, "config/domain_routes.tsv");

    if (!q) {
        fprintf(stderr, "usage: %s [--test] [--file PATH] \"query\"\n", argv[0]);
        return 2;
    }
    cnet_domain_route_resolve(&R, q, &d);
    printf("route_kind=%s\n", d.kind_name);
    printf("reason=%s\n", d.reason ? d.reason : "-");
    printf("pattern=%s\n", d.pattern[0] ? d.pattern : "-");
    printf("pack_or_skill=%s\n", d.pack_or_skill[0] ? d.pack_or_skill : "-");
    printf("mtk_path=%s\n", d.mtk_path[0] ? d.mtk_path : "-");
    printf("conf_x1000=%d\n", d.conf_x1000);
    printf("rules=%d source=%s\n", cnet_domain_route_n_rules(&R), R.source);
    /* Machine-friendly one line for front door */
    printf("DISPATCH %s\n", d.kind_name);
    return d.kind == CNET_ROUTE_ABSTAIN ? 0 : 0;
}
