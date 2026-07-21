# CNET A–F depth pass (2026-07-21)

## A Structured learn
- `cnet_auto_learn_note_skill` → `skill_<slug>` / `research_*` / `chunk_*`
- MCP `cnet_learn_from_chat` accepts `skill=`
- Token `tk*` still used for freeform

## B JTC true unknown
- Explicit unknown tool name → `source=unknown_tool`, tool null, gap noted
- Empty features → `unknown_empty`
- Low confidence (`CNET_JTC_MIN_CONF`, default 0.20) → `unknown_low_conf`
- Known calculator/etc. still certified

## C Residual
- Deploy/MCP: `CNET_SOUL_RESIDUAL_HERMETIC=1` soft residual without GPU fight
- Optional `CNET_RESIDUAL_GGUF` when dual load allowed

## D G4 charter
- `config/actuator_charter.txt`
- `cnet_charter_allows` / `CNET_CHARTER_ENFORCE=1`
- Wire: gap_inbox can skip unchartered when enforce (see gap_lane note)

## E Ops
- `scripts/cnet_governance_brief.sh` → WEEKLY_BRIEF.md
- `scripts/cnet_pin_prune.sh` keep N pins
- Janitor timer + consolidate remain

## F Research → CNET
- `scripts/queue_research_skills.sh` queues Unity/RPG structured goals

## Verify
```bash
make auto_learn governance cnet_janitor_build cnet_consolidate_build
# rebuild MCP after JsonToolCall change
dotnet publish ... && bash scripts/hotswap_cnet_mcp.sh  # external recycle
bash scripts/queue_research_skills.sh
bash scripts/cnet_governance_brief.sh
```
