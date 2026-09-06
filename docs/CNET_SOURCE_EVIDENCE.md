# Bounded local-source evidence

This domain answers five named questions about literal CNET header definitions.
It does not interpret general source code, execute source, preprocess C, certify
unrestricted prose, or claim broader capability. A literal-line receipt proves
that the selected bytes appeared on that source line, not that a compiler used
the definition or that the source author was trustworthy.

| Named fact | Source definition |
| --- | --- |
| `capsule-schema` | `include/cnet_capsule.h:CNET_CAPSULE_SCHEMA` |
| `capsule-asset-schema` | `include/cnet_capsule.h:CNET_CAPSULE_SCHEMA_ASSET` |
| `capsule-asset-file` | `include/cnet_capsule.h:CNET_CAPSULE_ASSET_FILE` |
| `capsule-reason-limit` | `include/cnet_capsule.h:CNET_CAPSULE_REASON_MAX` |
| `json-depth-limit` | `include/cnet_json_internal.h:CNETD_JSON_DEPTH_MAX` |

## Acquisition and transfer

The Linux native producer accepts only these commands:

```sh
bin/cnet_source_capsule list
bin/cnet_source_capsule request capsule-asset-file
# The parent directory must already exist, be owner-controlled, and not be
# group/world writable. The capsule directory itself must NOT exist.
bin/cnet_source_capsule build /absolute/CNET /private/candidates/source_facts source_facts
```

`request` produces the exact typed request
`capsule cnet_source_fact cnet_source_answer 2`. Unknown names, instructions,
paths and trailing text refuse; no fuzzy intent interpretation is performed.

Acquisition independently checks each extracted line by running the fixed argv
`/usr/bin/grep --binary-files=without-match -n -F -x -- LINE SNAPSHOT` on a private
copy of the bytes just read. It requires exact stdout, EOF and exit zero within
five seconds; output is bounded. It records the actual numbered receipt, the
grep executable's SHA256, full-source SHA256, and extractor identity
`cnet_literal_define_v1`. No shell, teacher, network, or source execution is used.
The installed operating system, process environment and fixed tool executable
remain operator-trusted; receipts and digests are not signatures.

Definitions must be unique anchored `#define` lines containing a single decimal
literal (optional lowercase `u`) or a safe quoted filename literal. Values are
bounded to 1,000,000; strings to 64 safe ASCII bytes. Expressions, escapes,
comments on the selected line, multiline definitions, instruction-like strings
with whitespace, and missing/duplicate definitions refuse. Files are at most
1 MiB, lines at most 511 bytes, and NUL-containing files refuse. The extractor
does not evaluate conditional compilation: its claim is explicitly a literal
definition in a named file.

The existing finite-domain BTN construction compiles five exact typed labels,
then the unchanged robust certification floor of 0.05 is enforced. Labels 0–4
use full `PORT_BINARY_MSB`, width 3, count 1 signatures; 5–7 are outside the
coverage domain. Canonical text is exactly `relative-path:MACRO=value`.

The producer uses the existing schema-2 capsule exporter, with asset schema
`0x43534501` in the existing `frontend.cvfa` sidecar. It does not create a second
package. The asset binds source identities, extractor, actual tool receipts,
exact labels and the complete canonical decoder. The CNB/CNU1 seal, existing
manifest checks and certification/coverage replay still apply. The importer
must understand this asset schema or refuse it.

A new output directory is 0700; existing targets, symlink paths, traversal and
shared-writable output parents refuse. The leaf must use the snapshot loader's
safe ASCII component grammar, not start with a dot, and contain at most 96 bytes;
these checks precede acquisition. Export and reimport use the opened output
directory descriptor, not another lookup of its potentially replaced name.
The complete export is reimported,
recertified and rechecked against source before success, with directory/parent
sync. A sync failure after a complete export retains the capsule and returns
`SOURCE_CAPSULE_COMMIT_UNCERTAIN`/exit 3. Export itself retains the existing
multi-file publication bound: observers can see an incomplete directory and
must refuse it. `SOURCE_CAPSULE_PASS pending_activation=1` is not activation or
a statement that an incumbent working set can safely be replaced.

## Serving contract

`include/cnet_capsule_evidence.h` defines an opaque, caller-owned evidence object.
The strict parser copies its input and validates asset schema, supported fields,
receipts/decoder agreement, exact sealed contract labels, and exact active,
non-generalized labelled coverage. The serving core must:

1. Import through the asset-aware existing capsule importer and parse/bind the
   returned asset before exposing a candidate registry.
2. Include its full SHA256 identity in the resident capsule identity. Reject
   different text decoders for the same complete output interface.
3. Reopen and validate the owner-configured source root for every used hop;
   recheck all used evidence before returning the final reply.
4. Render the terminal text only after numeric certification, every coverage
   check and freshness succeeds. No partial answer survives refusal.

Source files are untrusted read-only data: owner-owned group-writable checkouts
are supported without changing their permissions. Source roots and descendants
must be owner-owned; nofollow descriptor traversal, regular single-link files,
bounded stable reads and full SHA256 checks remain mandatory. These relaxed
read permissions do not apply to capsules, snapshots or control state.
Changed/deleted files, replacement/symlink roots, and mismatched literal receipts
refuse. Every check reopens the configured path, not an indefinitely cached fd.

The owner must prevent concurrent source mutation during a request. Initial and
final checks narrow the race window but are not an atomic multi-file source
snapshot, do not defeat a malicious writer's ABA changes, and cannot promise
freshness after the final check returns. Same-owner malicious processes, signed
provenance and arbitrary code semantics are outside this bounded evidence gate.

The focused gates are `tests/test_source_evidence.c` and
`tests/test_source_capsule.py`; expected labels are independently maintained,
not copied from acquisition output or previous CNET answers. These establish
bounded source acquisition and asset semantics. Resident activation, daemon
swaps and cross-hop integration require their separate serving-core gates.
