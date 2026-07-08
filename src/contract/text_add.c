#include "../../include/contract/text_add.h"
#include "../../include/plan_table.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Decode one glyph into a single symbol code.
 * Uses the supplied glyph leaf's output representation and canonicalizes prior to
 * argmax for robustness.
 */
static int decode_symbol_code(const BinaryTransformNetwork *glyph_leaf,
                             const double *feat, size_t feat_len,
                             int *symbol_code, double *margin,
                             int require_confident) {
    /* Support for evidence distrib (from CCE sub-forest): if looks like distrib, use it directly */
    double s = 0; int non1 = 0;
    for (size_t ii=0; ii< (feat_len>10?10:feat_len); ii++) { s += feat[ii]; if (feat[ii]>0.1) non1++; }
    if (s > 0.8 && s < 1.2 && non1 > 1) {
        /* evidence mode */
        double top1=feat[0], top2=0; int b=0;
        for (int j=1; j<10; j++) if (feat[j]>top1) {top2=top1;top1=feat[j];b=j;} else if(feat[j]>top2) top2=feat[j];
        *symbol_code = b; *margin = top1 - top2;
        return 0;
    }
    size_t in_total;
    size_t i;
    double m;
    double *clean = NULL;
    const double *raw;

    if (!glyph_leaf || !feat || !symbol_code || feat_len == 0) {
        return -1;
    }
    if (glyph_leaf->output_port_count == 0 || glyph_leaf->input_port_count == 0) {
        return -1;
    }
    in_total = plan_port_total(glyph_leaf->input_ports[0]);
    for (i = 1; i < glyph_leaf->input_port_count; ++i) {
        in_total += plan_port_total(glyph_leaf->input_ports[i]);
    }
    if (feat_len != in_total) {
        return -1;
    }

    raw = btn_forward(glyph_leaf, feat);
    if (raw == NULL) {
        return -1;
    }

    for (i = 0; i < glyph_leaf->output_port_count; ++i) {
        if (glyph_leaf->output_ports[i].field_width == 0 ||
            glyph_leaf->output_ports[i].field_count == 0) {
            return -1;
        }
    }

    clean = malloc(plan_port_total(glyph_leaf->output_ports[0]) * sizeof *clean);
    if (clean == NULL) {
        return -1;
    }

    if (port_canonicalize(glyph_leaf->output_ports[0], raw, clean) != 0) {
        free(clean);
        return -1;
    }
    if (margin != NULL) {
        if (port_margin(glyph_leaf->output_ports[0], raw, &m) != 0) {
            free(clean);
            return -1;
        }
        *margin = m;
    }
    {
        size_t top = 0;
        double best = clean[0];
        size_t total = plan_port_total(glyph_leaf->output_ports[0]);
        for (i = 1; i < total; ++i) {
            if (clean[i] > best) {
                best = clean[i];
                top = i;
            }
        }
        *symbol_code = (int)top;
        if (require_confident && margin != NULL && *margin < 0.2) {
            free(clean);
            return -1;
        }
    }
    free(clean);
    return 0;
}

static int decode_op_code_to_char(int symbol_code, char *op_out) {
    char op = '+';
    switch (symbol_code) {
    case 0:
        op = '+';
        break;
    case 1:
        op = '-';
        break;
    case 2:
        op = '*';
        break;
    case 3:
        op = '/';
        break;
    case 4:
        op = '%';
        break;
    default:
        return -1;
    }
    *op_out = op;
    return 0;
}

int contract_text_decode_symbol(const BinaryTransformNetwork *glyph_leaf,
                               const double *feat, size_t feat_len,
                               int *symbol_code,
                               double *margin) {
    return decode_symbol_code(glyph_leaf, feat, feat_len, symbol_code, margin, 0);
}

int contract_text_map_digit(int symbol_code, int *digit) {
    if (digit == NULL) {
        return -1;
    }
    if (symbol_code < 0 || symbol_code > 9) {
        return -1;
    }
    *digit = symbol_code;
    return 0;
}

int contract_text_map_operator(int symbol_code, char *op) {
    if (op == NULL) {
        return -1;
    }
    return decode_op_code_to_char(symbol_code, op);
}

int contract_text_apply_binary(double left, char op, double right, double *out) {
    if (out == NULL) {
        return -1;
    }
    switch (op) {
    case '+':
        *out = left + right;
        return 0;
    case '-':
        *out = left - right;
        return 0;
    case '*':
        *out = left * right;
        return 0;
    case '/':
        if (fabs(right) < 1e-12) {
            return -1;
        }
        *out = left / right;
        return 0;
    case '%': {
        long a = (long)llround(left);
        long b = (long)llround(right);
        if (b == 0) {
            return -1;
        }
        *out = (double)(a % b);
        return 0;
    }
    default:
        return -1;
    }
}

int contract_init_text_add(Contract *c,
                           const Port *left, const Port *op, const Port *right,
                           const Port *out)
{
    if (!c || !left || !op || !right || !out) return -1;

    memset(c, 0, sizeof(*c));
    strncpy(c->name, TEXT_CONTRACT_NAME, CONTRACT_NAME_MAX-1);
    c->input_port_count = 3;
    c->input_ports[0] = *left;
    c->input_ports[1] = *op;
    c->input_ports[2] = *right;
    c->output_port_count = 1;
    c->output_ports[0] = *out;

    /* Exemplars can be generated by consolidation for true runtime training.
       Keeping this v1 contract table empty by default is valid for runtime
       inference usage. */
    c->exemplar_count = 0;
    c->owns_data = 0;
    return 0;
}

int port_contract_text_add(const BinaryTransformNetwork *glyph_leaf,
                           PrimitiveRegistry *reg,
                           const double *left_feat, size_t left_size,
                           const double *op_feat, size_t op_size,
                           const double *right_feat, size_t right_size,
                           double *numeric_out, size_t out_size,
                           double cnet_d_influence,
                           char *certified_str, size_t str_size)
{
    int left_code, op_code, right_code;
    int left_val, right_val;
    double left_margin, op_margin, right_margin;
    double score = 0.0;
    char op = '+';

    (void)reg;

    if (!glyph_leaf || !left_feat || !op_feat || !right_feat || !numeric_out ||
        !certified_str || out_size == 0) {
        return -1;
    }

    if (decode_symbol_code(glyph_leaf, left_feat, left_size, &left_code,
                          &left_margin, 1) != 0 ||
        decode_symbol_code(glyph_leaf, op_feat, op_size, &op_code, &op_margin, 1) != 0 ||
        decode_symbol_code(glyph_leaf, right_feat, right_size, &right_code,
                          &right_margin, 1) != 0) {
        return -1;
    }

    if (contract_text_map_digit(left_code, &left_val) != 0 ||
        contract_text_map_digit(right_code, &right_val) != 0 ||
        contract_text_map_operator(op_code, &op) != 0) {
        return -1;
    }

    if (contract_text_apply_binary((double)left_val, op,
                                  (double)right_val, numeric_out) != 0) {
        return -1;
    }

    score = (left_margin + op_margin + right_margin) / 3.0;
    if (score < 0.12) {
        return -1;
    }

    if (cnet_d_influence > 0.0) {
        /* Small deterministic CNET-D aware bias in certification, not in value. */
        snprintf(certified_str, str_size,
                 "CERTIFIED coherent '%.0f' via text contract (CNET-D %.2f)",
                 numeric_out[0], cnet_d_influence);
    } else {
        snprintf(certified_str, str_size,
                 "CERTIFIED coherent '%.0f' via text contract",
                 numeric_out[0]);
    }

    return 0;
}


