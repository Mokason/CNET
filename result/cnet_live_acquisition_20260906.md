# Live capsule acquisition — September 6, 2026

## Outcome

Enabled approved-tool demand acquisition in the live deployment. The missing
deployment wiring was the demand directory and tool-policy environment;
cnetd also needed a restart to load the rebuilt executable and configuration.
No production source changes or certification-floor changes were needed.

Before activation, the actual socket returned `ABSTAIN:
unknown_or_ambiguous_interface` for `capsule bytes bits 12`, without queueing.
The capture assertion failed with `LIVE_ACQUISITION_RED capture_disabled`.
After activation, the same miss returned `demand=queued`. The actual systemd
autoteach service consumed the demand and certified the acquired capsule;
the live socket then returned verified `96`, with `teacher=false`.

## Deployment

- Environment: `config/certified-core-deployment.env` adds
  `CNET_CAPSULE_DEMAND_DIR` and `CNET_CAPSULE_TOOL_POLICY`.
- Demand root: `artifacts/capsule_demand`, owner marble, mode 700.
- Policy: `config/capsule_tools.live.tsv`, owner marble, mode 600.
- Approved rules: bytes → bits (×8, 0–255), u8 → masked8 (XOR 255, 0–255),
  minutes → seconds (×60, 0–31), seconds → frames_24fps (×24, 0–1860).
- Existing timer retained: 20 minutes, scheduling accuracy 2 minutes.
  Limits remain two new jobs and 64 scanned requests per tick.
- Only cnetd was explicitly restarted. Shared MCP retained PID 2119296.
  The existing autoteach service performed its usual learner coordination.

## Verification

Preflight rebuilt `cnetd`, `capsule_core`, and `capsule_tool`, then passed
`tests/test_capsule_demand_growth.sh` and
`tests/test_capsule_composed_acquire.sh`. Logs:
`/tmp/cnet-live-acquisition-build.log` and
`/tmp/cnet-live-acquisition-preflight.log` (temporary, not durable artifacts).

Two explicit starts of `cnet-autoteach.service` exercised the real scheduled
entrypoint. Both finished with `Result=success`, `ExecMainStatus=0`. Its
acquisition receipts, not just the wrapper exit status, showed:

```
CAPSULE_ACQUIRE_TICK processed=2 published=2 examined=2 tool_calls=64 failed=0
CAPSULE_ACQUIRE_TICK processed=1 published=1 examined=1 tool_calls=32 failed=0
```

All three jobs passed history, evaluation and certification gates. The first
two evaluations improved from 0/32 to 32/32; the third improved from 1/32 to
32/32. Each reported zero wrong certified answers. Evidence is in
`logs/autoteach/tick_20260906.log` and the corresponding `tool_*` jobs under
`artifacts/capsule_curriculum`.

Post-restart live socket checks (all verified, `teacher=false`):

| Tested requests | Inputs | Correct |
|---|---|---|
| bytes → bits | 0–31 | 32/32 |
| u8 → masked8 | 0–31 | 32/32 |
| minutes → seconds, original coverage | 0–31 | 32/32 |
| minutes → frames_24fps, original composition | 0–31 | 32/32 |
| seconds → frames_24fps, expanded coverage | 0–31 | 32/32 |

Total: 160 checked queries; 95 newly covered input/interface pairs. These
include the acquired finite rows, not a claim of unseen primitive
generalization. Original capsule `unit.cnb` and `manifest.cknow` files compare
byte-for-byte equal to the pre-activation snapshot.

`capsule bytes bits 256` abstains with `invalid_typed_request`.
`convert 3.5 minutes to seconds` returns `capsule_clarify`.
Neither invokes the teacher. Restart persistence also passed explicit checks
for bytes 12 → 96, seconds 1 → 24 and composed minutes 3 → 4320.

One immediate restart probe produced no JSON, causing `jq` exit 4. The
journal confirms the restart happened; the client exits without JSON on
connection/read failure. This is consistent with socket startup timing, not
a proven cause. Repeated ready-socket checks, including all 160 cases above,
passed. Future restart checks should wait for a successful socket probe and
use `set -o pipefail` to preserve client failures.

Final deployed health passed: one mined unit, one coverage record, zero
unguarded or unreadable mined units, coverage gate on, residual teacher
reachable, no dangerous configuration. cnetd and shared MCP were active,
autoteach completed successfully, its timer was active, and no `.req` demands
remained. A later unattended timer firing was not observed in this check.

## Scope and recovery

Live acquisition is restricted to the four approved integer-tool rules and
their typed compositions. Unsupported requests still abstain; this does not
enable arbitrary knowledge acquisition or unmeasured broader capability.
Broader claims remain WITHHELD. No training used CNET's own Tier-A answers.

The private backup is `artifacts/deployments/live-acquisition-20260906`.
To disable future acquisition, restore the environment file from `before.env`
and restart cnetd. The next autoteach invocation will read the restored
configuration; an already running invocation must finish or be coordinated
before declaring acquisition disabled. Keep learned capsules and the active
base intact. Do not overwrite stores while writers run. `capsules-before`
preserves both original capsule directories. No user data was deleted.
