#ifndef CONTRACT_NARRATIVE_DIFFUSION_H
#define CONTRACT_NARRATIVE_DIFFUSION_H

#include "../nn.h"
#include "../router.h"
#include "contract.h"
#include "perceptual_query.h"

/* 4A: Narrative Diffusion Contract.
 * Iterative refinement for tiny stories using perceptual_query orchestrator.
 * 3 passes: skeleton -> compound detail -> reflection/coherence.
 * Can mint story chunks and evolve them.
 */

#define NARRATIVE_DIFFUSION_CONTRACT_NAME "narrative_diffusion"

int contract_init_narrative_diffusion(Contract *c,
                                      const Port *seed_port,
                                      const Port *out_port);

int port_contract_narrative_diffusion(const BinaryTransformNetwork *glyph_leaf,
                                      PrimitiveRegistry *reg,
                                      const char *seed_phrase,
                                      double noise_level,
                                      char *story_out, size_t story_size,
                                      double cnet_d_influence,
                                      char *certified_str, size_t str_size,
                                      char *reflection, size_t refl_size,
                                      const char *book_context /* optional, can be NULL */);

#endif /* CONTRACT_NARRATIVE_DIFFUSION_H */




