# Machine-Checkable Contracts (Behavioral Certification) — Design

Date: 2026-06-11
Status: Implemented 2026-06-11 (TDD; `make test` + `make certify` green). Deviations: training targets are soft (0.9/0.1), so nn_demo's emit_contract canonicalizes output rows before saving (the contract is the canonical mapping); contract_load additionally bounds declared dimensions against size_t overflow (hostile-file hardening).
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

The 2026-06-08 contract layer types the representation on the wire; semantic
tags (2026-06-11) name the meaning — but the name is trust-based. Nothing
checks that a primitive DOES what its tags claim: a net that outputs
well-formed nibbles with the wrong mapping wears "nibble_value" as
legitimately as hex_value does. Reliability evidence cannot catch it either
(it scores well-formedness, not correctness). Flagged since the composition
thesis: meaning must be machine-checkable, or composition guarantees only
shape.

## Decision: contracts are named transform specs, certified by replay

A contract is a NAMED FUNCTION defined by data: a full port signature plus
an exemplar table over canonical values. Primitives CLAIM a contract; a
certifier replays the exemplars through the frozen net and grants or denies.
Meaning becomes shared, first-class, domain-agnostic data: tags turn from
assertions into earned certificates, alternative implementations are
interchangeable once certified, and proven plans can emit the contract of
their composite behavior.

Rejected: tag-level value-set definitions (checks values, not mappings —
misses the actual trust gap); per-primitive private test vectors (meaning
stays siloed; nothing is shared or earned).

Certification runs on demand and is never persisted: at toy scale a replay
is 16–256 forwards, so caching buys nothing and a certificate file would
reintroduce the staleness problem stats sidecars already solve carefully.
Retrained weights simply pass or fail the next replay honestly.

## Contract model (new module: include/contract.h, src/contract.c)

```c
typedef struct {
    char name[CONTRACT_NAME_MAX];  /* 64; atom over [A-Za-z0-9_], like tags */
    Port input_ports[BTN_MAX_INPUT_PORTS];
    size_t input_port_count;
    Port output_ports[BTN_MAX_OUTPUT_PORTS];
    size_t output_port_count;
    double *inputs;     /* exemplar_count x (sum of input port totals) */
    double *outputs;    /* exemplar_count x (sum of output port totals) */
    size_t exemplar_count;
} Contract;

int  contract_save(const Contract *c, const char *path);
int  contract_load(Contract *c, const char *path);
void contract_free(Contract *c);
```

On disk (`<name>_contract.txt`):

```
CNET_CONTRACT 1
<name>
INPUTS n
PORT_IN family field_width field_count tag|-      (xn)
OUTPUTS m
PORT_OUT family field_width field_count tag|-     (xm)
EXEMPLARS k
<in values> <out values>                          (xk, canonical 0/1)
```

Multi-input rows concatenate port slices in port order; multi-output rows
concatenate every output segment (so split-style primitives are
certifiable). Values must be canonical members of their ports' domains;
contract_load validates this and refuses malformed files.

## Certification

```c
typedef struct { size_t exemplars; size_t passed; size_t failed; } CertifyReport;
int btn_certify(BinaryTransformNetwork *btn, const Contract *c,
                CertifyReport *report);   /* report optional */
```

Two gates, in order:

1. SIGNATURE: the primitive's port arrays must match the contract's exactly
   — count, family, field_width, field_count, AND tag. Certification is
   what entitles a primitive to wear the contract's tags; an absent or
   different tag fails. (Exactness, not port_compatible wildcarding: a
   claim is specific.)
2. BEHAVIOR: every exemplar replays through the frozen net. The raw output
   must be in-domain on EVERY output port (the same bar the executors
   score) and must canonicalize to the exemplar's output exactly. All
   exemplars must pass — a spec is a spec. The report carries counts for
   diagnostics; recording stops nothing.

btn_certify is stateless and read-only apart from the forward passes (which
touch last_output only — reliability counters are NOT bumped: certification
is not deployment experience, same principle as consolidation's teacher
hygiene).

## Teeth: registry flag + planner knob

