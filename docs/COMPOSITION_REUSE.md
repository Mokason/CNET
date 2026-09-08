# Reusable verified capsule compositions

The operator can export the capsules used by a successful typed request and
reuse that subset offline. Every subsequent request still goes through the
existing planner, certification audit and actual-input coverage at every hop.
This does not turn a successful answer into training truth or broaden coverage.

The result is an ordinary directory containing the original capsule directories:
`unit.cnb`, `manifest.cknow` and, when present, `frontend.cvfa`. There is no recipe
language, new capsule schema, fused kernel or answer cache. Unused alternative
paths are not exported. Capsule files originate from the existing
`cnb_export_subset`/capsule export path and are copied byte-for-byte here.

## Useful two-hop example

Run from a development checkout. All labels below are independently calculated
integer arithmetic, not earlier CNET answers. No network or GPU is required.

```sh
make capsule_core
reuse_demo=$(mktemp -d /tmp/cnet-composition-example-XXXXXX)
mkdir -m 700 "$reuse_demo/saved"
for h in $(seq 0 17); do
  printf '%d %d\n' "$h" "$((h * 60))"
done > "$reuse_demo/hours.tsv"
for h in $(seq 0 16); do
  printf '%d %d\n' "$((h * 60))" "$((h * 3600))"
done > "$reuse_demo/minutes.tsv"
bin/cnet_capsule_core teach "$reuse_demo/capsules" hours_minutes \
  hours minutes 5 10 verified_tool "$reuse_demo/hours.tsv"
bin/cnet_capsule_core teach "$reuse_demo/capsules" minutes_seconds \
  minutes seconds 10 16 verified_tool "$reuse_demo/minutes.tsv"
bin/cnet_capsule_core reuse "$reuse_demo/capsules" "$reuse_demo/saved" \
  'capsule hours seconds 2'
```

The receipt reports `verified=1`, `value=7200`, `hops=2`, selected unit names,
copied artifact bytes and `snapshot_sha256`. Substitute that SHA for `DIGEST`:

```sh
bin/cnet_capsule_core ask "$reuse_demo/saved/DIGEST" 'capsule hours seconds 3'
bin/cnet_capsule_core ask "$reuse_demo/saved/DIGEST" 'capsule hours seconds 17'
```

The first request returns 10800. The second must exit nonzero: 17 hours reaches
1020 minutes, outside the second capsule's coverage. Input 18 is outside the
first capsule's coverage. Only hours 0–16 have a verified complete path here.
Different covered inputs are replanned; export does not install a fixed route.

## Publication and trust boundary

`reuse ABS_SOURCE ABS_DESTINATION REQUEST` requires existing absolute,
owner-private directories on Linux. Keep the destination outside the source
inventory. The operation is explicit: ordinary asks never export, approve
learning, create demand, train or activate anything.

Before parsing the source, a bounded scan validates its complete inventory.
Even unselected capsules and dot entries must satisfy the existing snapshot
rules; only the exact private publisher lock is recognized as metadata.
Symlinks, hardlinks, FIFOs, unknown artifacts and unsafe ownership/permissions
refuse. Sources must remain owner-stable during the operation. Pinned descriptors
and per-file stability checks are not a transaction against malicious same-UID
writers or an authenticity signature.

The selected files are copied into a private temporary directory. A fresh core
must import that copy, match every selected canonical identity and reproduce
the verified value, hop count and unit receipt. Only then can the snapshot be
published with a no-clobber rename. Any validation refusal removes only the
temporary copy created by that call. Existing snapshots are never overwritten.
Identical retries revalidate the candidate and existing digest. Post-rename
sync/close errors can leave a complete artifact while reporting refusal; retry
identically to repair durability. An output-write failure can also occur after
publication, so missing CLI output alone does not prove no artifact exists.

Source-backed capsules keep their original asset and source pins. Changing or
removing the configured source causes refusal on reuse. Arithmetic capsules
need no source directory after export; this does **not** imply that source-backed
facts remain usable without their configured source or after it changes.

## Bounds and diagnostics

At most eight distinct capsules can be selected from a route of at most eight
hops. The inventory ceiling remains 4096; the snapshot boundary reads at most
32 GiB total, with 64 MiB per CNB and 16 MiB per other artifact. Whole-source
validation includes unselected bytes; export is an operator batch operation,
not a constant-time query. Request length is at most 255 bytes. Existing typed
port, search-work, state and certification limits are unchanged.

The CLI emits one structured key/value receipt. `REUSE_REFUSED reason=...`
distinguishes argument, ownership, inventory, source-core, uncovered request,
selection and combined candidate-validation/publication failures. It prints no
raw request or source text. The API clears prior answer fields on refusal.
Snapshot SHA binds names and bytes; it is neither authentication nor a training
eligibility receipt. There is no persistent composition-history database.

## Verification and remaining scope

`make capsule_reuse` runs actual-native arithmetic/source-asset tests and direct
validator-fault tests. `make capsule_reuse_coverage` instruments the changed
boundary in a private directory. `make capsule_product_sanitize` separately
instruments the complete linked native runtime with ASan/UBSan.

See the [measured result](../result/cnet_composition_reuse_20260909.md),
[decision](../plans/cnet_composition_reuse_20260909.md) and
[core contract](CAPSULE_CORE.md). This milestone covers reusable finite numeric
workflows, not general document tasks, arbitrary multi-field inputs, independently
frozen paraphrase performance, source-conflict arbitration or improved learned
task selection. Those remain in the [current queue](../tasks/todo.md).
