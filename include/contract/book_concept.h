#ifndef CONTRACT_BOOK_CONCEPT_H
#define CONTRACT_BOOK_CONCEPT_H

#include "../nn.h"
#include "../router.h"
#include "contract.h"
#include "../agent_memory.h"

/* Book Concept Contract (6A extension)
 * Turns extracted entities/titles from ingested book text into PORT_CONCEPT representations.
 * These can be used by the planner and narrative contracts for higher-level reasoning.
 *
 * In this slice: extracts from memory, registers conceptual "ports" (demo style),
 * and allows conditioning responses.
 */

#define BOOK_CONCEPT_CONTRACT_NAME "book_concept"

int contract_init_book_concept(Contract *c, Port *out_port);

/* Main entry: scan recent book thoughts, extract entities, create PORT_CONCEPT style
 * summaries. Fills concepts_out with lines like "CONCEPT: Lira (inventor, crystal)".
 * Returns number of concepts surfaced. */
int port_contract_book_concept(
    PrimitiveRegistry *reg,
    char *concepts_out,
    size_t concepts_cap,
    char *reflection,
    size_t refl_cap
);

/* Helper: turn a string entity into a Port with PORT_CONCEPT family (for future wiring) */
int book_entity_to_concept_port(const char *entity, Port *out_port);

/* 7C: Distill book concepts into a minted chunk using registry/expansion (reuses 3C machinery).
 * Creates a placeholder chunk with recipe back to the source concepts/thoughts.
 * Returns 1 if a chunk was "minted" (registered). */
int book_distill_to_minted_chunk(PrimitiveRegistry *reg, const char *concepts_summary, const char *source);

#endif /* CONTRACT_BOOK_CONCEPT_H */




