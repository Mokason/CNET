# Documentation cleanup and rewrite

Status: completed. Scope approved by the owner's request to clean outdated
`.md` / `.txt` files and rewrite relevant documentation. Worktree:
`experiment/offline-controller-20260906`, starting at `c615403`.

## Contract

The initial tracked inventory is 394 Markdown and 116 text files. A `.txt`
extension does not imply prose: weights, contracts, prompts, build inputs,
fixtures and evidence remain data, and are not rewritten as documentation.
Agent instructions, licenses, frozen benchmark protocols, measured/negative
evidence and unrelated user changes are protected. No runtime feature, deployed
service, model, database, certification floor or remote branch is changed.

Current guidance will describe the actual code and distinguish the deterministic
runtime, opt-in GPU experiment and historical deployment observations. Old
plans are not relabelled completed. Superseded narrative is retired only with a
replacement, reference check and recoverable original; unique historical
decisions remain available as history, not current operating instructions.

Before edits, all 510 tracked Markdown/text files were saved under
`/home/marble/AI/CNET-documentation-backup-20260906-pkRVLL/before.tar.gz`.
SHA256: `7f70888d64781698d94365c95aca53eb64e4328562e50b7c00123d7b3e96be8f`.
The source commit also preserves clean tracked originals; the backup additionally
preserves working copies of generated decimal weights.

## Ordered tasks

1. [x] Inventory and reference audit. Classify every original path; separately
   review text consumers and historical-plan retention. Record explicit
   retire/keep/rewrite reasons. Check dirty files and exact targets before edits.
2. [x] Rewrite the entry points in small groups: README/index, architecture/
   dispatch, build/verification and capsule operations. Verify links, code paths,
   command names and existing documentation contracts after each group.
3. [x] Rewrite current subsystem references and development/security guidance.
   Preserve operational safety constraints and public interface facts; identify
   generated ledgers and historical evidence instead of rewriting measurements.
4. [x] Consolidate obsolete planning/status clutter using the reviewed migration
   map. Preserve historical rationale in a recoverable archive. Remove only
   explicitly reviewed disposable text outputs; never weight/fixture families.
5. [x] Validate references and protected-file hashes, run documentation contract
   tests, build checks and proportionate regressions. Review the final removal
   manifest independently and report counts in the completed evidence record.

## Acceptance and recovery

- A new reader has one current navigation path and reproducible local commands.
- Every retired path has a reason and a replacement or recovery location; no
  active consumer is left pointing to a removed file.
- Frozen evidence, test/model inputs and user changes remain byte-identical.
- Source/build behavior is unchanged except a narrowly tested documentation-gate
  encoding repair. The existing `asi_framing` baseline failed because Make
  searched for a mojibake dash instead of the existing UTF-8 phrase; RED was
  reproduced and the literal check corrected without changing the doctrine.
- No release clearance is inferred from documentation checks. The known managed
  control-plane SQLite advisory remains visible.

For recovery, extract `before.tar.gz` into a new empty directory and copy only
the chosen files back after inspecting the diff. Do not extract over a live
workspace. Retired tracked originals can also be inspected with
`git show c615403:relative/path`.

Knowledge-graph discovery reported this CNET worktree is not indexed; file and
literal-reference inspection is the documented fallback. The shared skill
Definition-of-Done reference is absent; these explicit acceptance checks apply.

## Closure

[Evidence and test results](../result/documentation_cleanup_20260906.md):
96 original guides rewritten, four new guides, 47 recoverable retirements,
366 protected originals and one spec with two recovery-link updates.
All 144 archive members and the full 510-path inventory were independently
verified. Native regression: 28/28 suites; managed units: 1,147 passed,
36 explicit skips; focused doc, capsule, recovery and input-consumer checks pass.

The existing legacy coverage-harvest command remains red because of a removed
Python seeder; packaging and other implementation limits are now documented
accurately, not silently treated as complete. These code follow-ups and the
SQLite advisory are outside this documentation-only task.
