# Private deployment — 2026-09-07

**Deployed and operational at the recorded check; not 72-hour acceptance.**
No live service, existing configuration, main worktree or remote branch was
changed. The learned allocator remains disabled; no GPU job ran.

## Installation and lifetime

- Installed source: `bfefaa66eb43b53b00d16a58236a2f81c24fa80d`.
- Private root: `/home/marble/cnet-private-20260907-eTgwwG`.
- Daemon user unit: `cnet-learning-private-eTgwwG-daemon.service`, observed PID
  2698931, started 07:05:46 EEST.
- Learner user unit: `cnet-learning-private-eTgwwG-owner.service`, observed PID
  2706523, started 07:07:48 EEST with its original **600-second** learning budget.
- Existing cnetd PID 2895419 remained running and was not targeted.

Both new units were active/running at 04:10:17 UTC (07:10:17 EEST). The learner
had 74 persisted ticks, two jobs, no pending mutation and no outstanding
probation. The serving daemon is deliberately left available; the learner's
budget is not renewed. Ordinary expiry is expected around 07:17:48 EEST, but
boot-clock continuity, failure and owner stop govern the actual result. This
record does **not** assert that the future deadline completed successfully.

These are transient user services with Restart=no, not boot-enabled units;
login/linger settings were untouched. Each has a one-CPU-equivalent quota,
768 MiB cgroup memory cap, 128-task cap, NoNewPrivileges, AF_UNIX-only socket
families, UMask0077 and explicit private WorkingDirectory. Owner stop uses
KillMode=mixed and a 60-second systemd grace period. Policy bounds are 256 MiB
logical work storage, 16 jobs, four attempts/hour, four promotions/day, two
attempts/source, 10-second/512-MiB workers, two-second ticks, three probation
probes and a 30-second maximum gap. Logical accounting is not a kernel quota.

## Actual operational check

The only authorized dataset is synthetic `byte_squares`, authority
`verified_tool`. An awk integer calculation generated keys 0..15 and then
0..31; no CNET answer supplied labels. Independent JavaScript integer
multiplication checked every answer and every omitted-key abstention through
the actual installed CLI/native serving path.

| Phase | Checked inputs | Correct verified answers | Correct abstentions | Wrong results |
| --- | ---: | ---: | ---: | ---: |
| Initial 16-row source | 256 | 16 | 240 | 0 |
| Refreshed 32-row source | 256 | 32 | 224 | 0 |

Initial key 7 was uncovered. The existing standalone tick activated the first
capsule, then three real, correctly spaced probes accepted it. Its native
revision moved 1→2. The first whole-domain check took 14.913 wall seconds using
four concurrent installed CLI processes, not kernel-only serving latency.

With no periodic owner yet, the source was atomically replaced after retaining
its original bytes. Previously covered key 7 then abstained while both recorded
native STATUS frames were byte-identical at revision 2. This establishes stale
source refusal, not merely absence of a newly introduced key.

The actual periodic learner then acquired the refreshed source and moved native
revision 2→3. Its journal records activated→probation→probation→accepted→idle.
The second whole-domain check took 16.419 wall seconds with concurrent asks
while the learner continued. All 16 original rows were retained and all 16 new
rows answered correctly. These finite validation tables do not establish
arbitrary-domain learning, factual-source authentication or learned allocator
improvement.

An author-separated Astra review recomputed all 512 recorded results, checked
sources/pins, the stale refusal and unchanged native revision, and reconciled
all 74 recorded heartbeat ticks. The maximum recorded tick gap was 2.042 seconds.
The review found no remaining actionable launch blocker. No external-model
review was invoked, honoring the owner's preference.

## Build and security preflight

`make learning_native` passed its sandbox, reader, producer and verifier/
snapshot gates. The locked framework-dependent managed publish succeeded;
the production lock SHA256 remains
`1b7527d6bffa545f9c7283d8676bd8d5eae3d0ab93c5ec96cc0912d95c139552`.
The normal transitive NuGet advisory audit reported no vulnerabilities. This
matches known advisories only, not unknown flaws or all OS/framework libraries.

Eight managed and six native files were copied into the exact private
inventory, with the required immutable modes and no hard-link deployment.
The installed real entry assembly passed inspect, loaded its packaged SQLite
3.53.4, and initialized a fresh schema-2 ledger with all three policy/runtime
identities below. Installed code and policy were not subsequently changed.

Preflight rejected `/home/marble/AI` as an installation ancestor because it is
group-writable. No ancestor permissions were weakened or changed; the original
`/home/marble/AI/cnet-private-20260907-Jat5f2` is retained as build staging only.
The final root is directly under the trusted home path.

Fresh launch review also found that environment isolation alone would not
prevent cnetd's relative configuration reads. Both services now explicitly use
the private working directory; the general operator guide was corrected.

## Identities and retained evidence

All paths in this table are relative to the private deployment unless named
as a native snapshot. Hashes identify bytes, not source authentication.

| Artifact | SHA256 |
| --- | --- |
| `managed.json` | `c018206ead91a060c725f31eb705131e5daf6a5e20985b5f8cf99e9cb084947a` |
| `runtime.json` | `396c5950bd0e3a1c80368908fa71d196031363a50249dbdebb3b5b41120721ea` |
| `policy.json` | `3dbb27b438814ac8e5a77b56fffda00b6965711f7aaca650277bcc51be536390` |
| `receipts/source-v1.tsv` | `84aebfecd7aa2931393a433da5b4f9b0bb672f0db484587e1e83fdf37feb2dc7` |
| `work/data/byte_squares.tsv` (v2) | `552a1fc2d4a30a656ecd4d83065ffa31a13ebc32e7e70e85521d5d65e5e445c1` |
| `receipts/validation-v1.json` | `50b299105d570297e3604e8cfcb28b7ebad27fde64f5fb1afb1369ff505e87d8` |
| `receipts/validation-v2.json` | `3d12d721b02e4bff346ea624fb715b458ecdf7c7402d7ef1912917c36d330bee` |
| `receipts/status-verified.json` | `144c2f30be7fd32a63011093876a75bd8ab8fadf8f1e763b4f2c005136c2304c` |
| `receipts/owner-verified.log` | `820edd1f3f95880870893953712012b7eecde72b8ccf5b64cc2cf0eb99cdf116` |
| `receipts/services-verified.txt` | `c03ee37205e52f631ec2683f2c670478f99219741f49c5dfaecc75877c0a8ed4` |
| Native active revision-3 snapshot | `d5fe5aae2d1d878546fa9c01c13c1cb8a8ea73edf5fc8d3a0cabaeb2b6ad58e6` |

Boot identity: `31c61efa-181f-4239-9b95-67a7e268ee83`; original run start
768686809509022 ns, recorded heartbeat 768834047297501 ns. Later live state may
advance. The retained snapshots do not impersonate continuous monitoring.

At the recording, `du -s` reported 2,060 KiB managed, 2,184 KiB native, 428 KiB
work and 216 KiB receipts. These are allocated filesystem blocks for this small
validation instance, not RAM-per-capsule measurements or logical quota proofs.

Exact ask/status/pause/cleanup/stop commands are in
[OPERATIONS.md](/home/marble/cnet-private-20260907-eTgwwG/OPERATIONS.md).
Transient unit definitions are retained in `receipts/transient-units.txt`.
The first source remains recoverable in `receipts/source-v1.tsv`; nothing
material was deleted. Real production data onboarding, live-service integration,
useful learned allocator gain and 72-hour acceptance remain outside this result.
