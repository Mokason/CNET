#include "../../include/contract/book_concept.h"
#include "../../include/contract/contract.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Simple demo impl: pulls from agent memory (book entities), surfaces as CONCEPTs.
 * Does not yet register real BTNs (that would require training data), but
 * creates Port objects with PORT_CONCEPT and feeds summaries to caller.
 */

int contract_init_book_concept(Contract *c, Port *out_port) {
    if (!c || !out_port) return -1;
    memset(c, 0, sizeof(*c));
    strncpy(c->name, BOOK_CONCEPT_CONTRACT_NAME, CONTRACT_NAME_MAX-1);
    c->input_port_count = 0;
    c->output_port_count = 1;
    c->output_ports[0] = *out_port;
    c->exemplar_count = 0;
    c->owns_data = 0;
    return 0;
}

int book_entity_to_concept_port(const char *entity, Port *out_port) {
    if (!entity || !out_port) return -1;
    out_port->family = PORT_CONCEPT;
    out_port->field_width = 1;
    out_port->field_count = 1;
    port_set_tag(out_port, entity);
    return 0;
}

int port_contract_book_concept(
    PrimitiveRegistry *reg,
    char *concepts_out,
    size_t concepts_cap,
    char *reflection,
    size_t refl_cap
) {
    if (!concepts_out || !reflection || concepts_cap == 0 || refl_cap == 0) return -1;
    if (!reg) reg = NULL; /* not used yet for real registration in this slice */

    concepts_out[0] = '\0';
    reflection[0] = '\0';

    char know[1024];
    int n = agent_recall_knowledge("book", know, sizeof(know), 8);
    if (n == 0) {
        agent_recall_knowledge("", know, sizeof(know), 5); /* fallback any */
    }

    /* Parse entities from the tagged thoughts */
    char entities[512] = {0};
    const char *p = know;
    while ((p = strstr(p, "[ENTITIES:")) != NULL) {
        p += 10;
        const char *end = strchr(p, ']');
        if (!end) break;
        size_t len = end - p;
        if (len > 0 && strlen(entities) + len + 2 < sizeof(entities)) {
            if (entities[0]) strcat(entities, ", ");
            strncat(entities, p, len);
        }
        p = end;
    }

    /* Build concept summary */
    int count = 0;
    char *tok = strtok(entities, ", ");
    while (tok) {
        char line[128];
        Port cp;
        book_entity_to_concept_port(tok, &cp);
        snprintf(line, sizeof(line), "CONCEPT_PORT: family=CONCEPT tag=%s\n", tok);
        if (strlen(concepts_out) + strlen(line) < concepts_cap) {
            strcat(concepts_out, line);
        }
        count++;
        tok = strtok(NULL, ", ");
        if (count > 6) break;
    }

    if (count == 0) {
        /* fallback: treat recalled knowledge as concepts */
        snprintf(concepts_out, concepts_cap, "CONCEPT: BookKnowledge (from memory layer)\n");
        count = 1;
    }

    snprintf(reflection, refl_cap,
             "Book elements abstracted into %d PORT_CONCEPT entries. Ready for planner/narrative conditioning. (6A slice)",
             count);

    /* In a fuller version we would registry_add conceptual BTNs here */
    return count;
}

int book_distill_to_minted_chunk(PrimitiveRegistry *reg, const char *concepts_summary, const char *source) {
    if (!reg || !concepts_summary) return 0;

    // Create a synthetic "chunk" entry for the book knowledge
    // Use expansion recipe pointing to "perceptual_query + book_concept" as teachers
    const char *recipe[2] = {"contract_perceptual_query", "book_concept"};
    // Add a placeholder entry (reuse an existing btn or just register name)
    // For demo, we "mint" by setting expansion on a virtual chunk name
    static BinaryTransformNetwork book_chunk = {0};
    // minimal: use registry_set_expansion to record it as minted
    int res = registry_set_expansion(reg, "book_knowledge_chunk", recipe, 2, 100, 500, 1);
    if (res == 0) {
        // mark as FROZEN like certified
        registry_set_state(reg, "book_knowledge_chunk", PRIM_FROZEN);
        return 1;
    }
    return 0;
}


