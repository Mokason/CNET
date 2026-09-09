# Parser checkpoint publication verification

Recorded 2026-09-09 16:45 UTC. The user explicitly requested publication to
origin/master. Source commit: `59b8923e59c6275bf3d3ca78d8c81a72e8d782fc`.

## Fresh build and regression result

Run from the isolated Herdr worktree:

```sh
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj \
  --artifacts-path .artifacts/herdr-push-IlHmxN \
  -p:RestoreLockedMode=true \
  --logger 'console;verbosity=minimal' \
  --logger 'trx;LogFileName=full-suite.trx' \
  --results-directory /tmp/cnet-herdr-push-IlHmxN/results
```

Exit 0: **2,386 passed, zero failed, zero skipped**, including all 72 verb
regressions. Test execution reported 1 minute 25 seconds; this is not a latency
benchmark. Raw console output is retained as [push-full-suite.log](push-full-suite.log).
The detailed TRX remains in the local temporary results directory; it is not a
remote archival guarantee. Existing CA1416 platform warnings in unchanged test
fixtures remain visible in the log. No earlier frozen binary was rebuilt.

Source hashes before and after the run were unchanged:

| File | SHA-256 |
| --- | --- |
| LearningCaseRequestSyntax.cs | `42a4e34f0c6aace63ee7f027d14dd79f296ef8ea50dcbb29a9a12c6e5dd1cd29` |
| LearningTaskProposal.cs | `30d74983ce6fffe58c3d66ebf6daa7e4ba40b1949dede48bdffdb84eb26ce78d` |
| LearningCaseVerbCompositionTests.cs | `cd68b5088cd770e6da76e34a374f6d9cd7c972bc92dd9138817c4cc9134dd97b` |
| Built cnet-control.dll | `a43f74309b498596b8ac0d79e0ffb01935184149d0ed4fff010171af7a9b4e12` |
| push-full-suite.log | `2ca238375acc8e206da7111101ed33f88d82ce8e43a0eb0a4fbedf0e8f5809fd` |

The binary was built before the source commit; embedded revision metadata can
name its parent. The source hashes above match the committed source. Build
outputs are not published.

## Review and boundaries

The coordinator reviewed the source changes and reproduced the dropped leading
verb on the first supplied build. The user fixed it and added the regression.
The corrected supplied build independently passed 72/72 and direct probes:
`Raise 'a' after converting it to lowercase.` abstains with no dataset/key;
`Show 'a' after converting it to lowercase.` proposes lower/97; standalone
`Raise 'a' to uppercase.` proposes upper/97. Before-conversion and extra-action
probes abstain. The corrected supplied DLL hash was
`5ea3f8f2dd471d0b28c704f6e2d4d71625aa1c38c71cfce5c96e067318d66b02`.
An initial no-build probe hit the retained RED artifact instead; its failures
were not attributed to the corrected source.

`make asi_framing`, `bash tests/test_execution_tiers_doc.sh` and the source/doc
working-diff whitespace check passed. The full staged evidence check additionally
reports existing whitespace in `benchmarks-first/native/prerequisites-build.log`
and `corpus-continuation4/protocol.md`. Those hash-bound historical bytes are
preserved; all other staged paths pass the whitespace check. All three earlier benchmark bundles passed their
existing SHA256SUMS checks. A scoped credential-pattern scan found no matches;
this is not a comprehensive vulnerability audit. No native gate or performance
benchmark was newly run for this final verb candidate.

Documentation-and-ADRs guidance was used to preserve chronology and state the
limits: earlier acceptance failures remain failures; the fourth draft was never
admitted; no learned intent model, GPU training, live deployment or new source
authority is established by these regressions. Only source and reviewed evidence
are published. The primary dirty worktree and existing live services are untouched.
