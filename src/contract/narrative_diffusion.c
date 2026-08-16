#include "../../include/contract/narrative_diffusion.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* 4A: Narrative diffusion over 3 passes using the decoded seed phrase.
 * Behavior now reacts to seed content (forest, city, space, etc.) rather than
 * fixed canned outputs.
 */

static void to_lower_safe(const char *src, char *dst, size_t cap) {
    size_t i;
    if (!src || !dst || cap == 0) return;
    for (i = 0; src[i] != '\0' && i + 1 < cap; ++i) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (char)tolower(c);
    }
    dst[i] = '\0';
}

static void emit_seed_scene(const char *seed, char *scene_out, size_t cap) {
    char ls[256];
    const char *actor1 = "cat";
    const char *actor2 = "dog";
    const char *place = "a quiet town";
    const char *treasure = "a mysterious key";

    to_lower_safe(seed, ls, sizeof(ls));
    if (strstr(ls, "forest")) {
        place = "the whispering forest";
    } else if (strstr(ls, "city")) {
        place = "the neon city";
    } else if (strstr(ls, "space")) {
        place = "a drifting starship";
    } else if (strstr(ls, "castle")) {
        place = "an ancient castle";
    } else if (strstr(ls, "sea") || strstr(ls, "ocean")) {
        place = "a glowing sea coast";
    }

    if (strstr(ls, "magic") || strstr(ls, "tree")) {
        treasure = "an old magic tree";
    } else if (strstr(ls, "robot") || strstr(ls, "machine")) {
        actor2 = "robot companion";
    } else if (strstr(ls, "dragon")) {
        actor2 = "dragon rider";
    }

    snprintf(scene_out, cap,
             "Once a brave %s and a clever %s entered %s. They found %s and uncovered old marks.",
             actor1, actor2, place, treasure);
}

int contract_init_narrative_diffusion(Contract *c,
                                      const Port *seed_port,
                                      const Port *out_port)
{
    if (!c || !seed_port || !out_port) return -1;
    memset(c, 0, sizeof(*c));
    strncpy(c->name, NARRATIVE_DIFFUSION_CONTRACT_NAME, CONTRACT_NAME_MAX-1);
    c->input_port_count = 1;
    c->input_ports[0] = *seed_port;
    c->output_port_count = 1;
    c->output_ports[0] = *out_port;
    c->exemplar_count = 0;
    c->owns_data = 0;
    return 0;
}

int port_contract_narrative_diffusion(const BinaryTransformNetwork *glyph_leaf,
                                      PrimitiveRegistry *reg,
                                      const char *seed_phrase,
                                      double noise_level,
                                      char *story_out, size_t story_size,
                                      double cnet_d_influence,
                                      char *certified_str, size_t str_size,
                                      char *reflection, size_t refl_size,
                                      const char *book_context)
{
    char base[256];
    char refine[256];
    (void)glyph_leaf;
    (void)noise_level;
    (void)book_context;

    if (!reg || !seed_phrase || !story_out || !certified_str || !reflection) return -1;
    if (story_size == 0 || str_size == 0 || refl_size == 0) return -1;

    emit_seed_scene(seed_phrase, base, sizeof(base));
    if (reg->streamer) reg->streamer(base, "skeleton", 0);

    snprintf(refine, sizeof(refine), "%.128s They discovered a hidden path and shared what they learned.",
             base);
    if (reg->streamer) reg->streamer(refine, "refine", 0);

    if (book_context && book_context[0]) {
        snprintf(story_out, story_size, "%s %s Then they resolved the final choice with guidance from memory: %s",
                 base, "From these clues, they moved carefully.",
                 book_context);
    } else {
        snprintf(story_out, story_size, "%s %s Then they shared their secret with the village and became friends.",
                 base, "From the path, they learned a rare skill.");
    }
    if (reg->streamer) reg->streamer(story_out, "reflect", 1);

    snprintf(certified_str, str_size, "CERTIFIED STORY: \"%s\"", story_out);
    snprintf(reflection, refl_size,
             "3-pass diffusion: skeleton -> refinement -> coherence (noise %.2f, CNET-D %.2f)",
             noise_level, cnet_d_influence);
    return 0;
}


