# ROE pack runtime

ROE provides curated text skills, route selection, untrusted acquisition
candidates and explicit abstention. Its text catalogs are not CNU1 neural
capsules; the word CERT in a pack interface must be interpreted according
to that interface's review and persistence rules.

## Surfaces

| Surface | Purpose |
| --- | --- |
| `roe_asi_cli` | Catalog training/lookup and explicitly enabled teacher experiments |
| `roe_front_door` | Selective pack lookup and proposal/review entry |
| `cnetd` | Serial socket session, capsule integration and presentation |
| Native evolution tick | Reviewed evidence to pack growth |
| Goal/self-model tools | Inventory, coverage and bounded goal decomposition |

Source starts at [cnet_roe_asi.h](../include/cnet_roe_asi.h) and
[cnet_roe_asi.c](../src/roe/cnet_roe_asi.c).
Pack manifests, voice text, route tables and query corpora are runtime inputs.

## Local checks

```sh
make roe_asi roe_asi_train roe_asi_cli
make roe_daily_packs roe_front_door
make roe_asi_goal roe_asi_self
```

Some gates create private fixture catalogs under artifacts. A standalone CLI
may also write to its selected catalog; inspect `--catalog` and output paths
before manual use. Do not repeat old live `--accept` recipes as a smoke test.

## Acquisition and trust

Standalone ROE network adapters can use `ROE_LIVE`, `ROE_LLM`,
`ROE_LOOKUP` and configured URL/model settings. A loopback Ollama endpoint
can itself use a cloud model; loopback transport does not prove offline use.
External answers remain untrusted until independent review and the applicable
acceptance checks.

The daemon separately hard-disables its legacy teacher-on-miss branch.
Neither `ROE_LIVE=1` nor `CNET_TEACHER_ON_MISS=1` is proof that branch
will execute. See [daemon](CNETD.md) and [teacher paths](TEACH_PATH.md).

Repeated answer agreement, a route match, a PASS filename or a token-saving
ratio is not independent evidence of truth. Never train on CNET's own Tier-A
answers. See [gold handling](GOLD_CURRICULUM_HARVEST.md),
[coverage harvesting](CERT_COVERAGE_HARVEST.md) and
[distillation limits](CNET_DISTILL.md).

## Document and OCR scope

Native `make roe_asi_ocr`, `roe_asi_ocr_asset`, `roe_asi_ocr_local` and
`roe_asi_ocr_tables` exercise different bounded glyph, document-memory and
table mechanisms. A hermetic alphabet or repeated-page cache is not an external
OCR leaderboard. Removed torch/Unlimited execution paths are not supported
commands; external leaderboard claims remain WITHHELD without their own
frozen, executed protocol.

Old token-economy and OCR comparison tables remain in the
[documentation archive](MAINTENANCE.md). They describe their recorded
fixtures, not current service quality or a fresh external benchmark.

For typed portable knowledge, start with [CAPSULE_CORE.md](CAPSULE_CORE.md).
For presentation and identity, use [utterances](UTTERANCE.md) and
[instrumented inventory](ROE_SELF_MODEL.md).
