#ifndef CNET_BASE_H
#define CNET_BASE_H

#include "cnet_export.h"

/* Unified base ("CNB1"): ONE sealed container replacing per-unit file sprawl.
 *
 * Holds unit payloads (exact CNU1 byte images) in a content-addressed blob
 * table + name->blob references, plus the tag registry (mint-once governance
 * with near-miss refusal and provenance), digest-bound reliability stats, and
 * oracle descriptors. Modular, not monolithic: every unit blob keeps its own
 * CNU1 seal and is independently extractable/verifiable; the container adds a
 * whole-file seal on top (verified BEFORE parsing, unit.c discipline).
 *
 * Honesty rules (CCE weight-store lineage): a dedup "reused" verdict is
 * BYTE-VERIFIED against the stored blob, never trusted on the 64-bit digest
 * alone; a digest collision with different bytes is refused. Dedup fires on
 * re-ingest of an identical unit (idempotence) — CNU1 images embed the unit
 * name, so different-named units never share bytes; no cross-unit sharing is
 * claimed.
 *
 * Trust is replayed, never stored: cnb_load_registry re-certifies every unit
 * against its embedded contract (cheap via the certification cache) and
 * admits via registry_add_certified; a unit that fails replay is skipped.
 *
 * Determinism: insertion-order tables, monotonic mint_seq (NO wall-clock
 * anywhere), so save -> load -> save is byte-identical (regression gate).
 * Spec: docs/superpowers/specs/2026-07-02-unified-base-design.md */

#include <stddef.h>
#include <stdio.h>

#include "nn.h"
#include "router.h"
#include "contract/contract.h"
#include "acquire.h"   /* OracleRegistry / CnetOracleFn for binding */

#define CNB_NAME_MAX 64

typedef struct {
    unsigned long long digest;   /* FNV-1a over bytes */
    unsigned char *bytes;        /* owned; a complete sealed CNU1 image */
    size_t len;
} CnbBlob;

typedef struct {
    char name[CNB_NAME_MAX];
    size_t blob_index;
    unsigned long long behavior_digest;   /* contract_btn_digest at add time */
} CnbUnitRef;

typedef struct {
    char tag[PORT_TAG_MAX];
    char owner[CNB_NAME_MAX];    /* unit that minted it ("" = hand-minted) */
    unsigned long long mint_seq; /* monotonic provenance counter, not time */
} CnbTag;

typedef struct {
    char name[CNB_NAME_MAX];
    char kind[CNB_NAME_MAX];     /* atom, e.g. "builtin", "cce_model" */
    Port input_port;
    Port goal_port;
} CnbOracleDesc;

typedef struct {
    char name[CNB_NAME_MAX];
    unsigned long long bound_digest;  /* behavior digest the evidence describes */
    unsigned long long successes;
    unsigned long long failures;
} CnbStats;

typedef struct CnetBase {
    CnbBlob *blobs;         size_t blob_count,   blob_cap;
    CnbUnitRef *units;      size_t unit_count,   unit_cap;
    CnbTag *tags;           size_t tag_count,    tag_cap;
    CnbOracleDesc *oracles; size_t oracle_count, oracle_cap;
    CnbStats *stats;        size_t stats_count,  stats_cap;
    unsigned long long next_mint_seq;
    /* BTNs (and their name storage — RegistryEntry.name is borrowed, and the
       units[] table may realloc) materialized by cnb_load_registry: the
       registry BORROWS them, the base OWNS them — cnb_free after the registry
       is done (acquire-ledger ownership pattern). */
    BinaryTransformNetwork **loaded;
    char (*loaded_names)[CNB_NAME_MAX];
    size_t loaded_count, loaded_cap;
} CnetBase;

void cnb_init(CnetBase *b);
void cnb_free(CnetBase *b);

/* Whole-file persistence. Save writes <path>.tmp then renames over path
   (previous file survives a crash mid-save). Load verifies the seal BEFORE
   parsing, parses into a temp base and swaps on full success; missing or
   malformed/tampered file -> -1 with *b untouched. */
int cnb_save(const CnetBase *b, const char *path);
CNET_API int cnb_load(CnetBase *b, const char *path);

/* Add a unit (serialized via unit_save_mem). Mints every non-empty port tag
   with owner = unit name — ALL tags are near-miss-checked first, so the add
   is all-or-nothing. Same name + identical bytes -> idempotent 0 with
   *reused_out = 1; same name + different bytes -> -1 (retraining goes through
   the rebuild path's fresh names); tag near-miss -> -1 (see cnb_tag_near_miss
   to identify the collision); digest collision with different bytes -> -1. */
