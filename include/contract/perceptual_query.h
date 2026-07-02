#ifndef CONTRACT_PERCEPTUAL_QUERY_H
#define CONTRACT_PERCEPTUAL_QUERY_H

#include "../nn.h"
#include "../router.h"
#include "contract.h"
#include "text_add.h"
#include "text_add_compound.h"
#include "text_add_abstain.h"

/* 3G: Master PerceptualQuery orchestrator contract.
 * Ingests raw glyph sequence + noise, intelligently selects sub-contract
 * (simple/compound/abstain) based on scores including CNET-D.
 * Attaches reflection, supports chunk evolution.
 */

#define PERCEPTUAL_QUERY_CONTRACT_NAME "perceptual_query"

int contract_init_perceptual_query(Contract *c,
                                   const Port *glyphs, size_t num_glyphs,
                                   const Port *out);

int port_contract_perceptual_query(const BinaryTransformNetwork *glyph_leaf,
                                   PrimitiveRegistry *reg,
                                   const double **glyph_feats, size_t num_glyphs,
                                   double noise_level,
                                   double *numeric_out, size_t out_size,
                                   double cnet_d_influence,
                                   char *certified_str, size_t str_size,
                                   char *chosen_contract, size_t chosen_size,
                                   char *reflection, size_t reflection_size);

#endif /* CONTRACT_PERCEPTUAL_QUERY_H */




