#ifndef CONTRACT_NARRATIVE_BRANCHING_H
#define CONTRACT_NARRATIVE_BRANCHING_H

#include "../nn.h"
#include "../router.h"
#include "contract.h"
#include "narrative_diffusion.h"

/* 4B: Branching Multi-Sentence Narrator.
 * Extends diffusion with choice points at end of passes.
 * Scores branches with CNET-D + reflection, picks best, but can output alternative.
 * Emits tokens for sentences.
 */

#define NARRATIVE_BRANCHING_CONTRACT_NAME "narrative_branching"

int contract_init_narrative_branching(Contract *c,
                                      const Port *seed_port,
                                      const Port *out_port);

int port_contract_narrative_branching(const BinaryTransformNetwork *glyph_leaf,
                                      PrimitiveRegistry *reg,
                                      const char *seed_phrase,
                                      double noise_level,
                                      char *story_out, size_t story_size,
                                      double cnet_d_influence,
                                      char *certified_str, size_t str_size,
                                      char *reflection, size_t refl_size,
                                      char *alt_ending, size_t alt_size,
                                      const char *book_context /* optional */);

#endif /* CONTRACT_NARRATIVE_BRANCHING_H */




