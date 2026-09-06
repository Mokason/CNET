# Distillation proposals

[tools/cnet_distill_slice.c](../tools/cnet_distill_slice.c) prepares domain
proposals from explicit queries, query files or distinct miss-log queries.
It stops at proposal output: it does not train a student, seal a capsule or
install a live skill.

```sh
make distill_slice
```

The tool accepts `--domain`, repeated `--query`, `--from-file`,
`--from-miss-log`, `--out`, `--dry-run` and optional `--teacher`.
Even the dry-run proposal writes output. It suppresses teacher HTTP, not
the Tier-A probe, which can contact an already-running daemon through a peer
client. Use a private worktree/output and explicitly isolated probe; do not
treat `make distill_slice` as a service-independent offline smoke test.
Teacher mode can send query text to its configured HTTP endpoint.

## Evidence boundary

Each proposal contains `PROPOSE.json`, rows and candidate gold rows.
`auto_cert=false` and pending status are mandatory distinctions.
A file named `gold_rows.jsonl` contains candidates for review, not already
verified gold.

The probe attempts to identify queries already answered by Tier A. Only the
explicit `CNET_FRONT_DOOR_BIN` branch supplies residual-disabled environment
flags; other branches have different behavior. Unavailable probes do not stop
proposal generation, and the Make gate accepts `collapse_checked=0`.

Even `anti_collapse_checked=true` is not successful per-query verification:
it records executable availability, not checked exit/completion status.
A disconnected peer can still appear checked; availability of only
`bin/roe_front_door` can mark true without dispatching it. Neither that field
nor PASS proves that Tier-A exclusion actually ran successfully.

Do not train or admit unchecked proposals until independent provenance and
Tier-A exclusion have been established. `--no-collapse-check` is a test
bypass, not production authorization. This unresolved proposal boundary is
documented, not silently described as fail-closed.

## From evidence to a capability

External teacher data, user corrections or verified tool results can feed the
existing typed [capsule teaching path](CAPSULE_CORE.md). A language pack,
table/brick and CNU1 capsule have different formats and acceptance rules.
An MTK tensor delta is not any of those packages.

The current daemon does not automatically call its disabled legacy residual
branch on a miss. Use the explicitly configured acquisition/teacher workflow
described in [TEACH_PATH.md](TEACH_PATH.md). Domain shaping and admission
remain separate from this proposal tool.
