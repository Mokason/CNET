# Chronological allocator experiment — evidence-first development

Owner request: proceed with the chronological experiment proposed after the
failed allocator gate. Baseline `9e74a7e`; isolated existing feature worktree.
No deployment, service restart, source-policy migration, activation or push.

## Dependency-ordered experiment

1. Audit existing CNET request evidence read-only. Establish chronology, capture
   completeness, origin and stable task identity before choosing model inputs.
   Keep prompts/answers out of reports. Record malformed and missing data;
   never discard bad rows and call the remainder independent confirmation.
2. If an eligible trace exists, freeze a development-only replay protocol with
   past-only features, complete decision episodes, common budgets and strong
   controls. Verify attainable +.05 absolute coverage headroom before fitting.
   Keep all existing exposed datasets outside any new confirmation split.
3. Only after that feasibility check: freeze training, checkpoint selection and
   fresh whole-episode confirmation; run bounded AMD training. The original
   gain/confidence/non-regression floors and production refusal remain intact.

If existing evidence is insufficient, stop before fitting. Identify the exact
missing capture boundary and ask the owner which real ingress to instrument or
which external trace to supply; do not silently repoint a live service or turn
test traffic into user demand. No new broad telemetry/PII collection is inferred.

## First slice: bounded metadata audit

Add `allocator_chronology_audit.py`, its tests and a CPU-only Make target in the
existing experiment directory. This diagnostic is **not** a new promotion gate
or an experiment data format. It reads a fixed maximum 64 MiB regular-file
snapshot, maximum 64 KiB per physical line and a 10-second BOOTTIME budget.
No symlinks at the final path, special files or non-owner files; a changing
file/path refuses. Long/invalid/duplicate-key/nonfinite JSON and bad timestamps
are counted without printing any payload. Input hashes are identity, not source
authenticity. No input repair, writes, network calls, subprocesses or GPU jobs.
File read side effects such as filesystem atime are not a content mutation.

Report object/line counts, exact timestamp bounds/order violations,
presence of caller/episode/task fields, and presence of answer/target fields.
`source`/`verified` are response metadata, never proof of external request origin
or acceptable training labels. Legacy audits always withhold training eligibility:
field presence alone cannot prove capture completeness, custody or independence.
No raw prompts, answers, session IDs or per-request identifiers enter git.

Verification: RED contract tests first; valid and hostile parser fixtures, file
identity/limits and privacy checks; read-only live audit with recorded input hash;
Astra-only author-separated evidence/code review; focused CPU regressions and
documentation. Missing skill reference files are handled using the main skill
instructions and repository-specific gates, not invented instructions.

## Status

- [x] Legacy evidence audit, tests and bounded live run.
- [x] Legacy source eligibility independently reviewed: ineligible.
- [ ] Trusted capture choice resolved with owner; no live capture deployed.
- [ ] Eligible development replay and headroom check.
- [ ] Bounded training and genuinely fresh confirmation, if justified.

Current discoveries: schema-2 demand is aggregate-only (256 rows, 515 requests,
466 misses in the observed private deployment). The final-base usage JSONL is
aggregate verification data. The CNET-minimal miss JSONL is about 23.9 MB,
contains raw request/response material and malformed JSON. Its `source` values
describe response paths, not caller origin. Service journals sampled for the
two daemon units contain lifecycle text, not replayable typed request events.
The configured accounting-log file is absent. No real request contents have
been displayed or committed by this audit.

The complete bounded scan observed 90,557 physical lines in 23,901,122 bytes,
three invalid JSON lines and 1,606 objects without valid timestamps. None of
the inspected caller-origin, request-ID, episode-ID or label-receipt field names
was present. Source review also found miss-only censoring, benchmark emission
without origin tags, and typed rows that can contain CNET's own output. These
are structural exclusions, not permission to repair or relabel the old data.
See the [dated evidence report](../result/cnet_chronological_allocator_20260907.md).

Remaining prerequisite: designate a trusted real request ingress or supply a
trace with independently established capture custody. Then capture before query
rewrites/actions, include successes and misses, separate tests/agents/retries,
bind source/catalog versions, and join independently verified outcomes by event
ID. Current `PEER` text and a shared user socket do not authenticate human origin.
Changing a live adapter's authority/capture contract needs an explicit owner
choice; it is not inferred from the request to run this isolated experiment.
