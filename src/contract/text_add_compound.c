#include "../../include/contract/text_add_compound.h"
#include <stdio.h>
#include <string.h>

/* Compound arithmetic contract: (left op1 mid) op2 right. */

int contract_init_text_add_compound(Contract *c,
                                    const Port *left, const Port *op1, const Port *mid,
                                    const Port *op2, const Port *right,
                                    const Port *out)
{
    if (!c || !left || !op1 || !mid || !op2 || !right || !out) return -1;
    memset(c, 0, sizeof(*c));
    strncpy(c->name, TEXT_COMPOUND_CONTRACT_NAME, CONTRACT_NAME_MAX-1);
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

int port_contract_text_add_compound(const BinaryTransformNetwork *glyph_leaf,
                                    PrimitiveRegistry *reg,
                                    const double *left_feat, size_t left_size,
                                    const double *op1_feat, size_t op1_size,
                                    const double *mid_feat, size_t mid_size,
                                    const double *op2_feat, size_t op2_size,
                                    const double *right_feat, size_t right_size,
                                    double *numeric_out, size_t out_size,
                                    double cnet_d_influence,
                                    char *certified_str, size_t str_size)
{
    int lc, mc, rc, op1c, op2c;
    int left_v, mid_v, right_v;
    char op1, op2;
    double inter = 0.0;
    double m_lc, m_mc, m_rc, m_op1, m_op2;
    double final_out = 0.0;
    (void)reg;

    if (!glyph_leaf || !numeric_out || !certified_str || out_size == 0) return -1;
    if (!left_feat || !op1_feat || !mid_feat || !op2_feat || !right_feat) return -1;

    if (contract_text_decode_symbol(glyph_leaf, left_feat, left_size, &lc, &m_lc) != 0 ||
        contract_text_decode_symbol(glyph_leaf, op1_feat, op1_size, &op1c, &m_op1) != 0 ||
        contract_text_decode_symbol(glyph_leaf, mid_feat, mid_size, &mc, &m_mc) != 0 ||
        contract_text_decode_symbol(glyph_leaf, op2_feat, op2_size, &op2c, &m_op2) != 0 ||
        contract_text_decode_symbol(glyph_leaf, right_feat, right_size, &rc, &m_rc) != 0) {
        return -1;
    }

    if (contract_text_map_digit(lc, &left_v) != 0 ||
        contract_text_map_digit(mc, &mid_v) != 0 ||
        contract_text_map_digit(rc, &right_v) != 0 ||
        contract_text_map_operator(op1c, &op1) != 0 ||
        contract_text_map_operator(op2c, &op2) != 0) {
        return -1;
    }

    if (contract_text_apply_binary((double)left_v, op1, (double)mid_v, &inter) != 0) {
        return -1;
    }
    if (contract_text_apply_binary(inter, op2, (double)right_v, &final_out) != 0) {
        return -1;
    }
    numeric_out[0] = final_out;

    if (cnet_d_influence >= 0.7 &&
        (m_lc < 0.2 || m_mc < 0.2 || m_rc < 0.2 ||
         m_op1 < 0.2 || m_op2 < 0.2)) {
        snprintf(certified_str, str_size,
                 "ABSTAIN certified low-margin path, final='%.0f' via compound contract",
                 final_out);
        return -1;
    } else {
        snprintf(certified_str, str_size,
                 "CERTIFIED coherent '%.0f' via compound contract",
                 final_out);
    }
    return 0;
}


