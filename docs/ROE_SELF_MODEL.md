# ROE self-model (instrumented inventory)

Closes the self-model loops without consciousness claims.

## Law

- **never self-CERT**
- **second_brain: false**
- Tree ranks rise only from **gate evidence files** or explicit doctrine roots
- `beat_quality` stays **0** unless `artifacts/omnidoc_full/SOTA_CLAIM.txt` contains `OVERALL_GE_LEADERBOARD=1`

## Loops closed

| # | Loop | Mechanism |
|---|------|-----------|
| 1 | Per-turn inventory | `roe_turn` → `RoeReply.source_name` + `inventory_line` |
| 2 | Gate-bound skill tree | `roe_self_apply_gate_evidence` probes `logs/*PASS*` + reports |
| 3 | Coverage + pack | `roe_doc_coverage` + `roe_self_export_pack` → `SELF.abi` |
| 4 | Skill health | 5-layer scan REGISTRY→UTILITY on `RoeSkill` |
| 5 | Goal HAVE/MISS | `roe_self_goal_probe` (no auto_learn) |
| 6 | Reflection thought | optional `agent_record_thought` via `--thoughts` |

## Gates

```bash
make roe_asi_self        # ROE_ASI_SELF_PASS
make roe_asi_self_cli    # snapshot + validate → artifacts/roe_self_model/
```

## CLI

```bash
./bin/roe_asi_self_cli snapshot --catalog artifacts/roe_catalog \
  --out artifacts/roe_self_model --repo . \
  --goal "ocr document local packs" --thoughts

./bin/roe_asi_self_cli ask "who are you" --catalog artifacts/roe_catalog
./bin/roe_asi_self_cli validate --out artifacts/roe_self_model
```

## Pack layout

```
artifacts/roe_self_model/
  SELF.abi
  self_report.json
  MANIFEST.txt
  skill_tree.jsonl   # when tree saved
  doc_pack/          # if doc asset bound
```

## API

See `include/cnet_roe_self.h`.

## SOUL persona pack (`pack_soul_marble`)

Isolated SOUL.md-style capsule (kind=persona). Delivery + oath only; **seal_path forbidden**.

```bash
make roe_soul_pack          # ROE_SOUL_PACK_PASS
./bin/roe_front_door ask "who are you"     # Marble LOCAL
./bin/roe_front_door ask "one line marble"
```

Always-on with self/goal/toolcall. Source: `config/voice_marble.md` + SOUL.md in pack.
Swap souls later via another `pack_soul_*` (one active persona preferred).
