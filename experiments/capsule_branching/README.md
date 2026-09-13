# Bounded capsule branching experiment

Two separate tests, with no production changes:

1. `branching.py` joins two results from real certified CNU1 capsules using an
   experimental checked sum operator. Each receipt retains its split, branch,
   capsule identity, input, result, context and source units. The single-thread
   cache pins active cores, keeps unpinned cores warm, and evicts the least
   recently used warm core before admitting another. Capacity counts cores,
   not bytes. Receipts remain usable after eviction.
2. `paired_questions.py` measures passage answers for 200 explicitly paired
   questions using the existing VSA CLI. It compares one route with one or two
   passages against two independent routes. Both branches must accept before
   the compound request answers. This uses the CLI's existing cache, not the
   experimental CNU1 cache.

The checked sum is not a certified multi-input capsule. Context fields are
declared metadata; the experiment does not extract or validate assumptions
from prose. Receipt checks are local to a request, not cryptographic proof
authentication. There is no automatic decomposition, unbounded search,
concurrent cache use, new packaging format, or relaxed certification floor.

From the repository root, with the existing binaries available:

```sh
python3 experiments/capsule_branching/test_branching.py
python3 experiments/capsule_branching/prepare.py
python3 experiments/capsule_branching/run.py
```

`prepare.py` compiles independent arithmetic labels through the existing
`cnet_capsule_core teach ... verified_tool` path into four private fixture
roots under `var/capsule_branching_20260913/`. No composite answer is supplied
as a training label. The structured test exhausts all 4,096 two-input pairs
and exercises invalid joins, missing coverage, package corruption and cache
pressure. Run Python normally: these diagnostic scripts use assertions.

The separate paired-question experiment requires
`var/claude_scratch/answer_quality_q400_base.json`, the production VSA corpus,
and the existing local `mistral-small-3.2-24b-offline` judge on port 8092:

```sh
python3 experiments/capsule_branching/paired_questions.py
```

Judgments are cached by the exact prompt, question and answer. This exposed
question set and model judge are diagnostic evidence, not a human reference
or an unseen-content gate. Native route/rank timing excludes Python dispatch,
output assembly and the offline judge. The structured test times the complete
Python-to-native request and join, with already structured inputs.

Results and limitations: [test record](../../result/cnet_capsule_branching_20260913.md).

## Scaling follow-up

The scaling experiment expands only this experimental cache's configurable
ceiling to 32 cores. Production admission and serving remain unchanged.
`scaling.py` executes explicit topologically ordered plans, with at most 32
nodes and two parents per node. Every dependency uses a checked sum-mod-64
adapter from `next6` to `state6`; every native hop keeps its own coverage guard.
The plan and its adapters are not learned or certified multi-input capsules.

```sh
python3 experiments/capsule_branching/test_scaling.py
python3 experiments/capsule_branching/scaling_run.py
python3 experiments/capsule_branching/scaling_guards.py
python3 experiments/capsule_branching/scaling_questions.py
python3 experiments/capsule_branching/scaling_memory.py
```

These commands use `scaling_protocol.json`, fixed before the campaign and
copied to `var/capsule_scaling_20260913/protocol.json` by the runner.
`scaling_run.py` prepares 32 independently compiled capsule fixtures if its
spec catalog does not exist. It starts a fresh Python process per timing cell
and tests independent branches, chains and binary fan-in trees, with full
residency, four-core LRU residency and cold-handle reloads. All filesystem
timing is warm. The additional composition test exhausts all 64 inputs for
every sequence of two modules through depth eight and counts distinct
input/output mappings, not just program strings.

`scaling_guards.py` introduces an uncovered intermediate capsule at five
chain lengths and verifies that later nodes never execute. It also checks
32 active pins and hashes capsule files before and after its guard campaign.
`scaling_questions.py` uses the prior fixed question order, makes nested
groups of 1, 2, 4, 8 and 16 questions, and uses one uniform model judge prompt.
Its complete-answer rule requires every branch to accept. The CLI returns
at most four passages from a single route even when more are requested.
`scaling_memory.py` checks native outputs and samples RSS through 1,024 cache
cycles per policy in fresh processes, without retaining result graphs.

See the [scaling record](../../result/cnet_capsule_scaling_20260913.md) for
results, the tested limits and artifact hashes. The original two-branch
record retains the historical hash of the smaller cache implementation.
