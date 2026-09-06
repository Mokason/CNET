# Local table capsules

The local table adapter compiles a finite owner-approved dataset into the existing
CNU1-sealed CNB capsule, with exact labelled coverage. It accepts numeric keys
0–255 and numeric values 0–65535. It does not infer missing rows or interpret prose.

Configure `CNET_CAPSULE_DATA_ROOT` on the resident daemon to an existing absolute
private directory. A dataset named `stock_levels` is read only from
`CNET_CAPSULE_DATA_ROOT/stock_levels.tsv`. Dataset identifiers match
`[a-z][a-z0-9_]{0,30}`. Every source file must be a private owner-owned regular file
with one link; the configured root must be private and owner-owned. Symlinks,
dot traversal, unsafe ancestors, executable sources, and special files refuse.
Shared sticky system ancestors such as `/tmp` are permitted.

The complete canonical UTF-8/ASCII source is:

```text
CNET_LOCAL_TABLE_V1
dataset stock_levels
authority user_correction
input_bits 8
output_bits 16
rows 3
0	120
7	42
19	0
```

The three data rows use literal tab separators. Header spaces and newlines are
exact. Every line, including the last row, ends in one LF. Rows must be sorted by
key, distinct, and number 1–256. Decimal integers have no leading zeros except
`0`. The parser refuses comments, blank lines, extra fields, CRLF, missing final
newlines, NUL bytes, and source files over 4096 bytes.

`authority` is exactly `user_correction` or `verified_tool`. Use the former for
actual owner corrections and the latter for a policy-approved instrument or tool
result. Neither declaration authenticates its producer. The trusted supervisor
must separately authorize the dataset, source type, and publisher. The owner is
responsible for the truth of supplied numbers. Do not feed CNET's own answers into
either source category. Integrity hashes and exact parser agreement do not prove
external factual accuracy or third-party authenticity.

An explicit owner command can create a new capsule:

```sh
bin/cnet_table_capsule build /absolute/private/data stock_levels /absolute/private/output/new_capsule
```

The output parent must already exist and be private. Existing output directories
are never overwritten. Success exits 0 and emits `TABLE_CAPSULE_PASS`, including
the source SHA256, typed interfaces, unit name, authority, row count, certification
margin, and `pending_activation=1`. Refusal exits 1. An uncertain final sync exits
3 with `TABLE_CAPSULE_COMMIT_UNCERTAIN retained=1`; its complete artifact stays
available for owner reconciliation and must not be assumed published durably.

Unattended acquisition uses the restricted worker entry point:

```text
bin/cnet_table_capsule build-worker ABS_DATA_ROOT DATASET EMPTY_ABS_PRIVATE_OUTPUT_ROOT EXPECTED_PARENT CPU_SECONDS MEMORY_MIB
```

It enters the native learning sandbox before reading source data, and writes only
`OUTPUT_ROOT/capsule`. The parent creates an empty 0700 output root, passes stdin
as `/dev/null` and stdout/stderr as bounded pipes, enforces a deadline, kills and
reaps timed-out workers, and independently verifies completed output after reaping.
The launcher passes its own PID captured before spawning and the policy's CPU and
memory limits. All three arguments are canonical unsigned decimal without leading
zeros. The PID must be 2–2147483647 and still identify the worker's parent; CPU is
1–120 seconds and memory is 64–2048 MiB. Missing arguments, out-of-range limits,
or adoption by a subreaper after launcher death refuse before source parsing.
The worker applies exactly those CPU/address-space limits plus a 16 MiB per-file
limit, and dies if its original launcher exits after sealing. It does not activate a
capsule or contact a network, service, teacher, shell, or external process.

The asset is exactly the source bytes in `frontend.cvfa`, using asset schema
`0x43544501` inside the existing capsule schema 2. No additional package is
introduced. The full SHA256 binds these bytes. Versioned port names are
`data_<first22hex>_key` and `data_<first22hex>_val`; unit names use
`table_<first56hex>`. Short-name collisions with differing full identities refuse.
The distinct port suffixes preserve the existing CNB near-miss typo guard.
Ordinary capsules cannot claim the reserved `data_` port namespace.

