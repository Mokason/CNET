#include "../../include/contract/text_add_abstain.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* 3F: abstain if any decoded glyph is under confidence floor. */

static int glyph_margin_floor(const BinaryTransformNetwork *glyph_leaf,
                             const double *feat, size_t feat_len,
                             double *out_margin) {
    int symbol_code = -1;
    double m = 0.0;
    if (contract_text_decode_symbol(glyph_leaf, feat, feat_len, &symbol_code, &m) != 0) {
        return -1;
    }
    if (out_margin != NULL) {
        *out_margin = m;
    }
    return symbol_code;
}

int contract_init_text_add_abstain(Contract *c,
                                   const Port *left, const Port *op1, const Port *mid,
                                   const Port *op2, const Port *right,
                                   const Port *out)
{
    if (!c || !left || !op1 || !mid || !op2 || !right || !out) return -1;
    memset(c, 0, sizeof(*c));
    strncpy(c->name, TEXT_ABSTAIN_CONTRACT_NAME, CONTRACT_NAME_MAX-1);
    c->input_port_count = 5;
    c->input_ports[0] = *left;
    c->input_ports[1] = *op1;
    c->input_ports[2] = *mid;
    c->input_ports[3] = *op2;
    c->input_ports[4] = *right;
    c->output_port_count = 1;
    c->output_ports[0] = *out;
    c->exemplar_count = 0;
    c->owns_data = 0;
    return 0;
}

int port_contract_text_add_abstain(const BinaryTransformNetwork *glyph_leaf,
                                   PrimitiveRegistry *reg,
                                   const double *left_feat, size_t left_size,
                                   const double *op1_feat, size_t op1_size,
                                   const double *mid_feat, size_t mid_size,
                                   const double *op2_feat, size_t op2_size,
                                   const double *right_feat, size_t right_size,
                                   double *numeric_out, size_t out_size,
                                   double cnet_d_influence,
                                   char *certified_str, size_t str_size,
                                   double noise_level)
{
    double m[5];
    int i;
    (void)reg;
    (void)cnet_d_influence;

    if (!glyph_leaf || !left_feat || !op1_feat || !mid_feat || !op2_feat ||
        !right_feat || !numeric_out || !certified_str || out_size == 0) {
        return -1;
    }

    if (glyph_margin_floor(glyph_leaf, left_feat, left_size, &m[0]) < 0 ||
        glyph_margin_floor(glyph_leaf, op1_feat, op1_size, &m[1]) < 0 ||
        glyph_margin_floor(glyph_leaf, mid_feat, mid_size, &m[2]) < 0 ||
        glyph_margin_floor(glyph_leaf, op2_feat, op2_size, &m[3]) < 0 ||
        glyph_margin_floor(glyph_leaf, right_feat, right_size, &m[4]) < 0) {
        return -1;
    }

    /* Confidence-aware abstain gate.
       High-noise paths or ambiguous symbols abort early. */
    double min_margin = m[0];
    for (i = 1; i < 5; ++i) {
        if (m[i] < min_margin) min_margin = m[i];
    }
    if (noise_level > 0.6 || min_margin < 0.20 - 0.1 * (fabs(noise_level))) {
        numeric_out[0] = -1.0;
        snprintf(certified_str, str_size,
                 "ABSTAIN certified (noise margin fail) noise=%.2f", noise_level);
        return 0;
    }

    return port_contract_text_add_compound(glyph_leaf, reg,
                                          left_feat, left_size,
                                          op1_feat, op1_size,
                                          mid_feat, mid_size,
                                          op2_feat, op2_size,
                                          right_feat, right_size,
                                          numeric_out, out_size,
                                          cnet_d_influence,
                                          certified_str, str_size);
}


