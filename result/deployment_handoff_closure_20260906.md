# Explicit native-package private handoff — 2026-09-06

Scope: product closure plan item 12's bounded deployment preparation and manual
rollback guidance. This is not authorization or evidence for a live rollout.
No service, timer, live data directory, configuration merge or current-release
symlink was touched. Only newly allocated private test fixtures were published.

## Implementation and trust boundary

`scripts/deploy_cnet_minimal.sh` replaces the removed latest-pointer workflow
with required `--artifact`, `--sha256`, `--destination` and optional `--dry-run`.
There are no defaults or verification bypasses. It never invokes the packager.
The expected archive hash must come from a separately trusted owner channel;
the archive's own manifest is integrity evidence, not author authentication.
Non-dry operation runs approved packaged code with the invoking user's
authority. Offline environment flags are not a hostile-code sandbox.

The host-only validator is embedded Python stdlib (`/usr/bin/python3 -I -B`).
The package/runtime itself has no Python dependency. Rather than depend on
version-specific `extractall` defaults, the helper validates physical tar
headers and extracts regular files/directories manually through no-follow,
exclusive, descriptor-relative operations. GNU long-name records are needed by
the existing packager and are explicitly bounded. Other extensions, links,
special files, duplicates, absolute/traversing paths, multiple roots and
trailing streams are refused. The required native ELF inventory, shell/config
inventory, complete status, fixture-only VERSION record, pack indexes and exact
manifest file set are checked before any package execution.

Archive reads pin a regular owner-owned, single-link descriptor and compare
initial/final file metadata as well as the expected SHA-256. Destination parent
must already exist, be owner-owned and private; ancestor symlinks, foreign-owned
ancestors (except root), and shared writable non-sticky ancestors refuse.
Root/home/workspace, existing targets and targets within this repository refuse.
Destination ancestor `.git` markers also refuse, through metadata-only nofollow
checks; no checkout is traversed. Paths are deliberately narrower than
all legal POSIX names: safe ASCII components <=96 bytes, total input <=4095.

Bounds: compressed input 32 MiB, expanded tar 128 MiB, individual member 32 MiB,
4096 members, member path <=2047 bytes and <=20 components, manifest <=1 MiB,
smoke merged output <=256 KiB, smoke deadline 60 seconds, validation/handoff
deadline alarm 120 seconds after interpreter startup. Cleanup and uninterruptible
kernel I/O are not a hard wall-time guarantee.
The expansion test uses the actual 128 MiB production cap. These are explicit
input/workflow bounds, not a measured whole-process resident-memory ceiling.

Dry run performs reads/validation only: no explicit filesystem writes,
extraction, package execution, or native compatibility claim. A real invocation
creates only a random `0700` same-parent stage, writes/syncs files with private
permissions, verifies it, then runs the actual packaged offline smoke using the
helper's pinned `/proc/<pid>/fd/<stagefd>/.` path. It clears inherited runtime
configuration, observes exit using `waitid(WNOWAIT)`, and kills the remaining
same process group before reaping the direct child. This keeps the PID reserved
until group cleanup; it does not claim to reap all grandchildren or contain malicious
process-group escapes. All member bytes, types, permissions, links, ownership
and the root's private mode are checked again after smoke.

Publication uses Linux `renameat2(RENAME_NOREPLACE)`, then syncs both the new
deployment and parent directory. Deadline and SIGINT/SIGTERM are blocked across the
rename/committed-state bookkeeping interval. Pre-publication failure removes
only the exclusively created stage. Post-publication sync failure/deadline/interruption
returns distinct exit 3, reports COMMIT_UNCERTAIN and retains the complete
deployment. It never reports successful rollback or deletes committed files.
The owner must not concurrently mutate the selected artifact, pinned parent
or deployment during this workflow; this is not a hostile-same-UID service.

## Actual RED before production behavior changes

The legacy helper was **never run against its real repository or default live
destination**. The dry-run baseline copied its source into a private fake
repository and supplied a private `package_cnet_minimal.sh` stub that exited 97
at the legacy helper's first action, before its broad destination logic. The
explicit dry-run test failed with `DEPLOY_DRY_RUN_RED` (exit 97 instead of 0;
one test, 6.182 seconds). No legacy deletion, live-data copy or service operation
was reachable in that test.

Before the next production corrections, four focused tests actually produced
three assertion failures and one exception (6.870 seconds):

- `DEPLOY_REQUIRED_TYPE_RED`: a manifest-consistent directory replacing a
  required native executable passed dry validation.
- `DEPLOY_SMOKE_CHILD_RED`: a completed smoke left a same-group child alive.
- `DEPLOY_STAGE_MODE_RED`: post-smoke verification did not bind executable mode.
- Injected deadline after publication escaped the COMMIT_UNCERTAIN classifier.

A further independent root-mode assertion produced
`DEPLOY_ROOT_MODE_RED` (one test, 6.150 seconds), before adding stage-root
ownership/private-mode verification. No certification or smoke floor changed.

The child-identity ordering assertion then failed with
`DEPLOY_CHILD_IDENTITY_RED` (one test, 6.324 seconds), before replacing premature
`wait()` reaping with `waitid(WNOWAIT)` plus group cleanup before direct reap.

