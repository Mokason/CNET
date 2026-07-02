#ifndef CONTRACT_H
#define CONTRACT_H

#include <stddef.h>

#include "../nn.h"
#include "../router.h"

/* A machine-checkable contract: a NAMED transform defined by data -- a full
   port signature plus an exemplar table over canonical values. Primitives
   CLAIM a contract; btn_certify replays the exemplars through the frozen
   net and grants or denies. Certification runs on demand and is never
   persisted (a replay is cheap; a certificate file would reintroduce
   staleness). */

#define CONTRACT_NAME_MAX 64  /* atom over [A-Za-z0-9_], like port tags */

typedef struct {
    char name[CONTRACT_NAME_MAX];
    char parent[CONTRACT_NAME_MAX]; /* optional: name of the parent (layer) contract, or "" */
    Port input_ports[BTN_MAX_INPUT_PORTS];
    size_t input_port_count;
    Port output_ports[BTN_MAX_OUTPUT_PORTS];
    size_t output_port_count;
    double *inputs;        /* exemplar_count x (sum of input totals) */
    double *outputs;       /* exemplar_count x (sum of output totals) */
    size_t exemplar_count;
    int owns_data;         /* nonzero -> contract_free releases the tables */
    int seal_verified;     /* 1 = loaded from a v2 file whose SEAL matched the
                              recomputed content digest (tamper-evident);
                              0 = authored in-process or legacy v1 file */
} Contract;

/* A frozen (const) contract descriptor for embedding in read-only data.
   Used by contract_init_frozen to create a runtime Contract from static data. */
typedef struct {
    const char *name;
    const char *parent;              /* optional parent name, or NULL */
    size_t input_port_count;
    size_t output_port_count;
    const Port *input_ports;         /* array of input_port_count ports */
    const Port *output_ports;        /* array of output_port_count ports */
    const double *inputs;            /* borrowed exemplar input table */
    const double *outputs;           /* borrowed exemplar output table */
    size_t exemplar_count;
} FrozenContractData;

/* Initialize a Contract from a FrozenContractData (borrowed tables, owns_data=0).
   Returns 0 on success, -1 if fd is NULL or name is empty. */
int contract_init_frozen(Contract *c, const FrozenContractData *fd);

/* Set the parent name on an existing contract. Returns 0. */
int contract_set_parent(Contract *c, const char *parent_name);

/* Authoring: fill *c from a primitive's port signature and a BORROWED
   exemplar table (typically the training data). c->owns_data = 0; do not
   contract_free the tables' owner before saving. Values must be canonical.
   Returns 0, or -1 on a bad name/empty table. */
int contract_init_borrowed(Contract *c, const char *name,
                           const BinaryTransformNetwork *btn,
                           const double *inputs, const double *targets,
                           size_t exemplar_count);

/* Persist / restore. contract_save writes "CNET_CONTRACT 2": the v1 body
   plus a trailing "SEAL <fnv64hex>" over the semantic content (name, ports,
   exemplar tables) — tamper-evident. contract_load accepts v1 (legacy,
   seal_verified=0) and v2 (the SEAL must match the digest recomputed from
   the loaded content, else the file is REFUSED; seal_verified=1).
   contract_load allocates owned tables (owns_data = 1) and validates:
   known families, name and tag atoms, every value exactly 0.0 or 1.0 and
   every port slice canonical (port_validate). Returns 0, or -1 on
   malformed/tampered input (*c untouched). */
int contract_save(const Contract *c, const char *path);
int contract_load(Contract *c, const char *path);
void contract_free(Contract *c);

/* ---- content identity ----------------------------------------------------
   Deterministic FNV-1a digests. contract_btn_digest covers BEHAVIOR only:
   counts, port signatures, ternary settings, and the live weight/bias
   entries (the same set btn_save persists) — NOT learning_rate, capacity,
   activations, or the runtime evidence counters (evidence accrual must not
   invalidate a certification). contract_content_digest covers the spec the
   file round-trips: name + port signatures + exemplar tables (not parent,
   which is unpersisted lineage metadata). */
unsigned long long contract_btn_digest(const BinaryTransformNetwork *btn);
unsigned long long contract_content_digest(const Contract *c);

/* ---- certification cache ---------------------------------------------------
   btn_certify memoizes verdicts in-process, keyed by (btn digest, contract
   digest): re-certifying unchanged content returns the recorded verdict +
   report without replaying the exemplars. Any weight or exemplar change
   alters the key and forces a fresh replay, so a hit is exactly the verdict
   a replay would produce. Certification is still never persisted across
   processes. Single-threaded, like the rest of the BTN stack. */
void contract_cache_stats(size_t *hits, size_t *misses);
void contract_cache_reset(void);

/* ---- certification audit ---------------------------------------------------
   registry_add_certified (and the heal / shadow-promote paths) record the
   certified btn's content digest on the entry. registry_audit_certified
   recomputes each certified entry's digest: an entry whose weights changed
   since certification is DEMOTED (certified=0, state=PRIM_RESET) — a stale
   certificate is never trusted. Returns the number demoted. */
size_t registry_audit_certified(PrimitiveRegistry *reg);

