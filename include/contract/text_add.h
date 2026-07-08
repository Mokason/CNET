#ifndef CONTRACT_TEXT_ADD_H
#define CONTRACT_TEXT_ADD_H

#include "../nn.h"
#include "../router.h"
#include "contract.h"

/* Text-level contract v1: parse two glyph ports + operator -> certified numeric result.
 * This is the first contract above the primitive layer for "text" (perceptual glyphs).
 * Internals use the glyph leaf + dec_value + dec_full_add + response emit.
 */

#define TEXT_CONTRACT_NAME "glyph_text_add"

/* Create a text add contract descriptor.
 * left, op, right: glyph ports (typically raw or onehot for glyph feats, or dec_symbol).
 * out: numeric port (dec_sum or similar).
 * Returns a Contract (caller must contract_free if owns_data).
 * Exemplars are generated for canonical cases (0-9 + op).
 */
int contract_init_text_add(Contract *c,
                           const Port *left, const Port *op, const Port *right,
                           const Port *out);

/* The runtime "primitive" for the text add contract.
 * Takes three input vectors (left glyph, op glyph, right glyph), produces numeric output.
 * Internally does margin validation, snap, plan using recipe, execute.
 * If reg has the minted chunk and expansion logic, uses it based on power mode.
 * cnet_d_influence: optional bias for CNET-D (0.0 disables for now).
 * Returns 0 on success, -1 on bad inputs or planning failure.
 */
int port_contract_text_add(const BinaryTransformNetwork *glyph_leaf,  /* the perception leaf */
                           PrimitiveRegistry *reg,
                           const double *left_feat, size_t left_size,
                           const double *op_feat, size_t op_size,
                           const double *right_feat, size_t right_size,
                           double *numeric_out, size_t out_size,
                           double cnet_d_influence,  /* 0.1 for demo */
                           char *certified_str, size_t str_size);

/* Internal helpers shared by text-add contract variants.
 * These are intentionally minimal and deterministic:
 * - decode a glyph feature vector to a symbol code
 * - map symbol code -> digit/operator
 * - execute a binary operator over numeric values
 * They are not part of the external contract API contract and are used
 * by the text-add variants (3D/3E/3F) for consistency.
 */
int contract_text_decode_symbol(const BinaryTransformNetwork *glyph_leaf,
                               const double *feat, size_t feat_len,
                               int *symbol_code,
                               double *margin);
int contract_text_map_digit(int symbol_code, int *digit);
int contract_text_map_operator(int symbol_code, char *op);
int contract_text_apply_binary(double left, char op, double right, double *out);

#endif /* CONTRACT_TEXT_ADD_H */