Independent review found the interrupted-publication cleanup error. Actual
SIGINT at the successful rename return boundary caused `FileNotFoundError`
after cleanup had emptied the newly published test deployment; SIGINT during
post-publication sync escaped as `KeyboardInterrupt`. Both failed
`DEPLOY_INTERRUPT_RED`. A private alternate-checkout dry run simultaneously
failed `DEPLOY_OTHER_WORKSPACE_RED`: two test methods, three failures, 6.368
seconds, log `/tmp/cnet-deployment-interruption-red-20260906.log`. These tests
signalled only their own trusted test process, never any live process. A
synthetic no-chown ancestor-owner test separately failed
`DEPLOY_ANCESTOR_OWNER_RED` (one test, 6.029 seconds) before the ownership check.
RED outputs other than the named interruption log were retained in the tool
transcript, not separate filesystem log files.

## GREEN and reproducibility

On Linux `6.17.0-20-generic`, `/usr/bin/python3` `3.12.3`:

```sh
bash -n scripts/deploy_cnet_minimal.sh
/usr/bin/python3 -B tests/test_minimal_deploy.py
```

Shell syntax passed. Full focused suite: **18/18 test methods PASS, 15.506
seconds**, log `/tmp/cnet-deployment-green-20260906.log`. Test setup invokes the
repaired native packager against new private output/archive paths; successful
handoff runs its real native smoke and independently checks the resulting
manifest with `sha256sum -c`. Previous deployment sentinel stays byte-identical;
a retry refuses. Its nine LOCAL/six exact-gold/nine CERT/four OOD fixture checks
are the existing native smoke, not new knowledge certification.

The parent's integrated `make cnet_minimal_deploy_gate` also passed **18/18,
15.668 seconds**, log `/tmp/cnet-deployment-integrated-20260906.log`. The legacy
flagless `make cnet_minimal_deploy` was invoked after its replacement and
correctly returned exit 2 with explicit CLI usage; it ran no deployment helper.
Frozen reviewed source SHA-256 identities:

- Helper: `18fdee6617d9ec2d3bc5206bce35ea14e20106432a53e2db4971b9bc467f8ef0`.
- Tests: `b9db1215f93ac553cb3c9b6ed6404156d9e2533c4b66a63ec7cfe2e263821fb9`.

Negative cases cover missing args/wrong trusted digest, root/home/workspace and
existing destinations, symlink ancestors, shared writable parent, traversal,
absolute paths, symlink/hardlink/FIFO payloads, duplicate/missing/extra members,
manifest mismatch, directory executable, PAX/global PAX/sparse/longlink types,
truncated/trailing/concatenated gzip and actual expansion/size bounds. Smoke
nonzero-with-success-text, missing receipt, output limit, timeout and surviving
same-group child are tested, including PID identity at the actual group signal.
Actual private CLI failure refuses publication and
removes only its newly created stage.

Publication fault tests replace trusted Python function references inside the
test process; production has no test flags or fault environment variables.
They exercise pre-sync failure, post-rename I/O failure, post-rename deadline,
actual SIGINT and SIGTERM both at successful-rename return and post-rename sync,
and a newly created destination racing the no-replace rename. Independent
`sha256sum -c` verifies complete retained files after each signal case. Complete
published data is retained on uncertainty; the raced owner's sentinel remains
the only file in that destination. No live fault injection was performed.

Compatibility was also checked by validation-only invocation of the native
workflow agent's retained independent fixture archive:
`/tmp/cnet-native-workflows.T2ezkC0q/package.tar.gz`, SHA-256
`40c0371d9bb8c43a83046765e4249f2bcab30a54199029d9f734838295cb8116`.
Result: `CNET_MINIMAL_DEPLOY_DRY_RUN_PASS`, 529 members, `writes=0 executes=0
live_switch=0`. Destination `deployment-check` was not created. This is a
trusted test artifact identity, not a signed public release.

Fixtures are removed by their owning test TemporaryDirectory cleanup. Existing
native smoke independently retains its newly created `/tmp/cnet-runtime.*`
diagnostic copies; no broad temporary-directory sweep was performed.

## Review and withheld claims

The parent approved the bounded interface/trust design before implementation.
The native-workflows agent independently reviewed the artifacts, required the
interruption fix, then checked the frozen helper/test hashes, actual RED/GREEN
logs and corrections with no remaining required code/test finding. The parent
additionally required ancestor ownership checks. Security, incremental
implementation, TDD/debugging, documentation and adversarial-review skills drove
explicit failure tests and retained uncertain publication. Cross-model review
was skipped in this delegated non-interactive context; no external CLI was
called. Shared Make integration belongs to the parent, not this slice.

Manual rollback is only an owner's selection of a prior untouched deployment
for a separately approved consumer. No consumer was redirected. Live target
approval, service integration, configuration migration, host portability beyond
this Linux setup, durable recovery under actual power loss, prolonged workload
stability, and public-release/security clearance remain **WITHHELD**.

Primary API references: Python documents tar format/type handling and extraction
default changes in [tarfile](https://docs.python.org/3/library/tarfile.html), and
bounded decompression/stream completion in
[zlib](https://docs.python.org/3/library/zlib.html). No extraction default is
trusted here. Linux documents the no-replace operation and its support
requirements in [rename(2)](https://man7.org/linux/man-pages/man2/rename.2.html).
The non-reaping exit observation is documented in
[Python os.waitid](https://docs.python.org/3/library/os.html#os.waitid).
