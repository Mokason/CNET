# Certified capsule core

The local core executes typed, covered capabilities from the existing
`unit.cnb` + `manifest.cknow` capsule format. It verifies a complete inventory,
searches for a compatible plan and audits every executed hop. Unsupported input
or missing coverage abstains; it does not silently fall through to prose.

A capsule is not an MTK tensor-delta cartridge. See [architecture](ARCHITECTURE.md)
for the distinction and [security](SECURITY.md) for the trusted-owner boundary.

## Private-registry example

The example creates independent arithmetic-tool labels and a new private store.
It does not use a live base or train on an existing CNET answer.

```sh
make capsule_core
capsule_demo=$(mktemp -d /tmp/cnet-capsule-example-XXXXXX)
for x in 0 1 2 3 4 5; do
  printf '%d\t%d\n' "$x" "$((x * 60))"
done > "$capsule_demo/minutes.tsv"
bin/cnet_capsule_core teach "$capsule_demo/capsules" minute_conversion \
  minutes seconds 3 9 verified_tool "$capsule_demo/minutes.tsv"
bin/cnet_capsule_core ask "$capsule_demo/capsules" 'capsule minutes seconds 3'
bin/cnet_capsule_core ask "$capsule_demo/capsules" 'capsule minutes seconds 6'
```

The covered input 3 returns verified 180. Input 6 deliberately refuses; the
last command therefore exits nonzero. Keep that refusal as part of the test.

Rows contain two unsigned integers, no header/comments/duplicate inputs.
Teaching accepts 1–16 bits per side and at most 256 rows. The candidate must
pass all supplied rows at the unchanged 0.05 robustness margin.
`verified_tool` and `user_correction` describe local provenance, not
authentication. Do not expose this command as an untrusted upload API.

The ordinary teaching CLI can use `gradient_fit` or `finite_domain_compile`;
its receipt names which. Both use the existing BTN/certification format and
exact supplied-input coverage. Neither proves unseen-input accuracy. The
[GPU conversion experiment](GPU_TRAINING.md) instead directly copies genuinely
fitted weights and has no finite-compiler fallback.

## Publication and historical consistency

A candidate is verified, given coverage/provenance, exported, re-imported and
checked alongside the installed inventory before no-clobber publication.
Conflicting identities, incompatible interfaces and ambiguous tag signatures
refuse. Identical retries can return `reused=1`; different content under an
installed name cannot overwrite it. Use a new versioned name for new evidence.

Every publication replays exact guarded joins of sealed labels in both old and
proposed inventories. Optional evaluation samples cannot omit this obligation.
`label_obligations` counts replayed joins, not unique tasks. Replayed history is
evaluation evidence, never self-generated training data.

Staging directories begin with a dot and are not served. Keep unrelated content
outside the capsule root. Sync failures after rename report failure while
retaining the complete artifact; retry identical evidence to repair durability
rather than overwriting it. Fault-injection tests are not physical power-cut
certification or a transaction across arbitrary independent writers.

## Query interface and limits

Exact grammar: `capsule INPUT_TAG OUTPUT_TAG UNSIGNED_INTEGER`.
Tags bind complete typed signatures. The ordinary core supports single-field
binary ports up to 16 bits and one-hot ports up to 64 symbols, at most 256 hidden
neurons and 4096 sealed rows per kernel. Multi-slot execution and unknown asset
schemas refuse. The explicitly supported schema-2 source-evidence asset is
described in [CNET_SOURCE_EVIDENCE.md](CNET_SOURCE_EVIDENCE.md).

The inventory limit is 4096 immediate capsule directories. Search follows
typed-port/actual-value states, allowing up to eight hops, 1024 states and 65,536
charged scans/comparisons. Coverage applies to the actual value at each hop.
Exhaustion refuses; an uncovered value cannot blacklist a suffix usable for a
different covered value.

Growth replay scales aggregate work by
`max(1, ceil(max(old_count, new_count)/256))`, starting at two million operations
and capped at 32 million. Its cooperative deadline scales from two to 32 seconds;
direct consistency and individual phase checks retain separate bounds. Loading
and certification are separate, so these are not hard real-time guarantees.
The public contracts are in [cnet_capsule_core.h](../include/cnet_capsule_core.h).

## Daemon integration and swaps

An explicit `reuse ABS_SOURCE ABS_DESTINATION REQUEST` command can export only
the dependencies selected by a verified execution. The original capsule files
and assets are retained; a staged fresh-core identity/replay check precedes
publication. Later asks still plan and check coverage at every hop. See
[composition reuse](COMPOSITION_REUSE.md) for the two-hop unit-conversion example,
ownership/source-freshness limits and refusal behavior. This command does not
activate a snapshot or change live daemon state.

