/* web_lookup_v1 CLI — typed fetch hop, not residual speech.
 *
 *   ./bin/cnet_lookup --bind year --url URL
 *
 * Speaks a bound scalar with provenance, or abstains. Does not invent
 * search queries. The operator supplies the URL.
 */
#include "cnet_lookup.h"

#include <stdio.h>
#include <string.h>

static int parse_bind(const char *name, CnetLookupBind *bind) {
    if (name == NULL || bind == NULL) return -1;
    if (strcmp(name, "integer") == 0) {
        *bind = CNET_LOOKUP_BIND_INTEGER;
        return 0;
    }
    if (strcmp(name, "token") == 0) {
        *bind = CNET_LOOKUP_BIND_TOKEN;
        return 0;
    }
    if (strcmp(name, "line") == 0) {
        *bind = CNET_LOOKUP_BIND_LINE;
        return 0;
    }
    if (strcmp(name, "year") == 0) {
        *bind = CNET_LOOKUP_BIND_YEAR;
        return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *url = NULL;
    const char *bind_name = "integer";
    const char *question = NULL;
    CnetLookupBind bind = CNET_LOOKUP_BIND_INTEGER;
    CnetLookupReport report;
    char spoken[512];
    int i, rc;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc)
            bind_name = argv[++i];
        else if (strcmp(argv[i], "--url") == 0 && i + 1 < argc)
            url = argv[++i];
        else if (strcmp(argv[i], "--ask") == 0 && i + 1 < argc)
            question = argv[++i];
        else if (argv[i][0] != '-' && url == NULL)
            url = argv[i];
        else {
            fprintf(stderr,
                    "usage: %s --bind integer|token|line|year --url URL "
                    "[--ask TEXT]\n",
                    argv[0]);
            return 2;
        }
    }
    if (url == NULL || parse_bind(bind_name, &bind) != 0) {
        fprintf(stderr,
                "usage: %s --bind integer|token|line|year --url URL "
                "[--ask TEXT]\n",
                argv[0]);
        return 2;
    }

    if (question != NULL) printf("question=%s\n", question);
    printf("url=%s\nbind=%s\ncontract=%s\n", url, bind_name,
           CNET_LOOKUP_CONTRACT);

    rc = cnet_lookup_execute(url, bind, &report);
    printf("rc=%d bound=%d refusal=%s host=%s status=%ld bytes=%zu sha256=%s "
           "value=%s\n",
           rc, report.bound, report.refusal[0] ? report.refusal : "-",
           report.host[0] ? report.host : "-", report.http_status, report.bytes,
           report.sha256[0] ? report.sha256 : "-",
           report.value[0] ? report.value : "-");
    if (report.snippet[0]) printf("snippet=%s\n", report.snippet);
    if (rc == 0 && cnet_lookup_speak(&report, spoken, sizeof spoken) == 0) {
        printf("spoken=%s\n", spoken);
        return 0;
    }
    printf("spoken=-\n");
    return rc == 1 ? 1 : 3;
}
