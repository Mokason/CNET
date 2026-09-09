# Frozen task-proposal evaluation

This evaluates the bounded offline grammar behind [verified tasks](VERIFIED_TASK_CORE.md),
not answer generation, learned semantics or real-user performance. Only the
actual managed parser is invoked. No daemon, ledger, source import, learning,
network service or model provider is started by the evaluator.

## Populations and gates

The [frozen protocol](../plans/cnet_paraphrase_evaluation_20260909.md) declares
two separately authored synthetic collections of 128 requests: 80 typed ready
inputs (40 per operation), 24 clarifications and 24 OOD abstentions. Authors
received the behavior contract without parser code, regression examples or
outputs. Authors cross-reviewed labels before freezing; the implementer used
only qualification for repair and kept confirmation unopened until candidate
freeze. This is procedural same-model separation, not third-party human or IID
evaluation. All records remain synthetic and ineligible for training.

Ready scoring requires the exact original input byte and operation, never an
answer value. Wrong ready proposals must be zero; exact ready must reach 90%
overall and 85% per operation, clarification 90%, and OOD abstention 95%.
Conservative refusals count as misses. All rows, category/family denominators,
hashes and failed gates are retained. Certification floors are unchanged.

Both original collections are now exposed development data: the first
confirmation failed ready/clarification floors, and its misses informed the
[follow-up repair](../plans/cnet_paraphrase_acceptance_followup_20260909.md).
Their repaired scores are not fresh validation. The follow-up suite adds a
separately authored and reviewed 128-case confirmation, with the same quotas
and floors and no exact request overlap with either original collection. That
follow-up also failed ready/OOD floors and is now exposed. `round3` provides
another separately authored, reviewed and pinned 128-case confirmation, with
the same quotas/floors and no exact overlaps with the three exposed collections.
Each confirmation may be used once against a frozen candidate; adapting
to its results requires another independently authored confirmation population.
Reproduction is allowed but is not another independent sample. The CLI does
not enforce a durable once-only experiment counter; that is a documented
freeze/review discipline.

## Build and integrity tests

In a private development worktree with the normal .NET prerequisites:

```sh
dotnet build tools/task_paraphrase_eval/TaskParaphraseProbe.csproj \
  --artifacts-path .artifacts/task-verified --nologo
python3 tests/test_task_paraphrase_eval.py

python3 -m trace --count --missing --summary \
  --coverdir /tmp/cnet-paraphrase-python-coverage \
  --module unittest discover -s tests -p test_task_paraphrase_eval.py
```

Use unittest discovery for tracing: directly tracing a script with
`unittest.main()` can discover zero tests. Always inspect the executed test
count and failures, not just `OK` or a coverage percentage. Python tracing does
not measure the child managed process. Managed parser coverage comes from the
normal .NET suite with `--collect 'Code Coverage;Format=cobertura'`.

## Run exposed development evaluation

Commit the candidate sources, build from that checkpoint, then record source,
probe and managed assembly identities before evaluation. Do not rebuild during
the run. Assembly source-revision metadata can change after a commit even when
the parser text is identical. Source and binary hashes are separate identities:
the fresh build binds them procedurally; the evaluator does not prove that an
arbitrary supplied DLL was compiled from the supplied source.

```sh
PARAPHRASE_ASSEMBLY_PATH="$(pwd)/.artifacts/task-verified/bin/CnetControlPlane/debug/cnet-control.dll"
PARAPHRASE_ASSEMBLY_SHA256=$(sha256sum "$PARAPHRASE_ASSEMBLY_PATH" | cut -d ' ' -f 1)
PARAPHRASE_PARSER_SHA256=$(sha256sum dotnet/CnetControlPlane/Learning/LearningTaskProposal.cs | cut -d ' ' -f 1)
PARAPHRASE_REPORT_DIR=$(mktemp -d /tmp/cnet-paraphrase-run-XXXXXX)

python3 tools/task_paraphrase_eval/evaluate.py qualification \
  --assembly "$PARAPHRASE_ASSEMBLY_PATH" \
  --assembly-sha256 "$PARAPHRASE_ASSEMBLY_SHA256" \
  --parser-sha256 "$PARAPHRASE_PARSER_SHA256" \
  --output "$PARAPHRASE_REPORT_DIR/development.json"
```

The runner accepts only pinned suite/collection pairs and verifies the manifest
and requested corpus hashes. It never reads another collection. `--suite
original` is the backward-compatible default and supports `qualification` and
`confirmation`; both are development-only now. `--suite followup` and
`--suite round3` support only `confirmation` and cannot silently select another
population. The follow-up set is also development-only after its failed run.
After candidate source/binary pins are committed and review is complete:

```sh
python3 tools/task_paraphrase_eval/evaluate.py confirmation --suite round3 \
  --assembly "$PARAPHRASE_ASSEMBLY_PATH" \
  --assembly-sha256 "$PARAPHRASE_ASSEMBLY_SHA256" \
  --parser-sha256 "$PARAPHRASE_PARSER_SHA256" \
  --output "$PARAPHRASE_REPORT_DIR/round3-confirmation.json"
```

Record the first result even if it fails; never relabel, omit cases, lower
floors or overwrite evidence. A later reproduction of that population is not
a new confirmation. The immutable manifest records its historical unopened
state at freeze, not its current exposure state.

Exit 0 means every declared proposal gate passed; 1 means a quality gate failed
and the full report exists; 2 means arguments, integrity, runtime or report
publication failed. Existing output paths and symlinks refuse before reading a
corpus. New reports use mode 0600. A failed attempt can leave an empty reserved
file; an empty file is not evidence of a successful evaluation.

Only trusted local builds should be supplied. The reflection probe loads the
exact hash-checked assembly bytes and invokes a fixed parser method, not the
application entry point. Its private stdin format is a JSON array of UTF-16
unit arrays (at most 128 requests, 4096 units each, 4 MiB total), so isolated
surrogates reach the actual parser unchanged and are measured as abstentions.
The frozen UTF-8 corpus remains limited to 128 KiB; numeric transport does not
expand the production parser's 256-unit request limit.
Hash pinning is not a sandbox for hostile assemblies
or a compromised .NET host. JSON is bounded, duplicate properties refuse, and
corpus text is passed as data rather than shell arguments or reflection names.

## Native authority remains separate

The managed integration test `LearningNaturalTaskCommandTests` uses a new
private installation and daemon with teacher/self-answer paths disabled. It
records the missing µ task, requires owner approval of pinned external Unicode
tables, learns through the existing capsule machinery, then checks alternative
µ/A phrasing offline. It also verifies both 256-key tables, preserves uncovered
`ß` abstention, and checks that malformed/OOD/control requests create no
observations. This is representative native replay, not execution of all 128
confirmation texts through a learned runtime.

The [dated handoff](../result/cnet_paraphrase_evaluation_20260909.md) records the
actual scores, source pins, review findings, regression counts and remaining
scope. Existing live services, capture policy and the frozen soak are not
changed by this evaluation.