Configure a capsule inventory in the daemon's effective environment to enable
requests over its existing ASK socket. JSON: `{"q":"capsule minutes seconds 3"}`.
Only successful certified execution returns `source=LOCAL, verified=true`.

Constrained whole-request conversion forms are supported, including
`convert 3 minutes to seconds` and `how many seconds are in 3 minutes?`.
Keywords ignore case/whitespace; tags remain exact. Fractions, signed values,
ambiguous requests and trailing alternative instructions clarify or refuse.
This parser is not a free-form semantic model.

The daemon now retains a resident inventory. Legacy `CNET_CAPSULES_DIR` loads
once at startup; publishing into that directory does not trigger a reload.
For named-set operation, configure all three absolute paths, without the legacy
daemon variable:

```text
CNET_CAPSULE_SETS_DIR=/owner-private/cnet/sets
CNET_CAPSULE_STATE_DIR=/owner-private/cnet/state
CNET_CAPSULE_CONTROL_SOCK=/owner-private/cnet/control.sock
```

The set/state/control parent directories must already exist, owned by the
daemon UID with mode 0700. Ancestors must be root/owner-controlled, without
symlinks; trusted sticky `/tmp` is allowed. Each set is a named immediate
subdirectory containing only capsule directories and the native publisher's
optional private `.publish.lock`. Capsule artifacts are owner-owned regular
single-link files, not group/world writable. Newly exported directories may
need owner sealing to 0700. Never relax permissions to admit a candidate.

After building `make capsule_control`, inspect the actual status and use its
revision. For an initial empty state the concrete sequence is:

```sh
bin/cnet_capsulectl /owner-private/cnet/control.sock STATUS
bin/cnet_capsulectl /owner-private/cnet/control.sock STAGE 1 1 reviewed_set
# Copy the exact staged= digest from this successful response:
bin/cnet_capsulectl /owner-private/cnet/control.sock ACTIVATE 1 release_001 STAGED_DIGEST
bin/cnet_capsulectl /owner-private/cnet/control.sock UNLOAD 2 unload_001
bin/cnet_capsulectl /owner-private/cnet/control.sock ROLLBACK 3 restore_001
```

`STAGED_DIGEST` is a placeholder, not an executable example value. `STAGE REV 1`
requires candidate self-closure and all incumbent obligations; `STAGE REV 0`
explicitly switches the working set and may remove coverage. Neither operation
may replace a previously activated unit name with a changed sealed/coverage/
asset identity. Use a new versioned unit name for changed evidence.
`DISCARD REV DIGEST` abandons preparation, not stored knowledge. Chat has no
activation authority. An unloaded query still abstains; configured demand
recording may queue an uncovered typed request, but never activates its result.

Activation first copies and verifies immutable snapshots, then durably replaces
one selection record. Every operation uses a monotonic revision. Exact retry of
the most recent mutation with the same original revision, token and arguments
does not repeat it; older/different operations refuse. A post-rename sync error
reports `durability_uncertain`, retains current serving and freezes mutations.
Restart reconciles and revalidates the complete selection. A transport failure
after sending a mutation is an unknown outcome: query STATUS and retry exactly,
never assume rollback. This is tested crash recovery, not physical power-cut
certification or safety against malicious same-UID writers.

The host caps four materialized generations, 64 pinned requests, one staged
candidate and 16,384 historical names. A snapshot set admits 4096 units and
32 GiB aggregate artifact bytes; individual CNB files cap at 64 MiB and
manifest/assets at 16 MiB. These are disk/input caps, not a 32 GiB RAM quota.
Multiple loaded generations, coverage and scratch add memory. Unselected
private cache copies are collected on startup/next staging; source sets and
historical identities are not deleted. The serial daemon does not promise
uninterrupted low latency while admission runs. [GPU training](GPU_TRAINING.md)
remains a separate opt-in selector/trainer path.

## Scheduled evidence ingestion

Configure owner-private `CNET_CAPSULE_QUEUE` and worker `CNET_CAPSULES_DIR`
pointing to one mutable source set, e.g. `/owner-private/cnet/sets/reviewed_set`.
The worker publishes evidence; a separate operator STAGE/ACTIVATE changes the
resident daemon. Do not set this worker-only legacy variable on a daemon that
uses the three durable-control variables.
Each immediate queue job contains:

