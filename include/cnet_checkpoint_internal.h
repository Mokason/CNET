#ifndef CNET_CHECKPOINT_INTERNAL_H
#define CNET_CHECKPOINT_INTERNAL_H
/* Shared private startup/health predicate; no public ABI or format change. */
#include "hybrid_ai.h"
#include "contract/contract.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const double *values; size_t width; } CheckpointRow;
static int checkpoint_row_compare(const void *a, const void *b) {
    const CheckpointRow *x = a, *y = b;
    return memcmp(x->values, y->values, x->width * sizeof(double));
}

/* Sidecar rows may be reordered or a subset, but cannot widen the sealed
 * evidence. Sort borrowed row pointers once: no quadratic reservoir scan. */
static int checkpoint_rows_bind(const HybridAi *h, const char *name,
                                const Contract *ct, size_t width) {
    if (!ct->inputs || !ct->exemplar_count || !width ||
        ct->exemplar_count > SIZE_MAX / sizeof(CheckpointRow)) return 0;
    CheckpointRow *rows = malloc(ct->exemplar_count * sizeof *rows);
    if (!rows) return 0;
    for (size_t i = 0; i < ct->exemplar_count; i++)
        rows[i] = (CheckpointRow){ct->inputs + i * width, width};
    qsort(rows, ct->exemplar_count, sizeof *rows, checkpoint_row_compare);
    int valid = 1;
    for (size_t i = 0; i < h->coverage_count && valid; i++) {
        const HybridCoverage *g = &h->coverage[i];
        if (!g->active || strcmp(g->unit, name)) continue;
        if (!g->rows || !g->n_rows || g->in_dim != width) { valid = 0; break; }
        for (size_t j = 0; j < g->n_rows; j++) {
            CheckpointRow key = {g->rows + j * width, width};
            if (!bsearch(&key, rows, ct->exemplar_count, sizeof *rows,
                         checkpoint_row_compare)) { valid = 0; break; }
        }
    }
    free(rows);
    return valid;
}
#endif
