# mk/ — Makefile fragments

The root `Makefile` reached 8,530 lines and 989 targets: 527 KB, seven times
the size of the largest source file, and the single point through which every
build, gate, campaign and demo is expressed.

That size had a concrete cost, not just an aesthetic one. `tests/clgemm_unit.c`
existed, was cited four times in `docs/ARCHITECTURE.md` and once in `README.md`
as the executable proof of GPU bit-identity, and had **no rule to build it**.
Nobody noticed, because in a file this size a missing target is invisible.
Twenty-six test files and six tools were in that state.

## How the split works

Every `mk/*.mk` fragment is `include`d from the **end** of the root `Makefile`,
after all variables are defined. That ordering matters: prerequisites are
expanded when a rule is *read*, so a fragment included last can rely on every
variable in the tree, while the reverse is not true. `.DEFAULT_GOAL` is pinned
explicitly in the root `Makefile` so moving rules around can never change what
bare `make` does.

Current fragments:

| Fragment | Holds |
|---|---|
| `mk/integrity.mk` | the build-integrity gates: `curl_guard`, `orphan_tests`, `orphan_tools`, `platform_sweep`, `residual_http_nocurl`, `layering_guard`, `makefile_budget` |
| `mk/orphans.mk` | targets for the tests and tools that previously had none |

## The rest of the split is staged, not done

The remaining ~8,500 lines are **not** mechanically separable. The original file
has no section banners and 301 scattered `.PHONY` declarations, so there is no
boundary a script can cut on; classifying those rules is a hand job that needs a
green `make verify` on both the Windows and Linux boxes to land safely.

Two things keep that from being an open-ended liability:

1. **The harm is already fixed.** `make orphan_tests` and `make orphan_tools`
   fail the build when a source file has no rule, so the specific damage the
   monolith caused cannot recur regardless of the file's size.
2. **It cannot silently grow.** `make makefile_budget` fails if the root
   `Makefile` exceeds the ceiling recorded in `mk/makefile_budget.txt`. Moving
   rules into a fragment lowers the ceiling; adding rules to the root file has
   to be a deliberate act that edits that number.

When you do move a section here, lower the budget in the same commit.
