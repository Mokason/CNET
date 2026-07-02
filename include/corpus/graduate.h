#ifndef CORPUS_GRADUATE_H
#define CORPUS_GRADUATE_H
#include "../nn.h"
#include "../router.h"
#include "../contract/contract.h"
#include "corpus_split.h"
#include "tile_memory.h"

/* Graduate a near-deterministic regularity from the fuzzy memory into two frozen,
   btn_certify-proven, positionally-tagged primitives the router can auto-compose.
   Closes the loop: soft memory -> hard certified units (the non-monolithic proof). */

typedef struct {
    int    ok;                  /* 1 if a 2-step run X->Y->Z was graduated */
    int    vocab;               /* scoped vocab V (BTN dim) */
    int    exemplars;           /* decidable-core bigram exemplars (sources) */
    char   seed[64], mid[64], dst[64];   /* X, Y, Z */
    int    seed_idx, dst_idx;   /* scoped one-hot indices of X and Z */
    int    units;               /* certified units registered */
    int    cert_a, cert_b;      /* btn_certify result (0 = certified) */
    size_t tiles_evicted, mem_before, mem_after;
    double ms_mine, ms_build_certify, ms_register, ms_evict;
} GraduateReport;

/* Owns the two BTNs + contracts + shared exemplar tables; the registry borrows the
   BTN pointers, so keep this alive until after routing, then free it. */
typedef struct {
    BinaryTransformNetwork btn_a, btn_b;
    Contract con_a, con_b;
    double *X, *Y;
    char tag_a[32], tag_b[32], tag_c[32];
    int vocab, seed_idx, dst_idx, valid;
} GraduatedUnits;

/* Mine near-deterministic bigrams from `corpus`, distill+certify two positionally
   tagged units computing the dominant-next map, register them certified in `reg`,
   and evict tiles containing the seed word from `mem`. Returns 0 on a 2-hop
   graduation, -1 otherwise (rep still filled). Caller frees gu after routing. */
int graduate_deterministic(const StrList *corpus, PrimitiveRegistry *reg, TileMemory *mem,
                           int min_count, double min_share, int max_sources,
                           GraduatedUnits *gu, GraduateReport *rep);
void graduated_units_free(GraduatedUnits *gu);
#endif
