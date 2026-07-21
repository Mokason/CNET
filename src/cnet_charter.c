#include "../include/cnet_charter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cnet_charter_enforce(void) {
    const char *e = getenv("CNET_CHARTER_ENFORCE");
    return e && e[0] == '1' && !e[1];
}

const char *cnet_charter_path(void) {
    const char *p = getenv("CNET_CHARTER_PATH");
    return (p && p[0]) ? p : "config/actuator_charter.txt";
}

static int line_allows(const char *rule, const char *tag) {
    size_t rl;
    if (!rule || !tag || !rule[0] || !tag[0]) return 0;
    rl = strlen(rule);
    if (strcmp(rule, tag) == 0) return 1;
    if (rl > 0 && rule[rl - 1] == '_')
        return strncmp(tag, rule, rl) == 0;
    /* bare prefix without underscore: prefix match */
    return strncmp(tag, rule, rl) == 0;
}

int cnet_charter_allows(const char *goal_tag) {
    FILE *f;
    char line[256];
    if (!goal_tag || !goal_tag[0]) return 0;
    if (!cnet_charter_enforce()) return 1; /* open when not enforced */
    f = fopen(cnet_charter_path(), "r");
    if (!f) return 1; /* missing charter = fail open for safety of ops */
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        size_t n;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' '))
            p[--n] = '\0';
        if (line_allows(p, goal_tag)) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}
