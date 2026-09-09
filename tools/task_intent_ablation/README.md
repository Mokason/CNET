# Offline intent contribution diagnostic

Compare the existing hybrid with the existing private grammar-only parser and
a guarded learned-raw arm. No production switch, native task, source approval,
model fitting job, live request or fresh confirmation is run. Raw learned input
has different preprocessing and retains handwritten safety/operand guards: it
is not a causal test of removing all grammar.

From this worktree on the configured development host:

```sh
dotnet build tools/task_intent_ablation/TaskIntentAblation.csproj --artifacts-path .artifacts/intent-ablation
python3 -m unittest discover -s tools/task_intent_ablation -p 'test_*.py' -v
python3 tools/task_intent_ablation/run_diagnostic.py \
  --assembly .artifacts/usability-review3-20260910/bin/CnetControlPlane/debug/cnet-control.dll \
  --corpus result/cnet_learned_intent_proposer_20260910/confirmation-v2/evidence/private/confirmation.json \
  --corpus-sha256 10cdd9e0c4a5422fe08a105b6d0cabc72d092a76df1b8b70ab34a13812276e32 \
  --output /absolute/new/private/diagnostic-directory
```

Build the control/test project into the named artifacts path first. The guard
tests intentionally use the retained baseline under `.artifacts/usability-20260910`;
they test the diagnostic interface, not the latest production regressions.
The runner pins `/home/marble/dotnet/dotnet` as its absolute host and runs the
probe with an empty environment. These are explicit local experiment paths,
not a portable installed CLI. Retain baseline assets when reproducing its tests.

The output directory must not exist. It receives pins before execution, bounded
raw predictions, and exact per-arm scores only after pins are checked again.
Scores include all 128 rows and check status, operation and original scalar.
Equal full outputs do not prove that the fallback was never invoked. Success
is not published after a detected pin change; raw partial artifacts may remain.
This is not a crash-durable journal or the one-shot confirmation runner.

Every result is synthetic, exposed and training-ineligible. Never rerun
`score_once.py` or relabel development diagnostics as fresh confirmation.
Historical snapshots and mappings preserve old source/binary identities when
the working source advances. Executable/build snapshots remain local artifacts;
their hashes are recorded rather than committing binaries to the repository.
