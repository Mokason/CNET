#ifndef CONTRACT_TEXT_ADD_ABSTAIN_H
#define CONTRACT_TEXT_ADD_ABSTAIN_H

#include "../nn.h"
#include "../router.h"
#include "contract.h"
#include "text_add.h"
#include "text_add_compound.h"

/* 3F Abstention contract for high noise.
 * Same signature as compound, but if margin < 0.4 on any input glyph, ABSTAIN.
 * Else delegate to compound.
 */

#define TEXT_ABSTAIN_CONTRACT_NAME "glyph_text_add_abstain"

int contract_init_text_add_abstain(Contract *c,
                                   const Port *left, const Port *op1, const Port *mid,
                                   const Port *op2, const Port *right,
                                   const Port *out);

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
                                   double noise_level);

#endif /* CONTRACT_TEXT_ADD_ABSTAIN_H */