After the owner activates an approved snapshot, query it through the same resident
core or daemon:

```text
data stock_levels 7
```

This request resolves only one activated table whose dataset and full source hash
match the currently configured source. It returns the certified decimal value or
abstains. It does not create learning demand. The serving guard reopens the source
at every used table hop and before returning the answer. The owner must keep the
source stable during a request; these checks are not a filesystem transaction or
a post-return freshness promise.

Refresh appends a new immutable capsule and preserves every historical capsule.
Until that version is activated, changed source content causes the logical query
to abstain. Activation selects the fresh version through the loaded inventory;
there is no separate mutable latest-version map. Old contract and asset identities
remain static upgrade obligations. A direct request to an old typed interface
still abstains when its source has changed. Source-independent incumbent capsules
continue serving. Reaching the inventory limit refuses further growth rather than
discarding history automatically.

Certification keeps the `.05` robust margin and checks every supplied row. Sparse
coverage refuses missing keys. All-domain verification measures fidelity on the
supplied finite rows and refusal on omitted keys; unseen-row capability is WITHHELD.

## Independent candidate evaluation

Build the bounded native test surface with `make learning_native`; run its managed
reference, protocol and ledger tests with `make learning_managed` after restoring
the existing control-plane project. These are component gates, not an unattended
acceptance certificate.

`make learning_daemon` adds an actual private daemon integration: two sealed
acquisitions, 512 isolated evaluator observations, 768 resident table queries,
unchanged incumbent answers, same-process hot swapping, stale-source refusal,
revision/token-checked rollback and restart. The test coordinates the sequence;
it does not claim that the managed unattended supervisor is integrated.

The trusted parent can freeze a complete proposed inventory with:

```text
bin/cnet_learning_snapshot freeze ABS_COMPLETE_SET ABS_PRIVATE_CACHE
```

This reuses the existing canonical snapshot implementation. It copies opaque
artifact bytes, publishes exclusively and verifies the result; it does not parse
capsule semantics or approve a capability. Its four-line output contains the
full snapshot SHA256 and copied byte count. The helper's native maximum is 32 GiB;
the supervisor must enforce its smaller storage quota before calling it. This is
trusted parent authority, not a sealed evidence worker.

Persist the expected identity before native `STAGE revision 1 named_set`.
The named set is at most 63 characters; the 64-character digest itself is not a
valid control-protocol set name. The set must contain the same per-capsule names
and bytes that were frozen. STAGE does not change the active serving set. Its
returned digest must exactly match the frozen identity.

Evaluate the actual staged snapshot using:

```text
bin/cnet_table_verify snapshot-worker ABS_DATA_ROOT DATASET ABS_NATIVE_SNAPSHOT_CACHE SNAPSHOT_SHA256 EMPTY_ABS_PRIVATE_OUTPUT_ROOT EXPECTED_PARENT CPU_SECONDS MEMORY_MIB
```

The worker seals before reading source or candidate bytes, validates the existing
canonical snapshot before and after execution, and observes all 256 native table
queries. Exit zero means complete observations only. The bounded ASCII report
has a version header, snapshot hash, dataset, source hash, `results 256`, exactly
256 ordered result rows, and `end`. Each result row is `KEY<TAB>1<TAB>VALUE` or
`KEY<TAB>0<TAB>-`. There is no worker-provided approval flag.

The managed `LearningTableEvaluation` factory independently compares every result
to `LocalTableReference`, including verified zero versus refusal, and binds the
receipt to the exact private output bytes. The caller must also require clean
child exit, unchanged source identity before/after evaluation, and the actual
staged identity. Native growth checks preserve old sealed obligations. Only then
may a separate persisted activation intent be sent. Runtime installation,
supervisor orchestration and actual 72-hour acceptance remain unfinished.
