# Owner-DM verified learning

This opt-in adapter exposes the existing policy-bounded table learner through
the already authorized owner-only Discord DM. It is not arbitrary text learning,
web-to-truth conversion, general task coverage or a learned allocator gain.
The main peer and read-only MCP paths remain unchanged. The frozen 72-hour soak
is a different installation and must never be used as the bridge's target.

The newer [natural-language task/approval CLI](VERIFIED_TASK_CORE.md) is a
separate source milestone. It has not replaced these deployed exact-command
forms or migrated this installation's ledger.

## Requests and answers

Four exact, case-sensitive command forms are supported. Plain outer ASCII
spaces and the existing configured Discord prefix are presentation padding;
tabs, newlines, Unicode whitespace and extra instructions are not normalized
into valid learning commands.

| Request | Approved finite contract |
| --- | --- |
| `unicode upper 181` | Explicit Latin-1 simple-uppercase field: 924 / U+039C |
| `unicode lower 65` | Explicit Latin-1 simple-lowercase field: 97 / U+0061 |
| `unicode category DIGIT_ZERO` | Unicode NAME token to General_Category: Nd |
| `unicode bidi DIGIT_ZERO` | Unicode NAME token to Bidi_Class: EN |

Numeric input is canonical decimal 0..255. Empty explicit case fields abstain;
they do not implement Unicode's default identity mapping. Symbolic inputs are
the 95 exact printable-ASCII Unicode names with spaces replaced by underscores,
not literal characters: `A` is unknown, `LATIN_CAPITAL_LETTER_A` is known.
The existing [source contract](../data/unicode17/README.md) defines all limits.

A miss makes one existing `learning ask`/`lookup` attempt, records normalized
demand and replies `ABSTAIN ... request recorded`. The existing confined worker,
independent finite evaluation, canonical capsule publication and probation run
under the original owner budget. A later request may receive a verified answer.
The adapter never returns the reference label as a substitute while waiting.
It compares a native-certified answer's exact identity/type/value to the pinned
external source before rendering it. Missing native values are explicitly null.

Capture and persistent deduplication happen before that one attempt. Duplicate
or unfinished requests do not re-execute. Other users, guilds, forwards, webhooks
and other channels never reach the learner. A recognized malformed command or
failed learning result cannot fall through to the main peer. Malformed/failed
native results latch the bridge and attempt a persistent pause. A subprocess
timeout remains an unknown outcome without automatic retry. A pause failure is
logged explicitly; the bridge remains latched. No reply is stored as a label.

## Installation and enablement

Provision a separate private installation using the exact existing
[managed/native contract](AUTONOMOUS_LEARNING.md). Preauthorize only
`unicode17_upper_latin1`, `unicode17_lower_latin1`, `ascii_category`, `ascii_bidi`
in that order, all with `verified_tool` authority. The two symbolic entries pin
the existing keys-only vocabulary. Import the four complete canonical source
tables and the exact `UnicodeData-Latin1.txt` excerpt. Allocator must be disabled.
Do not overwrite another installation, change frozen files or renew a ledger.

The private `bridge.json` contains exactly:

```json
{
  "schema_version": 1,
  "dotnet": "/absolute/trusted/dotnet",
  "managed_sha256": "SHA256_OF_EXACT_managed.json_BYTES",
  "native_sha256": "SHA256_OF_EXACT_runtime.json_BYTES",
  "policy_sha256": "SHA256_OF_EXACT_policy.json_BYTES",
  "sources": {
    "unicode17_upper_latin1": "SHA256_OF_CANONICAL_TABLE",
    "unicode17_lower_latin1": "SHA256_OF_CANONICAL_TABLE",
    "ascii_category": "SHA256_OF_CANONICAL_SYMBOL_TABLE",
    "ascii_bidi": "SHA256_OF_CANONICAL_SYMBOL_TABLE"
  }
}
```

Placeholders above are not valid hashes. Files must satisfy the existing
private regular-file/single-link boundaries. Configure **both**
`CNET_DISCORD_LEARNING_ROOT` (canonical absolute installation root) and
`CNET_DISCORD_LEARNING_PIN` (actual lowercase SHA256 of `bridge.json`) in the
gateway and capture-monitor services. Partial configuration refuses. The gateway
also requires the existing remotely verified capture owner/DM configuration.
The hash binds owner-approved bytes, not builder authenticity. The trusted
dotnet host/framework, Python, kernel and same-UID owner remain outside this
manifest threat boundary. No environment variables or secrets reach child CLIs.

Use the separate `cnet-learning-daemon@INSTANCE.service` and
`cnet-learning-owner@INSTANCE.service` templates. They expect `%h/INSTANCE` and
`%h/dotnet/dotnet`, cap each process group to one CPU, 1 GiB and 128 tasks, and
never restart automatically or enable at boot. Daemon loss stops the owner.
The existing capture monitor additionally checks the learning installation and
owner heartbeat, producing `learning_unavailable` through its existing local
alert path. Give that monitor 64 tasks, 512 MiB and a 30-second timeout for the
bounded .NET status child; it creates no demand. This monitor is not the soak's
hard lifetime interlock or a claim of continuous full-dataset verification.

Before each ask, the bridge checks source/runtime/policy pins and requires a
fresh running, unpaused owner within its original elapsed budget. Heartbeat age
is capped by the smaller of 120 seconds and policy. This is admission gating
before starting the CLI, not a hard-real-time guarantee that an in-flight CLI
finishes before the deadline. Each child has a 12-second boot-time deadline and
32-KiB limit on each output pipe; its isolated process group is killed/reaped on
timeout/overflow. Already accepted native work remains governed by its existing
durable control-plane budgets and reconciliation, never by replaying a request.

Use the existing `pause`, owner service stop and `quiesce` operator sequence.
Require settled quiescence before stopping a healthy daemon. Preserve terminal
ledgers, failed attempts and recovery evidence; starting a new authorized
installation is an explicit operator decision, not automatic budget renewal.
The bridge refuses when the original owner budget completes. Roll back only the
new gateway, capture-monitor and capture-watchdog drop-ins, not all service
configuration, and preserve capture.

## Evidence and tests

Freeze gateway, journal, monitor, dataset, legacy evidence, learning bridge,
Unicode query and Unicode evidence modules together. A changed closure or
capture segment restarts capture continuity without erasing its old history;
it does not restart or alter the separate frozen soak.
Switch the gateway, capture-monitor **and capture-watchdog** service commands to
that same frozen release: an old watchdog correctly refuses a new observer pin.
Preserve the independent existing alert-helper override.

Run `make -C experiments/offline_controller discord-capture-test` with
`DISCORD_PYTHON` set to the existing websocket-client environment. Negative
fixtures must remain in disposable stores. Real-daemon rehearsal demand is
explicit synthetic operational traffic, excluded from genuine capture evidence.
Extended immutable exports use `--unicode-source` on both export/qualification
commands as described in the [capture runbook](../tools/discord_capture/README.md).
Origin review, native four-family allocator trajectories, useful headroom,
bounded AMD fitting and fresh independent confirmation remain separate gates.
