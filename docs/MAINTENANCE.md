# Documentation maintenance and recovery

Current navigation starts at [INDEX.md](INDEX.md). Documentation is organized by
role, not by which file was most recently edited.

| Role | Location and rule |
| --- | --- |
| Operating guides | `README.md` and current `docs/` references; verify against source |
| Current task queue | [tasks/plan.md](../tasks/plan.md), [tasks/todo.md](../tasks/todo.md) |
| Decisions and contracts | `plans/`; retain rationale, limitations and supersession |
| Historical design specs | `docs/superpowers/specs/`; not automatic implementation claims |
| Frozen protocols and evidence | `benchmarks/`, `result/`, evidence subdirectories; preserve bytes |
| Generated claims | `verified-today.generated.md`; regenerate through `make claims` |
| Retired instructions | Recovery map/archive described below; never today's task queue |

## September 6 cleanup policy

The starting inventory was 394 Markdown and 116 text files at `c615403`.
The cleanup replaces sprawling status narratives with source-oriented guides.
It does not discard historical failures, certify old results again, or mark
unfinished work complete.

Historical implementation transcripts and superseded handoffs are retired only
after checking consumers and retaining the original text. Matching design
specifications remain. A design's presence is not proof its whole proposal was
implemented. In particular, historical transcripts record these corrections:

- Lifecycle spine: FROZEN tie-break preference was deferred.
- Lifecycle recovery: property-derived labels and runtime property-law triggers
  were deferred; router/contract recovery used fixed-epoch training.
- Tiered tile memory: WARM promotion-on-hit was deferred in that slice. Its
  graduation deferral is historical; a later graduation slice exists.
- JSON story labels: a finite name lexicon replaced a failed capitalization
  heuristic in `tests/jsonstory_demo.c`.

Consult the retained implementation amendments before using an older design as
a description of today's behavior. The [complete inventory](DOCUMENTATION_INVENTORY.tsv)
classifies all 510 original paths: 96 rewritten, 47 retired, 366 preserved and
one historical spec with two recovery-link updates. Four new operating guides
cover build/test, GPU training, security and maintenance.

The 47 retirements are 44 superseded implementation plans/handoffs and three
text files: the reproducible demo output, a speculative conversation and a
stale progress summary. They remain recoverable. Other plans retain decisions,
open scope or evidence; they are not today's execution queue.

## Text files are often runtime inputs

Do not delete by extension or age. Root weights/contracts/property files and
`build/CMakeLists.txt` are build/test inputs. English windows are consumed by
explicit paths and wildcard discovery. Their `.words.txt` companions are also
derived dynamically by runtimes; absence of a literal filename reference is
not proof of non-use.

Goldens, frozen holdouts, recertification verdicts, benchmark output and failed
runs preserve provenance. A known stale measurement can be important evidence
of a withdrawn claim. Keep it labelled historical rather than erasing it.
Prompts, charters, policy lists and pack manifests are data, not prose to reword.

## Recover an original

The checked-in [archive](../attic/documentation-20260906.tar.gz) contains 144
original files: every retired file, every rewritten pre-existing guide and
the reference-updated spec. [Members](../attic/documentation-20260906.members)
give exact original paths; the inventory records each original SHA256 and its
replacement. The archive is 679,555 bytes.
SHA256: `19f152004f3fa557410f869a3198c9fc6a3d7f7694e1eb8581014d0f6941c579`.

From the repository root, recover one file into a new private directory:

```sh
doc_restore=$(mktemp -d /tmp/cnet-documentation-restore-XXXXXX)
tar -xzf attic/documentation-20260906.tar.gz -C "$doc_restore" \
  docs/superpowers/plans/2026-06-18-lifecycle-spine.md
sha256sum "$doc_restore/docs/superpowers/plans/2026-06-18-lifecycle-spine.md"
```

Compare the hash with the matching inventory row before using the restored
copy. The archive also preserves old tutorial detail and benchmark tables;
recovering them does not make their setup/status claims current.

Tracked clean originals remain at the immutable pre-cleanup checkpoint:

```sh
git show c615403:docs/ARCHITECTURE.md
```

A full pre-edit backup also preserves all 510 working copies, including existing
dirty decimal weights:
`/home/marble/AI/CNET-documentation-backup-20260906-pkRVLL/before.tar.gz`.
SHA256: `7f70888d64781698d94365c95aca53eb64e4328562e50b7c00123d7b3e96be8f`.

Extract backups into a new empty directory, inspect the selected files and copy
only what you intend to restore. Never unpack over a live workspace or use a
broad reset to undo documentation changes.

## Rules for future updates

Document the actual API and its failure behavior. Separate default behavior,
opt-in experiments, historical observations and open scope. Put benchmarks in
dated evidence reports with source/fixture identity, then link them from guides.

Run the documentation contract and reference checks after edits. Preserve the
license and project doctrine, keep shell examples private and reproducible,
and never replace missing evidence with a confident status paragraph.
