#include "../include/cnet_auto_learn.h"
#include "../include/gap_lane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fnv1a(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    if (!s) s = "";
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

int cnet_auto_learn_freeform_enabled(void) {
    const char *e = getenv("CNET_AUTO_LEARN_FREEFORM");
    /* Default OFF: freeform chat must not mint tk*q* sludge.
     * Set CNET_AUTO_LEARN_FREEFORM=1 to restore legacy rewrite. */
    if (!e || !e[0]) return 0;
    return !(e[0] == '0' && e[1] == '\0');
}

int cnet_auto_learn_enabled(void) {
    const char *e = getenv("CNET_AUTO_LEARN");
    if (!e || !e[0]) return 1; /* default ON for automatic learning */
    return !(e[0] == '0' && e[1] == '\0');
}

size_t cnet_auto_learn_window(void) {
    const char *e = getenv("CNET_AUTO_LEARN_W");
    long v;
    if (!e || !e[0]) {
        e = getenv("CNET_WINDOW_FILE");
        (void)e;
        return 256;
    }
    v = strtol(e, NULL, 10);
    if (v < 8) v = 8;
    if (v > 4096) v = 4096;
    return (size_t)v;
}

size_t cnet_auto_learn_k(void) {
    const char *e = getenv("CNET_AUTO_LEARN_K");
    long v = e && e[0] ? strtol(e, NULL, 10) : 3;
    if (v < 1) v = 1;
    if (v > 8) v = 8;
    return (size_t)v;
}

int cnet_auto_learn_shape_ok(Port in, Port goal) {
    return in.family == PORT_ONEHOT && in.field_count == 1 &&
           goal.family == PORT_ONEHOT &&
           goal.field_width == in.field_width &&
           goal.field_count >= 1 && goal.field_count <= 8 &&
           in.field_width >= 8;
}

static int is_tk_tag(const char *tag) {
    /* tk{digits}q{digits} */
    const char *p;
    if (!tag || tag[0] != 't' || tag[1] != 'k') return 0;
    p = tag + 2;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    if (*p != 'q') return 0;
    p++;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return *p == '\0';
}

int cnet_auto_learn_make_teachable(Port *in, Port *goal, const char *seed_text) {
    if (!cnet_auto_learn_freeform_enabled()) return 0; /* structured path only */

    size_t W, k;
    uint64_t h;
    unsigned long id;
    char gtag[PORT_TAG_MAX];
    const char *seed;

    if (!in || !goal) return -1;
    if (!cnet_auto_learn_enabled()) return 0;

    /* Already a canonical teachable tag — leave ports alone (may still fix W). */
    if (is_tk_tag(goal->tag) && cnet_auto_learn_shape_ok(*in, *goal))
        return 0;
    /* Structured skill tags — preserve name, only force window shape. */
    if (goal->tag[0] &&
        (strncmp(goal->tag, "skill_", 6) == 0 ||
         strncmp(goal->tag, "research_", 9) == 0 ||
         strncmp(goal->tag, "chunk_", 6) == 0)) {
        size_t W = cnet_auto_learn_window();
        size_t k = goal->field_count;
        char gtag[PORT_TAG_MAX];
        if (k < 1 || k > 8) k = cnet_auto_learn_k();
        snprintf(gtag, sizeof gtag, "%s", goal->tag);
        memset(in, 0, sizeof *in);
        in->family = PORT_ONEHOT;
        in->field_width = W;
        in->field_count = 1;
        port_set_tag(in, "w_cur");
        memset(goal, 0, sizeof *goal);
        goal->family = PORT_ONEHOT;
        goal->field_width = W;
        goal->field_count = k;
        port_set_tag(goal, gtag);
        return 1;
    }

    W = cnet_auto_learn_window();
    k = goal->field_count;
    if (k < 1 || k > 8) k = cnet_auto_learn_k();

    seed = (seed_text && seed_text[0]) ? seed_text
           : (goal->tag[0] ? goal->tag
              : (in->tag[0] ? in->tag : "chat"));
    h = fnv1a(seed);
    id = (unsigned long)(h % (uint64_t)W);
    if (id == 0) id = 1;

    memset(in, 0, sizeof *in);
    in->family = PORT_ONEHOT;
    in->field_width = W;
    in->field_count = 1;
    port_set_tag(in, "w_cur");

    memset(goal, 0, sizeof *goal);
    goal->family = PORT_ONEHOT;
    goal->field_width = W;
    goal->field_count = k;
    snprintf(gtag, sizeof gtag, "tk%luq%lu", id, id);
    port_set_tag(goal, gtag);
    return 1;
}

int cnet_auto_learn_note_text(const char *inbox_path, const char *text,
                             size_t k_override) {
    Port in, goal;
    size_t k;
    if (!inbox_path || !inbox_path[0] || !text) return -1;
    k = k_override > 0 ? k_override : cnet_auto_learn_k();
    if (k > 8) k = 8;
    memset(&in, 0, sizeof in);
    memset(&goal, 0, sizeof goal);
    goal.field_count = k;
    if (cnet_auto_learn_make_teachable(&in, &goal, text) < 0) return -1;
    return gap_inbox_note_no_plan(inbox_path, in, goal);
}

static void slugify(const char *src, char *dst, size_t cap) {
    size_t i = 0, j = 0;
    /* Leave room for skill_ prefix when caller adds it (7 chars). */
    size_t max_body = (cap > 8) ? cap - 1 : 8;
    if (max_body > 24) max_body = 24; /* skill_ + 24 = 30 < PORT_TAG_MAX */
    if (!dst || cap < 2) return;
    if (!src) src = "skill";
    while (src[i] && j + 1 < cap && j < max_body) {
        unsigned char c = (unsigned char)src[i++];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            dst[j++] = (char)c;
        else if (c >= 'A' && c <= 'Z')
            dst[j++] = (char)(c - 'A' + 'a');
        else if (c == '_' || c == '-' || c == ' ') {
            if (j == 0 || dst[j - 1] == '_') continue;
            dst[j++] = '_';
        }
    }
    while (j > 0 && dst[j - 1] == '_') j--;
    if (j == 0) {
        dst[0] = 'x';
        j = 1;
    }
    dst[j] = '\0';
}

int cnet_auto_learn_note_skill(const char *inbox_path, const char *skill_name,
                               const char *text, size_t k_override) {
    Port in, goal;
    size_t k, W;
    char slug[48], gtag[PORT_TAG_MAX];
    if (!inbox_path || !inbox_path[0] || !skill_name || !skill_name[0])
        return -1;
    k = k_override > 0 ? k_override : cnet_auto_learn_k();
    if (k > 8) k = 8;
    W = cnet_auto_learn_window();
    slugify(skill_name, slug, sizeof slug);
    if (strncmp(slug, "skill_", 6) == 0 || strncmp(slug, "research_", 9) == 0 ||
        strncmp(slug, "chunk_", 6) == 0)
        snprintf(gtag, sizeof gtag, "%s", slug);
    else
        snprintf(gtag, sizeof gtag, "skill_%s", slug);
    memset(&in, 0, sizeof in);
    memset(&goal, 0, sizeof goal);
    in.family = PORT_ONEHOT;
    in.field_width = W;
    in.field_count = 1;
    port_set_tag(&in, "w_cur");
    goal.family = PORT_ONEHOT;
    goal.field_width = W;
    goal.field_count = k;
    port_set_tag(&goal, gtag);
    (void)text; /* seed reserved for future exemplar attach */
    return gap_inbox_note_no_plan(inbox_path, in, goal);
}
