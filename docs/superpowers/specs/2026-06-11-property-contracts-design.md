# Property Contracts (Equational Laws) — Design

Date: 2026-06-11
Status: Implemented 2026-06-11 (TDD; `make test` + `make property` green). Deviations: the perturbation constant for violation tests/demo is 10.0, not the planned 4.0 — a +4.0 nudge on the trained nets was rounded away by canonicalization (the law spuriously held); the constant must clear the canonicalization margin, documented at both sites.
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

Exemplar contracts certify one transform pointwise. They cannot state
RELATIONS between primitives: that split inverts combine, that two chains
compute the same function, that a retrained implementation still satisfies
the algebra its neighbors depend on. Such laws are checkable without
knowing the function pointwise — and they are the regression net for
retraining: a broken combine violates `split ∘ combine = identity` even if
nobody ever wrote its exemplar table. Deferred from the machine-checkable
contracts design as the property layer.

## Decision: properties are equations between named chains

A property is a NAMED EQUATION defined by data: a typed source signature
plus two chains of primitive names — LHS (>= 1 step) and RHS (0 steps =
identity on the sources). It holds iff, for EVERY canonical member of the
enumerated source domain, strictly executing the LHS chain equals strictly
executing the RHS chain.

Chain semantics: step 1 consumes the concatenated source vector; each
step's whole output vector feeds the next step's whole input vector.

Rejected: hardcoded law kinds (each new shape needs core code — knowledge
must stay data); a full expression language with parallel/tuple application
(a term interpreter in C; the chain form covers every law today's library
can state).

