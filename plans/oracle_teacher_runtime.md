# Oracle Teacher Runtime — Tier A+B (2026-07-21)

Evolve Oracle into a teach-time governed runtime without rewriting the v2 ABI.

## Tier A
- `cnet_oracle_identity_is_attested` — non-zero SHA-256 + toolchain
- `OraclePolicy.require_attested_to_teach` — refuse register/teach if unattested
- Bind lease: `acquire_oracle_bind` / `unbind` + `require_lease_to_teach`
- Scorecard: `acquire_oracle_scorecard`
- Auto-retire: `retire_unfit_rate` + `acquire_oracle_apply_retire_policy`
- Drain only matches **teachable** oracles (`find_oracle`)
- Seal/unfit counters on teacher entries

## Tier B
- `v2_only_new` — refuse new v1 registers when set
- `require_validator` on v2 register
- Teacher **family** atom via `acquire_oracle_register_v2_family`
- `cnet_oracle_invoke_batch` + `fn_batch_v2` (per-row `CnetOracleResult`; serial fallback)

## Gate
```
make oracle_teacher_runtime   # ORACLE_TEACHER_RUNTIME_PASS
make oracle_v2_test acquire   # regression
```

Hermetic defaults: all policy flags **0** (existing tests unchanged).
Deploy profile can set attested + lease + v2_only as desired.