int cnb_add_unit(CnetBase *b, const BinaryTransformNetwork *btn,
                 const Contract *c, int *reused_out);

/* Materialize one unit by name (unit_load_mem; per-blob CNU1 seal verified).
   On success the caller owns btn (btn_free) and c (contract_free). */
CNET_API int cnb_get_unit(const CnetBase *b, const char *name,
                 BinaryTransformNetwork *btn, Contract *c);

/* 1 if a unit with this name exists in the base, else 0. */
int cnb_has_unit(const CnetBase *b, const char *name);

/* ---- tag governance ------------------------------------------------------
   Mint-once with refusal teeth. Exact re-mint is idempotent (returns 1).
   A NEAR-MISS against an existing tag is refused (-1): case-insensitive
   equality, underscore-stripped equality, or Levenshtein distance 1.
   "nibble" vs "nibbel"/"Nibble"/"nibble_" -> refused; "nibble" vs
   "nibble_next" -> a genuinely different tag, fine. */
int cnb_tag_mint(CnetBase *b, const char *tag, const char *owner_unit);
int cnb_tag_lookup(const CnetBase *b, const char *tag);      /* index or -1 */
/* 1 if tag near-misses an existing entry (existing written to existing_out,
   cap CNB existing PORT_TAG_MAX), else 0. Exact match is NOT a near-miss. */
int cnb_tag_near_miss(const CnetBase *b, const char *tag,
                      char *existing_out, size_t existing_cap);
/* One row per tag: name, owner, mint_seq, #units whose ports reference it,
   ORPHAN marker when no unit references it. */
void cnb_tag_audit(const CnetBase *b, FILE *out);

/* ---- digest-bound reliability stats ---------------------------------------
   put records the unit's live counters bound to its CURRENT behavior digest
   (replacing any prior entry for the name). apply restores counters into btn
   ONLY when btn's digest matches the bound digest — the retrainer-invalidates
   rule enforced mechanically; mismatch/missing -> -1, counters untouched. */
int cnb_put_stats(CnetBase *b, const char *unit_name,
                  const BinaryTransformNetwork *btn);
int cnb_apply_stats(const CnetBase *b, const char *unit_name,
                    BinaryTransformNetwork *btn);

/* ---- oracle descriptors ----------------------------------------------------
   Functions cannot persist; intent can. bind resolves each descriptor to a
   runtime fn via the caller's resolver (NULL fn = skip, counted in
   *unbound_out) and registers the bound ones into orc. */
typedef CnetOracleFn (*CnbOracleResolver)(const char *name, const char *kind,
                                          void *rctx);
int cnb_add_oracle_desc(CnetBase *b, const char *name, const char *kind_atom,
                        Port input_port, Port goal_port);
int cnb_bind_oracles(const CnetBase *b, OracleRegistry *orc,
                     CnbOracleResolver resolver, void *rctx,
                     size_t *unbound_out);

/* ---- registry bridge -------------------------------------------------------
   Materialize every unit, re-certify each against its embedded contract, and
   admit via registry_add_certified. Failed replays are skipped (counted in
   *skipped_out), never admitted. Loaded BTNs are base-owned. */
CNET_API int cnb_load_registry(CnetBase *b, PrimitiveRegistry *reg, size_t *skipped_out);

/* ---- migration -------------------------------------------------------------
   Ingest a loose .cnu file (bytes verified by loading them once). Same
   idempotence/refusal semantics as cnb_add_unit. */
int cnb_ingest_cnu_file(CnetBase *b, const char *path, int *reused_out);

/* ---- cross-unit overlap analysis -----------------------------------------
   Read-only "mining-prefetch" scan:
   - For every unit: cnb_get_unit (read-only) then copy *only* its sealed
     Contract's exemplar input+output tables (the data produced by
     mine_from_oracle). Free the heavy BTN+contract immediately.
   - Groups units that have byte-identical full input tables (or full
     input+output tables).
   - Reports unique input tables, largest identical groups, partial row
     sharing outside groups, behavior dups, and full training dups.
   - Also mixes goal into mining sampling (in acquire.c) to reduce
     accidental identical input sets for different goals on the same input.
   Prints a compact summary first. Safe, no side effects. */
void cnb_analyze_cross_unit_overlap(const CnetBase *b, FILE *out);

#endif /* CNET_BASE_H */
