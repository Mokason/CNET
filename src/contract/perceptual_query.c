#include "../../include/contract/perceptual_query.h"
#include <stdio.h>
#include <string.h>

/* 3G: Parse decoded glyphs and choose the best sub-contract.
 * The contract now evaluates the real input rather than hardcoded demo tokens.
 */

static void set_reflection(char *out, size_t out_cap, const char *mode) {
    if (out == NULL || out_cap == 0) return;
    snprintf(out, out_cap, "via steered %s contract selection", mode);
}

static int emit_universal_abstain(const char *mode, double *numeric_out, size_t out_size,
                                 char *certified_str, size_t str_size,
                                 char *chosen_contract, size_t chosen_size,
                                 char *reflection, size_t reflection_size) {
    if (numeric_out && out_size > 0) {
        numeric_out[0] = -1;
    }
    if (certified_str && str_size > 0) {
        snprintf(certified_str, str_size, "ABSTAIN certified (noise margin fail)");
    }
    if (chosen_contract && chosen_size > 0) {
        snprintf(chosen_contract, chosen_size, "abstain");
    }
    if (reflection && reflection_size > 0) {
        snprintf(reflection, reflection_size, "via steered %s contract", mode ? mode : "abstain");
    }
    return 0;
}

int contract_init_perceptual_query(Contract *c,
                                   const Port *glyphs, size_t num_glyphs,
                                   const Port *out)
{
    if (!c || !glyphs || !out) return -1;
    memset(c, 0, sizeof(*c));
    strncpy(c->name, PERCEPTUAL_QUERY_CONTRACT_NAME, CONTRACT_NAME_MAX-1);
    c->input_port_count = num_glyphs;
    for (size_t i = 0; i < num_glyphs && i < BTN_MAX_INPUT_PORTS; i++) {
        c->input_ports[i] = glyphs[i];
    }
    c->output_port_count = 1;
    c->output_ports[0] = *out;
    c->exemplar_count = 0;
    c->owns_data = 0;
    return 0;
}

int port_contract_perceptual_query(const BinaryTransformNetwork *glyph_leaf,
                                   PrimitiveRegistry *reg,
                                   const double **glyph_feats, size_t num_glyphs,
                                   double noise_level,
                                   double *numeric_out, size_t out_size,
                                   double cnet_d_influence,
                                   char *certified_str, size_t str_size,
                                   char *chosen_contract, size_t chosen_size,
                                   char *reflection, size_t reflection_size)
{
    int sym0 = -1, sym1 = -1, sym2 = -1, sym3 = -1, sym4 = -1;
    double m0 = 0.0, m1 = 0.0, m2 = 0.0, m3 = 0.0, m4 = 0.0;
    int parse_simple = 0, parse_compound = 0;
    double score_simple, score_compound, score_abstain;
    double final_score = 0.0;
    const char *selected = "abstain";

    (void)reg;
    if (!glyph_leaf || !glyph_feats || !numeric_out || !certified_str ||
        !chosen_contract || !reflection || out_size == 0) {
        return -1;
    }

    if (num_glyphs >= 3) {
        if (!glyph_feats[0] || !glyph_feats[1] || !glyph_feats[2]) return -1;
        if (contract_text_decode_symbol(glyph_leaf, glyph_feats[0], 10, &sym0, &m0) == 0 &&
            contract_text_decode_symbol(glyph_leaf, glyph_feats[1], 10, &sym1, &m1) == 0 &&
            contract_text_decode_symbol(glyph_leaf, glyph_feats[2], 10, &sym2, &m2) == 0) {
            parse_simple = 1;
            if (sym0 >= 0 && sym0 <= 9 && sym2 >= 0 && sym2 <= 9) {
                parse_simple = 1;
            }
        }
    }

    if (num_glyphs >= 5) {
        if (!glyph_feats[3] || !glyph_feats[4]) {
            return emit_universal_abstain("parse", numeric_out, out_size,
                                          certified_str, str_size,
                                          chosen_contract, chosen_size,
                                          reflection, reflection_size);
        }
        if (contract_text_decode_symbol(glyph_leaf, glyph_feats[3], 10, &sym3, &m3) == 0 &&
            contract_text_decode_symbol(glyph_leaf, glyph_feats[4], 10, &sym4, &m4) == 0) {
            if ((sym0 >= 0 && sym0 <= 9) && (sym2 >= 0 && sym2 <= 9) &&
                (sym4 >= 0 && sym4 <= 9)) {
                parse_compound = 1;
            }
        }
    }

    score_simple = (1.0 - noise_level) * (1.0 + cnet_d_influence);
    score_compound = (1.0 - noise_level * 0.5) * (1.0 + cnet_d_influence * 1.2);
    if (parse_simple) {
        score_simple *= ((m0 + m1 + m2) / 3.0);
    }
    if (parse_compound) {
        score_compound *= ((m0 + m1 + m2 + m3 + m4) / 5.0);
    }
    score_abstain = (noise_level > 0.62) ? (1.0 + noise_level) : 0.0;

    if (score_abstain >= score_simple && score_abstain >= score_compound) {
        return emit_universal_abstain("abstain", numeric_out, out_size,
                                      certified_str, str_size,
                                      chosen_contract, chosen_size,
                                      reflection, reflection_size);
    }

    if (parse_compound && (!parse_simple || score_compound >= score_simple)) {
        selected = "compound";
        if (port_contract_text_add_compound(glyph_leaf, reg,
                                           glyph_feats[0], 10,
                                           glyph_feats[1], 10,
                                           glyph_feats[2], 10,
                                           glyph_feats[3], 10,
                                           glyph_feats[4], 10,
                                           numeric_out, out_size,
                                           cnet_d_influence, certified_str, str_size) != 0) {
            return emit_universal_abstain("compound fallback", numeric_out, out_size,
                                          certified_str, str_size,
                                          chosen_contract, chosen_size,
                                          reflection, reflection_size);
        }
        final_score = score_compound;
    } else if (parse_simple) {
        int left_d, right_d;
        char op;
        selected = "simple";
        if (contract_text_map_digit(sym0, &left_d) != 0 ||
            contract_text_map_digit(sym2, &right_d) != 0 ||
            contract_text_map_operator(sym1, &op) != 0 ||
            contract_text_apply_binary((double)left_d, op, (double)right_d, numeric_out) != 0) {
            return emit_universal_abstain("simple fallback", numeric_out, out_size,
                                          certified_str, str_size,
                                          chosen_contract, chosen_size,
                                          reflection, reflection_size);
        }
        final_score = score_simple;
    } else {
        return emit_universal_abstain("parse", numeric_out, out_size,
                                      certified_str, str_size,
                                      chosen_contract, chosen_size,
                                      reflection, reflection_size);
    }

    if (chosen_contract && chosen_size > 0) {
        snprintf(chosen_contract, chosen_size, "%s (score %.2f)", selected, final_score);
    }
    set_reflection(reflection, reflection_size, selected);
    if (certified_str && str_size > 0 && strncmp(certified_str, "CERTIFIED", 9) != 0) {
        if (selected && strcmp(selected, "compound") == 0) {
            snprintf(certified_str, str_size, "CERTIFIED %.0f via steered compound", numeric_out[0]);
        } else {
            snprintf(certified_str, str_size, "CERTIFIED %.0f via steered simple", numeric_out[0]);
        }
    }
    return 0;
}