Names resolve against a registry at check time: the property is a claim
about a NAMED library ("this registry's split inverts this registry's
combine"), so the same file checks retrained or swapped implementations.

This slice ships the mechanism only (checker + tests + demo). Integration
knobs — registries demanding property suites, certification requiring laws
— layer on later, per the project's mechanism-first staging.

## Property model (new module: include/property.h, src/property.c)

```c
#define PROPERTY_NAME_MAX 64   /* atoms over [A-Za-z0-9_], like tags */
#define PROPERTY_MAX_SOURCES BTN_MAX_INPUT_PORTS
#define PROPERTY_MAX_STEPS 8

typedef struct {
    char name[PROPERTY_NAME_MAX];
    Port sources[PROPERTY_MAX_SOURCES];
    size_t source_count;
    char lhs[PROPERTY_MAX_STEPS][PROPERTY_NAME_MAX];
    size_t lhs_len;                       /* >= 1 */
    char rhs[PROPERTY_MAX_STEPS][PROPERTY_NAME_MAX];
    size_t rhs_len;                       /* 0 = identity on the sources */
} Property;

int  property_save(const Property *p, const char *path);
int  property_load(Property *p, const char *path);   /* fixed-size: no free */

typedef struct {
    size_t inputs;     /* domain members checked */
    size_t held;       /* both sides clean and equal */
    size_t violated;   /* unclean handoff on either side, or mismatch */
} PropertyReport;

int property_check(const Property *p, const PrimitiveRegistry *reg,
                   size_t max_samples, PropertyReport *report);
```

On disk (`<name>_property.txt`):

```
CNET_PROPERTY 1
<name>
SOURCES n
PORT family field_width field_count tag|-    (xn)
LHS k
<primitive name>                              (xk, one per line)
RHS m
<primitive name>                              (xm; m may be 0)
```

property_load validates: magic + version, atom names everywhere, source
count in [1, PROPERTY_MAX_SOURCES], chain lengths in bounds (LHS >= 1),
known families, widths/counts nonzero, tags via port_set_tag. Malformed
input refuses with *p untouched.

## Checking: resolve, type-gate, replay

property_check runs three stages; report is zeroed first and rc is 0 iff
the replay stage ran with zero violations.

1. RESOLVE: every chain name must match a registry entry (linear scan by
   strcmp; first match wins, consistent with registry semantics).
   Unresolvable name -> -1, no execution.
2. STATIC TYPE GATE (before any forward pass):
   - sources -> chain[0]: port COUNTS equal and position-wise
     port_compatible(source[i], step_input[i]);
   - step j -> step j+1: same rule over output ports vs input ports;
   - LHS final vs RHS final (RHS final = the source ports when rhs_len is
     0): counts equal and REPRESENTATION equal per position (family,
     field_width, field_count). Tags are NOT required to match across the
     equation — the law equates values, not labels.
   Any mismatch -> -1.
3. REPLAY: enumerate the canonical source domain (deterministic order,
   RAW refused, over-max_samples refused — the same rules as
   consolidation/emission). For each member, run BOTH chains with strict
   validate-then-canonicalize at every handoff (input to each step
   validated against its input ports per position, raw output validated
   against EVERY output port and canonicalized — the executor bar). An
   unclean handoff or raw output on EITHER side, or a final canonical
   mismatch, counts that input as a violation (a law is not satisfied
   where a chain cannot even produce a clean value). held/violated
   accumulate; checking never aborts early — the report shows how broken
   a broken law is.

The chain runner is built directly on btn_forward + port_validate /
port_canonicalize. btn_forward never records reliability evidence (only
the executors do), so property checking is stateless by construction — no
counter snapshot/restore needed, same hygiene outcome as certification.

## Shared-unit widening (plan_table)

The domain-enumeration machinery (struct Domain + domain_build /
domain_write / domain_free) is currently static in src/plan_table.c. It is
exported as `PlanDomain` + `plan_domain_build` / `plan_domain_write` /
`plan_domain_free` (rename-to-export, bodies unchanged; plan_table_build
keeps using them internally). property_check streams one enumeration pass,
running both chains per input — no tables are allocated. The existing
consolidation/certification suites pin that the widening changes nothing.

## Tests (TDD, tests/test_property.c)

- Property file round-trip (multi-source signature, tags, both chains,
  RHS 0); malformed refusals (wrong magic, bad atom, LHS 0, counts out of
  bounds, unknown family).
- Hermetic inverse pair: tiny trained `pack` ([bit,bit] -> pair) and
  `unpack` (pair -> [bit,bit]); law `unpack(pack(a,b)) = identity` holds
  4/4 and law `pack(unpack(p)) = identity` holds 4/4.
- Violation: perturb one of unpack's weights; the first law now reports
  violated >= 1 with held + violated == 4 and rc -1 (the checker catches a
  broken retrain).
- Static gates: unresolvable name; port-count mismatch; representation
  mismatch between LHS final and RHS final; RAW source refusal; over-cap
  refusal (max_samples below the domain size); empty-LHS refusal.
- Identity semantics pinned: rhs_len 0 compares against the canonical
  source vector itself.
- Statelessness: members' reliability counters unchanged by a check.

## Demo (tests/property_demo.c, make property)

Loads the real frozen split + combine, registers them, then:
(1) authors both laws IN CODE via property_save —
`split_inverts_combine_property.txt` (sources [nibble_value x2], LHS
[combine, split], RHS identity) and `combine_inverts_split_property.txt`
(sources [byte_value], LHS [split, combine], RHS identity) — reloads them
via property_load (round-trip on real files), and checks both: HOLD,
256/256 each. (2) The regression story: a second combine instance is
loaded and one hidden-output weight is perturbed in memory; registered in
a fresh registry under the same names, the first law now reports real
violations — "a broken retrain cannot hide from the algebra". PASS/FAIL
banner + exit code, house demo voice.

## Out of scope (YAGNI)

- Parallel/tuple application in chains (needed to state
  hex_pair_to_byte == combine over (hex_value x hex_value) — the known
  next step in law expressiveness; revisit when a law needs it).
- Injectivity/bijectivity and other non-equational law shapes (different
  evaluation: uniqueness across the domain, not pointwise equality).
- Property suites bound to registries or required by certification.
- Sampled checking for non-enumerable domains (the cap refuses honestly).
- Conditional / quantified laws.
