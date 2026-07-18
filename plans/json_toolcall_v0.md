# Closed-set JSON tool-call spine (v0)

**Place:** Boundary between host JSON (System.Text.Json / MCP) and certified CNET skills.

**Dilemma:** Free-form JSON generation is open-ended; treating residual text as certified JSON is dishonest. Re-implementing a full JSON engine in pure C delays useful tool routing.

**Consequence:** Host owns parse/emit. CNET owns a **closed-set** classifier:

```
jtc_feat  RAW[16]  (keyword bag from JSON text)
    →
json_tool ONEHOT[8]  (calculator | memory_store | memory_recall | file_read | cnet_recall | web_search | wiki_lookup | final)
```

## Gate

```
make json_toolcall → JSON_TOOLCALL_PASS
  encode + hermetic teacher
  mine+admit json_toolcall_v0
  seal CNB → soul_request source=CERTIFIED for all 8 tools
```

## .NET

```csharp
using CNET.Cce;
// Host JSON
JsonToolCall.TryParseAgentJson(json, out var tool, out var args, out var final);
// Certified skill (base must contain json_toolcall_v0)
var toolName = JsonToolCall.Classify(soul, json);
```

Alphabet must stay aligned: `src/json_toolcall.c` ↔ `dotnet/Cce/JsonToolCall.cs`.

## Unification (A–E)

| Step | Status |
|---|---|
| A Agent classify before execute | `CceHost/Agent.cs` uses `JsonToolCall.ClassifyOrGap` |
| B MCP tools | `cnet_classify_toolcall`, `cnet_json_toolcall_status` |
| C Alphabet SoT | `config/json_toolcall_v0.json` → `make json_toolcall_alphabet` |
| D Metrics / recycle | `personal_ai_metrics` + seal script recycle note |
| E Gap path | `JsonToolCall.NoteGap` / unknown tool → inbox `jtc_feat→json_tool` |
| F JTC teacher | `gap_lane_run`: bind `jtc_hermetic_v0` + `drain_jtc_gaps` → `cnet_jtc_ensure_sealed` |

### Gap-lane teacher (F)

Generic `mine_from_oracle` treats `PORT_RAW` as **unbounded**, so JTC gaps used to stay `skipped_no_oracle`. The lane now:

1. Registers hermetic oracle `jtc_hermetic_v0` for matching open gaps (with or without GGUF).
2. After each tick, `drain_jtc_gaps` runs the **finite** closed-set path (`cnet_jtc_ensure_sealed`) and marks those gaps `CLOSED`.

Disable: `CNET_JTC_TEACHER=0`.

## Seal into personal / live CNB

```bash
# Stops learner briefly, mines+seals, verifies SoulHost, restarts learner
scripts/json_toolcall_seal.sh
# or
scripts/personal_ai_auto.sh jtc-seal

# CLI only (operator ensures no concurrent writer):
make json_toolcall_seal_cli
bin/json_toolcall_seal /path/to/soul.cnb
```

Idempotent: if `json_toolcall_v0` already present with identical bytes, reuses.

## v1 lookup tools

Unit `json_toolcall_v1` adds:

| Tool | Host execute | Native |
|---|---|---|
| `web_search` | `McpTools.WebSearch` / MCP `cnet_web_search` | DuckDuckGo Instant Answer; **Wikipedia fallback** when DDG is empty |
| `wiki_lookup` | `McpTools.WikiLookup` / MCP `cnet_wiki_lookup` | Wikipedia REST summary + fact memory cache |

Agent system prompt tells the LLM to call lookup when it lacks facts. Certified classifier confirms closed-set tool id before execute.

Legacy `json_toolcall_v0` may remain in an older CNB; seal script adds v1 without removing v0.

## Not in spine

- Arbitrary schema validation as certified law
- Nested/open tool catalogs (extend closed set + re-mine)
- Residual “JSON-looking” text without host validate
- Full browser / multi-page scrape (use host residual / Hermes if needed)
