#ifndef CONTRACT_TEXT_ADD_COMPOUND_H
#define CONTRACT_TEXT_ADD_COMPOUND_H

#include "../nn.h"
#include "../router.h"
#include "contract.h"
#include "text_add.h"

/* Compound text contract v1: chains two text_add contracts.
 * e.g. (left op1 mid) op2 right
 * Reuses port_contract_text_add twice.
 * Produces single certified numeric.
 */

#define TEXT_COMPOUND_CONTRACT_NAME "glyph_text_add_compound"

int contract_init_text_add_compound(Contract *c,
                                    const Port *left, const Port *op1, const Port *mid,
                                    const Port *op2, const Port *right,
                                    const Port *out);

int port_contract_text_add_compound(const BinaryTransformNetwork *glyph_leaf,
                                    PrimitiveRegistry *reg,
                                    const double *left_feat, size_t left_size,
                                    const double *op1_feat, size_t op1_size,
                                    const double *mid_feat, size_t mid_size,
                                    const double *op2_feat, size_t op2_size,
                                    const double *right_feat, size_t right_size,
                                    double *numeric_out, size_t out_size,
                                    double cnet_d_influence,
                                    char *certified_str, size_t str_size);

#endif /* CONTRACT_TEXT_ADD_COMPOUND_H */