| File | Format |
| --- | --- |
| `request.tsv` | `UNIT INPUT_TAG OUTPUT_TAG INPUT_BITS OUTPUT_BITS verified_tool` or `user_correction` |
| `training.tsv` | External `INPUT EXPECTED` rows |
| `evaluation.tsv` | Independent `INPUT_TAG OUTPUT_TAG INPUT EXPECTED` cases, including old tasks |

`bash scripts/cnet_capsule_curriculum_tick.sh` processes the configured queue.
Do not invoke it against a live queue without operator intent. It snapshots and
hashes evidence and uses bounded round-robin scanning. Successful `receipt.txt`
files carry `evidence_sha256`. A `refusal.txt` is diagnostic, not digest-bound;
malformed jobs can fail before producing it. Defaults: two attempts/tick, 120 seconds/attempt,
64 scanned jobs. Bounds: 1–8 attempts, at most 256 scanned jobs, 4096 queue
entries, 256 rows per training/evaluation file and 64 KiB per evidence file.

Every evaluation case must receive the correct certified answer. New publication
also requires strictly improved correct coverage; exact retries can retain equal
coverage. Improving a partially covered evaluation set is insufficient.
The ingestion path does not make teacher calls or infer labels from a miss.

## Approved-tool acquisition

Opt-in acquisition paths (worker environment; the daemon needs only the demand
path plus its resident-control configuration):

```text
CNET_CAPSULES_DIR=/private/sets/reviewed_set
CNET_CAPSULE_QUEUE=/private/curriculum
CNET_CAPSULE_DEMAND_DIR=/private/demand
CNET_CAPSULE_TOOL_POLICY=/private/tool-policy.tsv
```

Use directories owned by the service user with mode 0700 and a policy file with
mode 0600, under a trusted parent. Start from
[capsule_tools.example.tsv](../config/capsule_tools.example.tsv).
Policy columns: `INPUT_TAG OUTPUT_TAG INPUT_BITS OUTPUT_BITS OP OPERAND MIN MAX`.
Supported tools are bounded unsigned `mul` and `xor`, not arbitrary commands.
Policies allow at most 256 rules and 64 KiB, with newline-terminated lines.
Tags contain 1–31 alphanumeric/underscore characters. Input/output tag pairs
must be unique, and every reuse of a tag must retain its bit width.

A miss remains unverified when demand is queued. The acquisition tick computes
independent tool labels in blocks of at most 32 inputs, intersects policy and
representable ranges, then publishes curriculum jobs. Training and evaluation
on that same finite block prove exact acquisition, not held-out accuracy.

The native planner can discover an approved multi-step tool path:
`bin/cnet_capsule_tool plan PRIVATE_POLICY INPUT_TAG OUTPUT_TAG VALUE`.
It permits eight hops, 1024 value states and 65,536 charged comparisons.
Exit 0 is an acquisition proposal, not certification; exit 3 means no supported
approved path, and exit 2 means invalid input/arithmetic/work failure.

Each tick scans at most 64 demands, publishes at most two new jobs, and permits
2048 evidence-tool invocations. At most 256 pending demands are retained.
Longer paths progress across ticks. Durable jobs or terminal policy refusal
release demand; tool failure/full queues retain it. Certification can still
refuse a produced job. Inspect receipts, not merely `demand=queued`.

## Checks, evidence and recovery

```sh
make capsule_frontdoor capsule_curriculum capsule_demand_growth
make capsule_demand_security capsule_acquire_security
make capsule_value_search capsule_history_coverage capsule_core_budget
make capsule_composed_acquire capsule_composed_refusal
make capsule_capacity checkpoint_binding checkpoint_durability capsule_publish_recovery
make capsule_product_closure capsule_control
```

These use isolated fixtures. [Inventory results](../result/cnet_capsule_capacity_4096_20260906.md)
describe synthetic capacity, not 4096 independent acquired domains or a universal
RAM-per-capsule constant. September deployment observations remain in
[live acquisition evidence](../result/cnet_live_acquisition_20260906.md) and
[remaining-sequence evidence](../result/cnet_remaining_sequence_20260906.md);
they are not a claim that today's live service is unchanged.

Use `bash scripts/check_deployed_learning.sh` to inspect effective deployed
configuration and `bin/cnet_capsule_core inspect BASE` to inspect a chosen base.
Never restore an old base over running writers. Preserve new evidence, coordinate
the affected writers, validate exact coverage bindings and perform explicit
operator rollback. Quarantined unguarded units must be independently revalidated,
not restored by disabling coverage.
