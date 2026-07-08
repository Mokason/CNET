#ifndef CONTRACT_INTERACTIVE_AGENT_H
#define CONTRACT_INTERACTIVE_AGENT_H

#include "../nn.h"
#include "../router.h"
#include "contract.h"
#include "perceptual_query.h"
#include "narrative_diffusion.h"
#include "narrative_branching.h"
#include "mcp_wiki.h"
#include "mcp_web_search.h"
#include "mcp_file_read.h"
#include "mcp_calculator.h"
#include "mcp_summarizer.h"
#include "mcp_file_write.h"
#include "../agent_memory.h"
#include "book_concept.h"

/* 5A: Interactive Memory Agent.
 * Top-level contract for user queries.
 * Decides mode: math/story/branching/abstain using full stack.
 * Recalls persisted memory (last story from 4B).
 * Self-reflection + status report.
 */

#define INTERACTIVE_AGENT_CONTRACT_NAME "interactive_agent"

int contract_init_interactive_agent(Contract *c,
                                    const Port *query_port,
                                    const Port *out_port);

int port_contract_interactive_agent(const BinaryTransformNetwork *glyph_leaf,
                                    PrimitiveRegistry *reg,
                                    const char *query,
                                    double noise_level,
                                    char *response, size_t response_size,
                                    double cnet_d_influence,
                                    char *certified_str, size_t str_size,
                                    char *reflection, size_t refl_size,
                                    char *status_report, size_t report_size);

#endif /* CONTRACT_INTERACTIVE_AGENT_H */




