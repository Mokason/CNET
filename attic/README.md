# attic/

Code that is kept for reference but is deliberately **not** part of the build.

Nothing here is referenced by the Makefile, and `make orphan_tools` /
`make orphan_tests` skip this directory. Anything moved here should say what
superseded it, so "is this current?" has an answer in the tree rather than only
in whoever last ran a campaign.

## Compete fixture generations

The competition fixtures are versioned by generation, and every generation
except the first two is still wired and still reproducible:

| Generation | Status | Built by |
|---|---|---|
| `cnet_compete_fixture.c` | **superseded** — here | — |
| `cnet_compete_fixture_v2.c` | **superseded** — here | — |
| `cnet_compete_fixture_v3.c` + `_oracle_v3` | live | `benchmarks/cnet_asi5_v3` targets |
| `cnet_compete_fixture_v4.c` + `_oracle_v4` | live | `benchmarks/cnet_asi5_v4` targets |
| `cnet_compete_fixture_v5.c` + `_oracle_v5` | **current** | `benchmarks/cnet_asi5_v5` targets |
| `cnet_compete_fixture_audit.c` | live, generation-independent | audit targets |

**v5 is the current generation.** v3 and v4 stay in `tools/` because their
benchmark directories and recorded results are still cited; they are frozen
history, not work in progress. The first two generations have no surviving
benchmark directory and no Makefile rule, which is why they are here.

Everything in this directory remains in git history at full fidelity — moving a
file here changes what the build sees, not what the record holds.