- `RegistryEntry` gains `int certified;` — plain registry_add leaves it 0.
- `int registry_add_certified(PrimitiveRegistry *reg,
   BinaryTransformNetwork *btn, const char *name, const Contract *c);`
  runs btn_certify and REFUSES registration (-1, nothing added) on failure.
- `PrimitiveRegistry` gains `int require_certified;` (registry_init zeroes
  it — default off, old behavior by construction). When set, route_plan and
  dag_plan skip uncertified entries entirely, so a returned plan is
  certified end-to-end. Sources are caller data, not claims — unaffected.
- No function signatures change; both knobs follow the strict-mode
  precedent (opt-in, zero-init = legacy behavior).

## Emission: both sources

1. Trainer-authored: nn_demo already holds every BTN's full truth table.
   After each btn_save it also writes `<name>_contract.txt` from the
   training data (name, the primitive's ports, the table). Every shipped
   primitive gets its spec; the spec becomes separable from the trainer.
2. Plan-derived: `contract_from_route(plan, name, max_samples, out)` and
   `contract_from_dag(plan, sources, n_sources, name, max_samples, out)`
   replay a proven plan as a STRICT teacher over its enumerated canonical
   domain and build the exemplar table from the kept rows (teacher-aborted
   inputs excluded). Same guards and hygiene as consolidation: RAW or
   over-cap domains refused, every source consumed exactly once (DAG),
   member reliability counters snapshotted and restored. Unlike
   consolidation, a 1-step/1-primitive plan is allowed — emitting the spec
   of a single primitive is legitimate.

Implementation note: the domain-enumeration and strict-teacher machinery is
currently static in consolidate.c. It moves to a shared unit
(include/plan_table.h + src/plan_table.c) used by both consolidate.c and
contract.c — a behavior-preserving refactor, pinned by the existing
consolidation tests staying green and byte-identical demo behavior.

The loop this closes: plans -> chunks (consolidation) -> contracts
(emission) -> certified replacements (certification). A chunk distilled
from a teacher plan is certifiable against the contract emitted from that
same plan, by construction.

## Tests (TDD, tests/test_certify.c)

- Contract file round-trip (multi-input and multi-output signatures, tags,
  exemplar values); contract_load refuses malformed/non-canonical files.
- btn_certify: correct tiny net passes (report k/k); one tampered exemplar
  fails (report shows the miss); wrong tag fails; missing tag fails; shape
  mismatch fails; reliability counters unchanged by certification.
- registry_add_certified: refuses the imposter (nothing registered),
  accepts the certified net (entry flagged).
- require_certified: with the knob on, planners exclude uncertified
  entries (a worse-scoring certified alternative wins; an uncertified-only
  registry yields no plan); with the knob off, behavior is byte-identical
  to today (existing tests pin this).
- contract_from_route / contract_from_dag: emitted table matches the
  teacher's input/output pairs; guards (RAW, cap, unconsumed source)
  refuse; member stats restored.
- Punchline: consolidate the combiner DAG into a chunk, emit the contract
  from the same teacher plan, btn_certify(chunk, contract) passes.

## Demo (tests/certify_demo.c, make certify)

Loads the real frozen primitives plus their nn_demo-emitted contracts.
(1) Certifies each against its own contract — prints a verdict table.
(2) Imposter theater: increment claims hex_value's contract —
registry_add_certified refuses (signature gate), and a tampered contract
shows the behavior gate firing. (3) Sets require_certified, plans
hex_digit -> incremented value end-to-end, executes 16/16 — a plan whose
every member is certified. (4) Emits the contract of the
combine(hex_value, hex_value) plan and certifies the hex_pair_to_byte
chunk against it — the chunk wears a machine-checked spec.

## Out of scope (YAGNI)

- Algebraic/property contracts (split ∘ combine = identity, bijectivity).
  The exemplar machinery is the substrate a property layer would replay
  through; revisit when a domain needs laws exemplars cannot state.
- Sampled certification for non-enumerable domains (the cap refuses
  honestly, as in consolidation).
- Certificate persistence or weight-hash binding.
- Contracts for the scalar NeuralNetwork type.
- A global registry of contract names / versioning.
