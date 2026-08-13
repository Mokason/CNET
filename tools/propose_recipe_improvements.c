/* Emit bounded, evidence-backed recipe proposals (Phase 4).
 * Never proposes lowering certification bars.
 *
 * Usage: propose_recipe_improvements [--out PATH] [--kind KIND]...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

typedef struct {
    const char *kind;
    int priority;
    const char *evidence;
    const char *tags_json; /* preformatted JSON array */
} Proposal;

static const char *BANNED[] = {
    "lower_margin", "lower_wilson", "skip_certify", "force_admit", "disable_cert",
    NULL
};

static const Proposal DEFAULTS[] = {
    {
        "topk_set_v2", 5,
        "qwythos margin sweep: set 255/256 vs ordered 101/256 at eps 0.02; new goldens required",
        "[\"recipe\", \"campaign\", \"topk_set\", \"never_lower_cert_bars\"]"
    },
    {
        "train_fast_default", 4,
        "TEACH_FAST_PASS: 1.61x serial byte-identical plain-SGD",
        "[\"recipe\", \"deploy\", \"train_fast\"]"
    },
    {
        "teacher_sleep_cnb_primary", 4,
        "CNET_TEACHER_IDLE_SEC + CNB serve; teacher only on open gaps",
        "[\"recipe\", \"deploy\", \"resource\"]"
    },
    {
        "budgeted_gap_drain", 3,
        "CNET_LANE_MAX_CLOSURES rate-limits drain without skipping certify",
        "[\"recipe\", \"gap_lane\", \"budget\"]"
    },
};

static int is_banned(const char *kind) {
    int i;
    for (i = 0; BANNED[i]; i++)
        if (strcmp(kind, BANNED[i]) == 0) return 1;
    return 0;
}

static void json_escape(const char *s, char *out, size_t out_sz) {
    size_t o = 0;
    if (o + 1 < out_sz) out[o++] = '"';
    for (; *s && o + 2 < out_sz; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            if (o + 3 >= out_sz) break;
            out[o++] = '\\'; out[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 7 >= out_sz) break;
            o += (size_t)snprintf(out + o, out_sz - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    if (o + 1 < out_sz) out[o++] = '"';
    out[o] = 0;
}

static int ensure_parent(const char *path) {
    char tmp[512];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *out_path = "suggestions/cnet_recipe_proposals.jsonl";
    const char *kinds[32];
    int n_kinds = 0, i, n = 0;
    FILE *f;
    time_t now = time(NULL);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--kind") == 0 && i + 1 < argc) {
            if (n_kinds < 32) kinds[n_kinds++] = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--out PATH] [--kind KIND]...\n", argv[0]);
            return 2;
        }
    }

    ensure_parent(out_path);
    f = fopen(out_path, "a");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", out_path);
        return 1;
    }

    for (i = 0; i < (int)(sizeof DEFAULTS / sizeof DEFAULTS[0]); i++) {
        const Proposal *p = &DEFAULTS[i];
        char id_esc[128], kind_esc[128], ev_esc[1024];
        char id_buf[128];
        int k, want = (n_kinds == 0);
        if (is_banned(p->kind)) {
            fprintf(stderr, "banned kind: %s\n", p->kind);
            fclose(f);
            return 1;
        }
        for (k = 0; k < n_kinds; k++)
            if (strcmp(kinds[k], p->kind) == 0) want = 1;
        if (!want) continue;

        snprintf(id_buf, sizeof id_buf, "cnet-recipe-%s", p->kind);
        json_escape(id_buf, id_esc, sizeof id_esc);
        json_escape(p->kind, kind_esc, sizeof kind_esc);
        json_escape(p->evidence, ev_esc, sizeof ev_esc);

        /* sort_keys order: evidence, id, kind, meta, priority, status, tags, ts */
        fprintf(f,
            "{\"evidence\":%s,\"id\":%s,\"kind\":%s,"
            "\"meta\":{\"actionable\":true,\"never_lower_cert_bars\":true,"
            "\"source\":\"propose_recipe_improvements\"},"
            "\"priority\":%d,\"status\":\"proposed\",\"tags\":%s,\"ts\":%ld}\n",
            ev_esc, id_esc, kind_esc, p->priority, p->tags_json, (long)now);
        n++;
    }
    fclose(f);
    printf("RECIPE_PROPOSALS_PASS wrote=%d path=%s\n", n, out_path);
    return 0;
}
