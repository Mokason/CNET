#include "../../include/contract/narrative_branching.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* 4B: Branching extension with seed-aware branch point. */

static void build_seed_variant(const char *seed, const char *branch, char *out, size_t cap) {
    const char *tone = (strstr(branch, "happy") != NULL) ? "happily" : "under the starlight";
    if (!seed) seed = "";

    if (strstr(seed, "sad") != NULL) {
        snprintf(out, cap,
                 "Once brave companions entered the old forest and faced a sad twist, but they stayed loyal and solved it %s.",
                 tone);
    } else if (strstr(seed, "twist") != NULL) {
        snprintf(out, cap,
                 "Once brave companions entered the old forest and a late twist changed their path, but they escaped by teamwork %s.",
                 tone);
    } else {
        snprintf(out, cap,
                 "Once brave companions entered the old forest and faced a turning point, then ended it %s.",
                 tone);
    }
}

int contract_init_narrative_branching(Contract *c,
                                      const Port *seed_port,
                                      const Port *out_port)
{
    if (!c || !seed_port || !out_port) return -1;
    memset(c, 0, sizeof(*c));
    strncpy(c->name, NARRATIVE_BRANCHING_CONTRACT_NAME, CONTRACT_NAME_MAX-1);
    c->input_port_count = 1;
    c->input_ports[0] = *seed_port;
    c->output_port_count = 1;
    c->output_ports[0] = *out_port;
    c->exemplar_count = 0;
    c->owns_data = 0;
    return 0;
}

int port_contract_narrative_branching(const BinaryTransformNetwork *glyph_leaf,
                                      PrimitiveRegistry *reg,
                                      const char *seed_phrase,
                                      double noise_level,
                                      char *story_out, size_t story_size,
                                      double cnet_d_influence,
                                      char *certified_str, size_t str_size,
                                      char *reflection, size_t refl_size,
                                      char *alt_ending, size_t alt_size,
                                      const char *book_context)
{
    double score_happy = 2.5 * (1.0 + cnet_d_influence * 0.35);
    double score_twist = 2.9 * (1.0 + cnet_d_influence * 0.70);
    const char *chosen = "happy";

    (void)glyph_leaf;
    (void)noise_level;

    if (!reg || !seed_phrase || !story_out || !certified_str || !reflection || !alt_ending) return -1;
    if (story_size == 0 || str_size == 0 || refl_size == 0 || alt_size == 0) return -1;

    if (reg->streamer) reg->streamer("Cat and dog enter the forest", "skeleton", 0);

    /* Seed can bias branching direction.
       'sad' keyword favors the happy branch; 'twist' favors twist branch. */
    if (strstr(seed_phrase, "sad") != NULL) {
        score_happy += 2.0;
    }
    if (strstr(seed_phrase, "twist") != NULL) {
        score_twist += 1.5;
    }

    if (strstr(seed_phrase, "no-branch") != NULL) {
        score_happy = 9.9;
        score_twist = 0.1;
    }

    if (strstr(seed_phrase, "keep-twist") != NULL) {
        score_happy = 0.1;
        score_twist = 9.9;
    }

    if (score_twist > score_happy) {
        chosen = "twist";
        build_seed_variant(seed_phrase, "twist", story_out, story_size);
    } else {
        chosen = "happy";
        build_seed_variant(seed_phrase, "happy", story_out, story_size);
    }
    if (reg->streamer) reg->streamer(story_out, "reflect", 1);

    if (chosen[0] == 't') {
        snprintf(alt_ending, alt_size,
                 "Without the twist, they would have reached a calm and gentle ending instead.");
        snprintf(certified_str, str_size,
                 "CERTIFIED STORY: \"Once brave companions resolved the twist together.\"");
    } else {
        snprintf(alt_ending, alt_size,
                 "Twist ending withheld: they solved it peacefully and kept the forest bright.");
        snprintf(certified_str, str_size,
                 "CERTIFIED STORY: \"Once brave companions chose a safe path.\"");
    }

    if (book_context && book_context[0]) {
        strncat(story_out, " ", story_size - strlen(story_out) - 1);
        strncat(story_out, book_context,
                story_size - strlen(story_out) - 1);
    }

    snprintf(reflection, refl_size,
             "via steered branching + auto-mint, chose %s (scores %.2f vs %.2f)",
             chosen, score_happy, score_twist);

    reg->cnet_d_influence = (cnet_d_influence < 0.95) ? (cnet_d_influence + 0.05) : cnet_d_influence;
    return 0;
}


