# Makefile fragments

The root [Makefile](../Makefile) owns shared variables and includes named
fragments. Prerequisite expansion depends on include order; inspect the actual
include sites before moving rules. `.DEFAULT_GOAL` is explicitly pinned.

[verify_tiers.mk](verify_tiers.mk) is the source of tier membership:

| Command | Role |
| --- | --- |
| `make verify-fast` | Short edit-loop checks |
| `make verify` / `make test` | Default integrity/runtime/contract gate |
| `make verify-t2` | Specialty and adapter checks |
| `make verify-long` | T1 + T2 + longer training/compatibility checks |
| `make verify-nightly` | Extended scheduled workload |

The verify runner uses fresh sentinels and log verdict checks; an old PASS
file is not a new run. Avoid duplicating target counts or dependency lists
in prose, where they quickly become stale.

Fragments cover core library builds, integrity, serving, security, AMD math
and other feature groups. [integrity.mk](integrity.mk) includes source/rule
coverage and [makefile_budget.txt](makefile_budget.txt) enforcement.
The text budget file is a build input, not obsolete documentation.

New specialty gates normally belong in T2 unless they catch a silent
main-path failure. Moving rules must preserve target behavior and pass
the applicable build-integrity and tier-sync checks.