typedef struct {
    size_t exemplars;
    size_t passed;
    size_t failed;
    double min_margin;  /* worst-case output headroom over all exemplars (see
                           port_margin); filled only when a report is requested */
} CertifyReport;

/* Certification: (1) SIGNATURE -- the primitive's ports must match the
   contract's exactly: count, family, field_width, field_count AND tag
   (certification is what entitles a primitive to wear the contract's
   tags). (2) BEHAVIOR -- every exemplar replays through the frozen net;
   the raw output must be in-domain on EVERY output port and canonicalize
   to the exemplar's output exactly. All exemplars must pass. Stateless;
   reliability counters are not touched. Returns 0 (certified) or -1;
   report (optional) carries counts either way. */
int btn_certify(BinaryTransformNetwork *btn, const Contract *c,
                CertifyReport *report);

/* Robustness certification: btn_certify's exact-replay gates PLUS a headroom
   bar -- every exemplar's worst output margin must be >= margin_floor (in
   port_margin's native units). Raises the bar from "not ambiguous" to "has
   margin to spare", turning the exact-replay contract into a robustness
   contract with no new machinery. margin_floor 0 reduces to btn_certify.
   Returns 0 (certified with headroom) or -1; report (optional) carries
   min_margin either way. */
int btn_certify_robust(BinaryTransformNetwork *btn, const Contract *c,
                       double margin_floor, CertifyReport *report);

/* Compare whether `candidate` is a strictly better implementation than `active`
   for the same contract. Returns 1 if candidate is better, 0 if not better,
   and -1 on invalid inputs. Comparison first requires both to certify, then
   uses average squared error on contract exemplars (lower is better), with
   reliability as a tiebreak when MSE is equal. */
int contract_better_if(const Contract *c, const BinaryTransformNetwork *active,
                       const BinaryTransformNetwork *candidate);

/* Swap-in replacement if and only if `candidate` is better than `*active` for
   the same contract. Returns 1 if swapped, 0 if not swapped, and -1 on
   invalid inputs. */
int contract_swap_if_better(const Contract *c, BinaryTransformNetwork **active,
                            BinaryTransformNetwork *candidate);

/* Certify-then-register: refuses certification entirely (-1, nothing
   added) when btn_certify fails, and also rejects a same-named
   incumbent-worse candidate. If a same-named primitive already exists,
   the candidate replaces it only when `contract_better_if` prefers the new
   candidate; otherwise it is rejected. On successful append/replacement,
   the entry's certified flag is set. */
int registry_add_certified(PrimitiveRegistry *reg,
                           BinaryTransformNetwork *btn,
                           const char *name, const Contract *c);

/* Evidence-gated hot-swap: promote shadow `shadow_name` to replace the active
   it shadows, iff it (1) has accrued >= min_evidence outcomes, (2) has
   reliability >= the active's, and (3) certifies `contract`. On promotion the
   shadow becomes a normal certified FROZEN primitive (shadow_of cleared) and
   the old active is demoted to PRIM_RESET. Returns 1 if promoted, 0 if not
   ready, -1 on bad args / unknown names. */
int shadow_promote_if_ready(PrimitiveRegistry *reg, const char *shadow_name,
                            const Contract *contract, size_t min_evidence);

/* Wake pass: attempt to repair the RESET primitive `name` against `contract`.
   If it has no LABELED retrain exemplars, it stays RESET (we never retrain
   without a verified target) and 0 is returned. Otherwise it is retrained
   (fixed-epoch btn_train) on the contract's exemplars UNION its labeled queue,
   then re-certified against `contract`. On a passing btn_certify it is restored
   to PRIM_FROZEN (certified = 1), its labeled queue is cleared, and its
   reliability counters are reset (the weights changed); 1 is returned. On a
   failing certify it stays PRIM_RESET and 0 is returned. The ONLY path back to
   FROZEN is a passing certify. Returns -1 on bad args / unknown name / OOM. */
int registry_heal(PrimitiveRegistry *reg, const char *name,
                  const Contract *contract, size_t max_epochs);

/* Emission: replay a proven plan as a STRICT teacher over its enumerated
   canonical domain (same guards and member-stats hygiene as
   consolidation; teacher-aborted inputs are excluded) and build the
   contract of the composite behavior. Unlike consolidation a 1-step /
   1-primitive plan is allowed. Output tables are owned (owns_data = 1).
   Returns 0, or -1 on refusal (RAW or over-cap domain, no labeled rows,
   bad name, unconsumed DAG source). */
int contract_from_route(const RoutePlan *plan, const char *name,
                        size_t max_samples, Contract *out);
int contract_from_dag(const DagPlan *plan, const DagSource *sources,
                      size_t n_sources, const char *name,
                      size_t max_samples, Contract *out);

/* Emission for a multi-root circuit: the contract's output signature is
   the roots' projected ports in goal order. Every source must be
   referenced exactly once (sharing-aware count). */
int contract_from_circuit(const CircuitPlan *plan, const DagSource *sources,
                          size_t n_sources, const char *name,
                          size_t max_samples, Contract *out);

#endif




