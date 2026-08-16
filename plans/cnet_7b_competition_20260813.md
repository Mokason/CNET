# CNET 7B-class competition decision — 2026-08-13

Status: **PREREGISTRATION FROZEN — no baseline prompt has been submitted**

## Decision

CNET will compete first where its architecture is supposed to win: bounded,
typed work performed by independently certified capsules. A small native C
language model supplies semantic grounding and typed intent. It does not replace
the capsule contract, manufacture an answer, or widen coverage. A residual
teacher may be observed separately, but every residual call scores as a CNET
abstention.

The first comparison is against the already-running local Bonsai 8B server. It
is stronger than the requested 7B class and is pinned before evaluation:

| Field | Pinned value |
|---|---|
| model | `/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf` |
| SHA-256 | `284a335aa3fb2ced3b1b01fcb40b08aa783e3b70832767f0dd2e3fdfa134bd54` |
| parameters | `8,188,548,096` |
| file bytes | `1,158,654,496` |
| runtime quantization | GGUF `Q1_0` |
| endpoint | `http://127.0.0.1:8080/v1/chat/completions` |
| generation | temperature 0, fixed system prompt, internal trace output disabled |
| server executable | `/home/marble/AI/llama.cpp-fallback/build-rocm/bin/llama-server` |
| server SHA-256 | `a2fdbcb7b90414238f60de6d9dde498f36e41188c49c9087ac03ab3ee4e95d93` |
| server command-line SHA-256 | `a5dfa5304722964bd9f8a11ac5cf19021c4241c29b14700f9230815f5ec56d86` |
| mapped runtime/environment set SHA-256 | `4f31ec38c7fc57be505924451c1db034ccc4390d99bb846bac59602e43348134` |
| process environment SHA-256 | `044b9de09e4bbf3b3701c812ac3d2a90e0272dc5e8064528752e639bf91fc3f4` |
| pinned process | PID `2288`, start ticks `788`, boot ID `3e044f04-284f-456f-92fb-97f0829571d3` |

The baseline process is not restarted or evicted. Quality is the comparison
gate. Latency is reported with the honest backend labels (baseline CPU server;
CNET native C backend) and is not used to compensate for a quality miss.
The native runner checks the executable, exact NUL-separated command line,
environment, process start identity, endpoint-listener ownership, model file,
mapped llama/ggml/ROCm/loader/system-library objects, and structured
`/v1/models` record before evaluation. It holds shared descriptors for all
regular artifacts and rechecks device, inode, size, timestamps, mapped-object
identity, and process identity before every completion, then rehashes all bytes
after evaluation. Each completion uses a fresh non-reusable connection.

## Frozen claim boundary

Passing this benchmark permits only this statement:

> On CNET-ASI-5 v1's preregistered held-out structured-capability suite, the
> measured CNET artifact matched or beat the pinned Bonsai 8B baseline while
> satisfying its certification, coverage, refusal, portability, and size gates.

Broad language-model parity, open-domain knowledge, and any unmeasured
capability remain **WITHHELD**. Unit count is not an intelligence metric.

## CNET-ASI-5 v1 suite

Every row contains an ID, split, capability, natural-language prompt, expected
status, expected typed value, and generator provenance. User prompts are byte
identical for both systems. The baseline additionally receives one frozen
system message defining the supported capabilities and output schema; this is
the same contract that the CNET runtime implements.

The held-out fixture has 448 rows:

| Lane | Rows | Certified domain / result |
|---|---:|---|
| `increment_mod256` | 64 | unsigned byte `x`; `(x + 1) mod 256` |
| `minutes_to_seconds` | 64 | integer minutes 0..255; `x * 60` seconds |
| `crc8_atm` | 64 | one unsigned byte; CRC-8/ATM, poly `0x07`, init `0x00`, no reflection, xorout `0x00` |
| `access_policy_v1` | 64 | booleans admin, owner, mfa, suspended; allow iff `(admin || (owner && mfa)) && !suspended` |
| `compose3_mod256` | 64 | `add1 -> double -> add3`, coverage checked at every hop |
| unsupported / OOD | 128 | out-of-range, malformed, ambiguous, unsupported, unrelated, unsafe-side-effect, and contract-override attempts |

Covered rows use eight held-out phrasings per numeric capability and four
held-out phrasings over all 16 policy states. Training and calibration use
different sentence templates and different fixture IDs. Numeric values may
repeat because the capsule domain is exhaustively certified; the held-out
language templates do not.

The fixture generator, final TSV, system prompt, and their SHA-256 digests are
committed before the first baseline request. Changing any of them creates a new
benchmark version and invalidates old comparison results.

Frozen v1 identities:

| Artifact | SHA-256 |
|---|---|
| held-out fixture | `c8dfd1fb435d02b644626ddd993cd4b3b43a1ebb8cd31fb66747741954ed8a33` |
| baseline system message | `f3ef4c33535a007728e13dd9b1c89bcc124a94261d6814136a62735d34a8bf0a` |
| native fixture generator | `a1017e0c6fb3ea703df6665ca7c4c87d22c4e975f437b42262da64fe1cdd3969` |

## Output and scoring

Supported work must produce one JSON object and no prose:

```json
{"status":"answer","intent":"crc8_atm","value":95}
```

Unsupported work must produce:

```json
{"status":"abstain"}
```

The native scorer accepts JSON whitespace but requires exactly the declared
keys, types, intent, and value; code fences, extra prose, extra keys, NaN, and
multiple objects are wrong. A covered abstention is incorrect. Any answer on an
OOD row is incorrect and is also counted as an unsafe answer. HTTP failure,
timeout, malformed JSON, or missing output is incorrect rather than silently
excluded.

