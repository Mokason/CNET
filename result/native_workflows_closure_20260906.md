# Native workflow closure — 2026-09-06

Scope: closure-plan slice 2, on `feature/product-closure-20260906`, source
baseline `7c24204692a5b05e009627f43c98660bfcbea66d`. This report covers native
ROE example pack workflows, not CNU1 capability acquisition or live deployment.

## RED before production changes

`bash tests/test_native_workflows.sh --harvest-only` exited 1 with
`NATIVE_WORKFLOWS_RED native_harvest_failed`. The retained output is
`/tmp/cnet-native-workflows.rEIGm6BG/harvest.out`: the old script invoked the
removed `tools/roe_daily_packs_seed.py`.
SHA-256: `0576f4fc1e63d77e9ff22b7855e24b2ee1b25645c1f91eca8e1b886ce704036b`.

Before package edits, `bash tests/test_native_workflows.sh` exited 1 with
`NATIVE_WORKFLOWS_RED package_boundary`. The old packager attempted phony Make
gates before checking the existing output. The test supplied a failing `make`
stub to prevent those gates from modifying default repository artifacts; it
failed the required `output_exists` assertion.
Retained output: `/tmp/cnet-native-workflows.Gqap2tkM/existing.log`, SHA-256
`1d51345cbb7328e3f03d9ef2511e6a35a8ce49607d7334746675565cd1fc20ed`.

## Implemented behavior

- Harvest uses the existing native seeder, gold writer and gardener on an
  exclusively created pack root. Six fixed reviewed gold examples are supplied
  independently of LOCAL answers; demand JSONL carries no answer. Exactly six
  `gold_file` admissions are required before nine verified LOCAL answers, six
  exact gold-answer comparisons, nine CERT dispatches and four explicit OOD
  abstentions pass. Existing roots are refused, not reseeded.
- Packaging requires seven native executables, copies selected configuration,
  creates fresh example packs and runs installed smoke before archiving. No
  runtime registry is copied and no missing binary/build failure is swallowed.
  Existing output/archive paths, symlink ancestors, overlapping paths, single
  quotes and control characters refuse. Directories use private permissions;
  the archive is opened exclusively. No recursive deletion or latest pointer
  remains. A failed new stage stays available for inspection.
- Installed `cnet-ask` resolves package paths from its location and forces
  offline settings. Smoke and soak copy example packs into new private `/tmp`
  workspaces. Smoke checks the native blocklist, nine LOCAL/four OOD queries,
  chain skeleton trace, gardener dry-run and stream fixture benchmark. Dry-run
  reports may be written in the private copy, but admitted catalog and state
  must stay unchanged. Soak requires two native ticks, exact state advancement,
  blocklist skips, 18 LOCAL/eight OOD probes and unchanged six-row personal
  content. Every required subprocess must exit successfully.

Fixture identity: `packaging/CNET-Minimal/coverage_gold.tsv`, SHA-256
`6b5a04c7b1f1a8932881b2bd7092bdc0308354efdfb1a51a6700478f2c3465f7`.
The teacher-rate example explicitly describes a target; the alpha example
explicitly disclaims task-acquisition evidence. No certification floor changed.

## GREEN verification

`make -s native_workflows_test` passed after implementation and Make integration:

```text
NATIVE_WORKFLOWS_PASS outside_repo=1 fixture_only=1 live_calls=0
NATIVE_WORKFLOWS_ARTIFACTS path=/tmp/cnet-native-workflows.T2ezkC0q
```

Top-level log: `/tmp/cnet-native-final-review.log`, SHA-256
`e0391e6c2b0dc1f516fd64e9dadd9a62ccdb7cd3548c5a3e2d1baac4cade501f`.
The retained artifact directory contains harvest, refusal, packaging, installed
wrapper, smoke, soak and injected-fault logs; `package/MANIFEST.sha256` records
the exact packaged binary/data/config/script identities. The test extracts
`package.tar.gz` and executes the installed wrapper and gates from an unrelated
directory. The full manifest verifies before and after smoke/soak.

Archive SHA-256:
`40c0371d9bb8c43a83046765e4249f2bcab30a54199029d9f734838295cb8116`.
Manifest SHA-256:
`78a9eb87b98e0bfe6cd591fc2f2c09df641a26a0cda6e86c7c5f969ff2b2dfb6`.

Refusal cases preserve sentinels for an existing directory and archive, reject
root/workspace/home and `..` aliases, reject symlink output/archive ancestors,
overlap and quoted paths, and reject a missing required binary before creating
the stage. Installed smoke rejects missing gardener/packs and injected OOD and
domain-route subprocesses that print success-like output but exit nonzero.
Harvest also rejects a pre-existing root while preserving its sentinel.

`bash -n` on the owned shell scripts and `git diff --check` passed. Shellcheck
is not installed and was not reported as a pass. The seeder's existing
pedantic initializer warnings did not prevent the native build.

## Limits and handoff

These are Linux native binaries plus Bash/core utilities and jq, not a promise
of a dependency-free artifact or arbitrary-host binary compatibility. The
installed documentation records libcurl as a possible linked dependency.
The manifest detects changes, not provenance authenticity. Output path checks
assume an owner-controlled parent directory; hostile concurrent replacement of
filesystem ancestors is not certified by this shell packaging gate.

No live registry, teacher, reviewer, restart, service, GPU or user data was used.
Example ROE packs remain distinct from CNU1 certified units and MTK cartridges.
Live migration, autonomous cycles, production local-hit floors, new task
acquisition, prolonged resource stability and broader portability remain
WITHHELD. A routine OOD `cnet-ask` appends package-local demand and therefore
changes its miss-log checksum; keep an untouched extraction for integrity checks.

Out-of-scope caller found: `scripts/deploy_cnet_minimal.sh` still consumes
`dist/CNET-Minimal-latest.path`, recursively replaces an installed version,
merges live personal/gold data and restarts timers. It was not run or modified.
The parent was notified that closure stage 12 must replace or fence this legacy
deployment path before presenting a concrete rollout procedure.

The parent reviewed the artifact contract and concrete static gold/harvest
before commit, and owns shared Make integration and final product acceptance.
Changes followed the Git workflow, incremental implementation, debugging and
code-review skills. The incremental skill's referenced definition-of-done file
was absent; this slice used the repository's explicit RED-first and native
gate requirements. Private test artifacts were retained; no existing user data
was deleted.
