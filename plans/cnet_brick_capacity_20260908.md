# Legacy brick capacity repair

The owner requested publishing completed work, pruning obsolete branches, then
repairing Discord failed-brick diagnostics. Published 24 commits through 890860e;
eight local unchecked-out and fourteen remote ancestor refs were removed with
expected-tip guards. Unmerged refs, checked-out branches and all worktrees remain.
The dirty primary checkout and private data were not published or discarded.

Read-only owner-DM diagnostics and the local failure witness agree: 24 existing
LUTs, both legacy bank limits 24, projected insertion 25 refused before minting.
This is not the separately measured 4096 certified-capsule capacity.

## Contract / sequence

1. Add RED tests for 25th entry, all 256 slots, retained prior snapshot on failed
   reload, clean empty refusal at startup, duplicate identities and no raw-table
   certification. Keep the existing 16-input domain and admission floors.
2. Raise both in-process bank limits to 256; bounded inline storage avoids new
   allocation/ownership APIs. Measure both struct sizes and rebuild all affected
   executables (struct layout is not binary-compatible with old consumers).
   Reload must publish only complete valid snapshots, preserve counters and leave
   the old bank unchanged on failure. No automatic eviction or second packaging.
   The global compose loader must obey the same replacement rule. Cooperating
   POSIX publishers lock the directory inode, validate the existing bank and new
   table, reserve capacity, then atomically rename a complete private file.
   Replacement at full capacity remains allowed. Invalid prior inventory needs
   operator repair; publication never silently repairs or evicts existing data.
   Canonical tags cannot alias after loader whitespace normalization. A directory
   fsync failure is reported even if rename already published the file.
   The Windows publisher explicitly refuses with ENOSYS pending an equivalent
   implementation; Windows runtime support is WITHHELD, not a weaker fallback.
3. Build/test native serving, mint and evolve tools, verify the actual 24-file inventory
   read-only, and exercise the 25th entry in a disposable copied bank. Use a
   separately versioned deployment and preserve old binaries/config for rollback.
   Do not change immutable capsule runtime pins, capture scope, learning policy,
   model-provider policy, legacy witness, or turn raw LUTs into certified units.
   Backport only the six affected runtime source/header files to the older live
   checkout, preserving all unrelated dirty changes. Build into a private release
   with its existing capsule shared library frozen byte-for-byte. Point only
   cnetd and its factory executable at that release; retain original binaries.
   Invalid evolve inventory must return failure without an OK marker. A refused
   configured publication must reach the factory before student ownership moves.
4. Record exact gates and live rollout status. Source publication is distinct
   from installing/retrying the owner-visible workflow; no fabricated Discord
   traffic or count-as-intelligence claim.

Broader dataset/headroom/72-hour evidence from the capture sequence remains
WITHHELD. This repair does not clear those independent gates.
Directory locking coordinates cooperating publishers, not arbitrary external
file editors. Reload is single-owner snapshot replacement, not a concurrency
API or an atomic snapshot of multiple external filesystem changes. An evolve
tick is not an all-or-nothing transaction across all its legacy subsystems.
