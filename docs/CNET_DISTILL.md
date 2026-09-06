# Distillation proposals

[tools/cnet_distill_slice.c](../tools/cnet_distill_slice.c) prepares bounded
teacher proposals from explicit queries, query files or distinct miss-log
queries. It does not train a student, seal a capsule, install a skill or admit
a draft as verified gold.

Run the isolated regression gate with:

```sh
make distill_slice
```

The default mode and `--dry-run` validate inputs and report the query count
without writing output, running a probe or contacting a teacher. An explicit
dry-run takes precedence if both mode flags are present. At least one query is
required; missing query files, malformed JSON miss-log rows, overlong queries
and more than 32 queries refuse instead of inventing or truncating input.
Queries are limited to 511 UTF-8 bytes; input files to 1 MiB and lines to 8191
bytes. Domain names contain 1–64 ASCII letters, digits, hyphens or underscores.

## Coverage and teacher boundary

Teacher mode requires an owner-selected pack inventory and probe executable:

```sh
CNET_PACKS_ROOT=/private/cnet/packs \
CNET_FRONT_DOOR_BIN=/private/cnet/bin/roe_front_door \
CNET_STAGE_HTTP=http://127.0.0.1:8081 \
CNET_STAGE_MODEL=owner-selected-model \
  bin/cnet_distill_slice --domain example --teacher \
  --from-file /private/cnet/queries.txt --out /private/cnet/proposals
```

This example contacts the explicitly configured teacher. Use plaintext HTTP
only for a trusted loopback endpoint; use HTTPS for a remote owner-selected
endpoint. Endpoint and executable configuration are operator authority, never
model-selected values. The teacher defaults to `http://127.0.0.1:8081` and model
`held` if those two variables are absent. No credential storage or proxy
configuration is introduced; curl configuration files and inherited proxies are
disabled for these requests.

Every query is first passed as an argv element to:

```text
roe_front_door probe QUERY --root OWNER_ROOT
```

The probe shares the front door's query normalization, aliases, slot/dialog
preparation, route selection and local pack loading. It cannot attach the
network configuration, append a miss log, emit thoughts or accept/promote
feedback. Live environment flags cannot enable network work on this branch.
Mutation flags and `--no-always` are rejected. A missing matched catalog,
malformed/binary catalog or malformed/ambiguous routes refuse the probe; an optional
absent pack that ordinary ask also leaves unloaded is not treated as coverage.

The only accepted stdout receipts are exactly one of the following, including
its terminating newline, followed by EOF and process exit zero:

```text
CNET_DISTILL_PROBE_V1 LOCAL
CNET_DISTILL_PROBE_V1 UNCOVERED
```

Missing executables or roots, errors, nonzero exits, extra output, NUL bytes,
overflow and timeouts refuse the whole batch. Query text is never echoed into
the receipt. There is no peer/daemon fallback and no `--no-collapse-check` bypass.

All coverage checks finish before any teacher request. LOCAL queries produce
an empty skipped row and never enter candidate gold or contact the teacher.
Only successfully checked UNCOVERED queries reach the teacher. Curl uses argv,
not a shell, and requires HTTP success and process exit zero. The complete
bounded JSON response must contain exactly one `choices` item with a nonempty
string at `message.content`. Duplicate authority fields, malformed or oversized
JSON, empty content and a present `finish_reason` other than `stop` refuse.
There are no teacher-miss placeholders. Responses are capped at 32767 bytes,
decoded answers at 4095 bytes. Every subprocess has a monotonic deadline
(default 25000 ms; `--timeout-ms` may lower it to 1–25000 ms), and timeout or
capture failure kills its process group and reaps the direct child.

## Publication and provenance

After every probe and required teacher response succeeds, the tool creates a
unique private directory under the selected output root. Output traversal
refuses symlink components and `.` / `..`; the final output directory must be
owned by the current user and not writable by other users. Row files and the
pending manifest use exclusive creation. `PROPOSE.json` is atomically linked
into place only after all files are complete; consumers must require that
manifest and ignore incomplete directories. Created ancestor directories, the
completed proposal directory and its parent are synced before success is
reported. If a sync fails after the manifest becomes visible, exit code **3** and
`DISTILL_SLICE_COMMIT_UNCERTAIN` identify a complete retained proposal. Inspect
that path before retrying; the tool does not erase possibly committed rows or
report PASS. No previous proposal is overwritten.

`PROPOSE.json` records successful per-query coverage checks, the skipped LOCAL
count and successful teacher count. `rows.jsonl` records each probe result.
`gold_rows.jsonl` contains only external drafts with `pending_verify`
provenance. Its filename does not make those rows gold. All outputs retain
`auto_cert=false`; row claims remain uncertified.

Coverage receipts describe the selected local inventory at probe time, not a
snapshot guarantee or future admission decision. The owner must supply a stable
offline inventory for the batch, including no concurrent catalog mutation
between text preflight and catalog loading. This probe does not create an
immutable pack snapshot. Synthetic teacher fixtures in the regression
gate prove boundary behavior only; they do not prove real task acquisition or
teacher correctness. Disk power-loss recovery, live deployment, certification
and broader capability claims remain outside this gate.

External teacher drafts, user corrections or verified tool results can feed the
existing typed [capsule teaching path](CAPSULE_CORE.md) after verification.
ROE packs, MTK tensor deltas and CNU1 capsules retain their distinct contracts.
See [TEACH_PATH.md](TEACH_PATH.md) for the supervised acquisition workflow.
