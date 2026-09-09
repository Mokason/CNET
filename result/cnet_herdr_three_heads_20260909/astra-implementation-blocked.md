Blocked before implementation. I read `AGENTS.md` and verified branch `experiment/herdr-language-20260909`.

- **Files changed:** no production/test files; only test-generated `.artifacts/` output. Coordinator files preserved.
- **RED:** 0 assertions executed; `TASK_CONSTITUENT_RED` not obtained.
- **Green/full managed/native:** not run.
- **Blockers:** file writes fail with `bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted`. The focused command reached MSBuild but failed with `NETSDK1004` for missing private build assets:

```sh
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --artifacts-path .artifacts/herdr-task --filter 'FullyQualifiedName~LearningCaseRequestSyntaxTests|FullyQualifiedName~ConstituentDispatchPreservesCanonicalAndBoundRequests|FullyQualifiedName~ConstituentProposalStillRequiresPolicyAndExternalEvidence' --no-restore --logger 'console;verbosity=normal'
```

No component parser was implemented. The remaining slice is independently composed prefix/action/operand-description/punctuation tests, actual RED capture, bounded constituent parsing, and focused/full verification.

No acceptance corpus was opened or scored. Certification floors and zero wrong-ready remain unverified. Workspace writes must function before this slice can proceed.