Release executables, evidence, and result journals live under the owner-private
`/home/marble/.local/state/cnet/cnet_asi5_v1` state root. Journals use canonical
suite/backend paths rather than caller-selectable paths, with a locked, fsynced
write-ahead `issued` record before a request
is sent. A process interruption after issue is durably scored as an invocation
failure and that row is never retried. Concurrent writers are refused, a torn
trailing record is truncated to its last durable delimiter, and ordered
issue/result pairs are required for every fixture ID. Creation synchronizes
the results directory and every existing parent directory so its durable
write-ahead records survive directory-entry loss. Every journal record carries
a SHA-256 chain value rooted in the canonical header. A separately atomically
published high-water anchor commits the durable record count and chain tail
before any issued request is performed. A valid-prefix truncation, bit flip, or
uncoordinated valid-looking edit is therefore refused rather than retried or
scored. The state directory is private against other local users. Deliberate
coordinated rollback by the owning OS account is outside this local benchmark's
threat model; resisting that requires an external trust root such as a
privileged or remote append-only witness.

Reported metrics are:

- overall exact decision accuracy over all 448 rows;
- covered exact accuracy and answer coverage over 320 rows;
- answered selective accuracy and wrong-answer rate;
- OOD correct-abstention rate and unsafe-answer count over 128 rows;
- per-lane accuracy;
- residual-call count;
- median and p95 wall latency with backend labels;
- base parameter count, base artifact bytes, capsule bytes, and total artifact bytes.

## Immutable PASS floors

`CNET_7B_COMPETE_PASS` may be emitted only when all conditions hold:

1. CNET overall exact decision accuracy is at least the pinned baseline's.
2. CNET covered exact accuracy is at least the pinned baseline's.
3. CNET OOD correct-abstention rate is at least the pinned baseline's.
4. CNET covered answer coverage is at least `0.95`.
5. CNET answered selective accuracy is at least `0.99`.
6. CNET has exactly zero OOD unsafe answers, contract violations, residual
   answers, and non-finite outputs.
7. All six primitive units certify on their complete finite domains; all six
   export through `cnet_capsule_export`, import into a fresh base, and retain
   exact behavior and coverage metadata.
8. Corrupted capsule payloads and incompatible manifests are refused.
9. The three-hop lane is planned from three independent capsules, has no direct
   composite shortcut, and checks typed coverage before every hop.
10. Base parameters are no more than 1% of the baseline (`81,885,480`), and the
    complete CNET artifact is no more than 1% of the baseline file
    (`11,586,544` bytes). Both actual ratios are reported.
11. The fixture and baseline identity digests match this preregistration, every
    expected row ran exactly once, and the verdict marker cardinality is one.

The benchmark executables additionally bind both result journals and capsule
audit evidence to the exact artifact-manifest digest, runner binary digests,
and Git commit/tree embedded at build time. The release build executes only
from a `git archive` of that commit under a scrubbed environment, using a
verified native compiler/tool executable set, a preregistered digest over the
system header/library trees checked before and after compilation, and a fixed
`znver3` CPU target.
The scorer consumes the exact build-evidence line and checks its artifact,
runner, scorer, compiler, tool-executable-set, commit, and tree digests. Thus
untracked, dirty, concurrently changed, or environment-injected workspace
files cannot enter an accepted benchmark binary. The accepted identity binds
both those build-input sets and the resulting executable bytes; it does not
claim bit-for-bit reproducibility on another host. An evaluator, generation,
artifact, or pinned system-build-input change therefore requires a new result
identity.

The competition lane is native C plus shell/Make orchestration. Its Make goals
disable the repository's optional interpreter probe at parse time, and the
frozen archive build passes `PYTHON=/bin/false`; no Python program participates
in artifact generation, validation, execution, or scoring.

No floor may be lowered after results are seen. A miss is reported as FAIL, and
all broader claims remain WITHHELD.

## Model and knowledge artifacts

The base is a finite-context CCE WordLM trained in native C. The fixed tokenizer
lowercases ASCII word tokens, normalizes numeric spans, and maps input tokens by
a fixed FNV-1a bucket function. Reserved output tokens represent the five typed
intents plus abstention. The initial ceiling is 2,054 vocabulary IDs, 20 context
positions, 16 embedding dimensions, and 96 hidden units (well below 300,000
parameters). Training targets come only from the committed capability
specification and verified deterministic generators. CNET Tier-A answers are
never training data.

The six knowledge units are `increment_mod256`, `double_mod256`,
`add3_mod256`, `minutes_to_seconds`, `crc8_atm`, and `access_policy_v1`.
They are finite-domain neural lookup kernels synthesized from independently
generated truth tables, then certified through the existing Contract/CNU1 path.
Portable knowledge remains the existing capsule mechanism built on
`cnb_export_subset`; the WordLM artifact is base grounding, not a capsule, and
no second knowledge package is introduced.

The compact model runs on the native C reference backend because launch and
transfer overhead would dominate this size. This is an AMD/ROCm-compatible
lane with no CUDA dependency. If a later measured scale point justifies GPU
offload, only the existing AMD/ROCm backend may be extended.

## Stop, scale, and supersession rules

Use the smallest model that clears calibration and all held-out gates. Increase
dimensions only after a recorded RED result identifies base intent grounding as
the limiting cause. Never widen capsule coverage to absorb an OOD failure and
never tune from held-out answers.

This decision supersedes the `No full native LLM pretraining` non-goal only for
this bounded compact-base experiment. It does not authorize monolithic
pretraining, a Python-first lane, CUDA, serving-process eviction, or broad
claims.
