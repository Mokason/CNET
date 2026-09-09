# Frozen confirmation harness verification

The candidate is unchanged. This verification concerns evaluation orchestration,
not fresh language acceptance. No confirmation corpus or candidate prediction
was used to build or test the runner.

- Existing evaluator integrity suite: **34/34 passed** using
  `python3 -m unittest discover -s tests -p test_task_paraphrase_eval.py -v`.
  Its deliberately fabricated wrong-ready fixture output is not a CNET
  confirmation score.
- New single-use wrapper: **5/5 passed** with `test_score_once.py`.
- Executed RED: four initial assertions reported
  `CONFIRMATION_RUNNER_RED missing one-shot runner` before implementation.
- Independent read-only agent `one_shot_harness_review` found that unsuccessful
  probe invocations skipped post-run hash checks.
- Executed second RED: `CONFIRMATION_RUNNER_RED timeout skipped post-run pins`
  observed one check instead of two on a mocked timeout.
- Fix: post-invocation pin checks run in `finally`; transport files are flushed
  and synced on failures; the attempt journal retains exception chains.
- Independent follow-up review confirmed closure and all five tests passing.
  It made no candidate invocation. Final binding must include wrapper, evaluator,
  probe/runtime configuration, candidate artifacts, corpus, admission and freeze.
- Runtime preflight found that `dotnet` exists only under `/home/marble/dotnet`,
  outside the default executable search path used by an empty environment.
  An additional RED assertion caught the relative runtime invocation; the runner
  now uses the binding's pinned absolute runtime. A read-only `--list-runtimes`
  invocation with an empty environment succeeded. No parser was invoked.
- The runner also requires explicit role pins and probe/candidate dependency and
  runtime-configuration pins before proceeding. The shared runtime inventory is
  .NETCore.App 8.0.30 and 10.0.7, with ASP.NETCore.App 10.0.7; this .NET 8 probe
  uses the .NET 8 runtime. This is not a hermetically packaged runtime claim.
- Final independent review confirmed absolute pinned role paths, required asset
  pins and the absolute host invocation; no remaining concrete blocker was found
  in that narrow review. No reviewer invoked the candidate.

The wrapper reserves a single fixed attempt path before preflight. It reuses
the unchanged strict 128-case validator and exact scorer, sends only original
request UTF-16 units to the pinned managed probe, and retains raw transport
evidence. It does not call the native control entry point. There is no retry,
label repair after scoring, push, deployment or GPU campaign in this sequence.

The seven candidate pins were rechecked before authoring and during preparation.
The full 2,403-test result is recorded by the candidate's supplied freeze receipt;
this turn did not rerun that full suite or rebuild the frozen candidate.

Review and incremental-implementation skills guided the executed RED/GREEN
checks; documentation and git-workflow skills kept evidence separate from the
uncommitted candidate. No commit, staging or branch change was made.
