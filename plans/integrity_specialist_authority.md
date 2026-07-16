# Integrity Slice — Specialist Admission Authority

**Place:** `include/router.h`, registry internals, Specialist admission, and tests.

**Dilemma:** exported `registry_add` can create uncertified planner-visible entries while default registries permit uncertified planning.

**Consequence:** callers can bypass the claimed one Specialist admission door.

**Systematic component:** source/API boundary and planner eligibility. **Noise:** none.

## TDD

1. Add an authority test proving default production admission cannot plan an unchecked entry.
2. Rename the low-level append to an explicitly internal/unchecked operation and remove it from the stable exported API.
3. Route supported production callers through `specialist_admit`/`registry_add_certified`.
4. Give tests an explicit fixture-only unchecked helper rather than weakening production defaults.
5. Add a static source/export gate preventing public reintroduction.
6. Focused marker: `SPECIALIST_AUTHORITY_PASS`.

## Verified Closure

- Production registries initialize certification-required; supported production admission remains `specialist_admit`.
- The source bypass audit refuses production calls to fixture-only unchecked admission.
- `registry_init`, `registry_add`, and `registry_add_certified` carry hidden ELF visibility in the unified DLL build; `specialist_admit` remains exported. `tests/audit_admission_abi.sh` verifies the actual dynamic symbol table.
- `make specialist_authority` combines behavior, static-source and ELF-ABI checks and emits `SPECIALIST_AUTHORITY_PASS`.
