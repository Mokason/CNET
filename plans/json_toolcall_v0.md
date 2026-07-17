# Closed-set JSON tool-call spine (v0)

**Place:** Boundary between host JSON (System.Text.Json / MCP) and certified CNET skills.

**Dilemma:** Free-form JSON generation is open-ended; treating residual text as certified JSON is dishonest. Re-implementing a full JSON engine in pure C delays useful tool routing.

**Consequence:** Host owns parse/emit. CNET owns a **closed-set** classifier:

```
jtc_feat  RAW[16]  (keyword bag from JSON text)
    →
json_tool ONEHOT[6]  (calculator | memory_store | memory_recall | file_read | cnet_recall | final)
```

## Gate

```
make json_toolcall → JSON_TOOLCALL_PASS
  encode + hermetic teacher
  mine+admit json_toolcall_v0
  seal CNB → soul_request source=CERTIFIED for all 6 tools
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

## Not in v0

- Arbitrary schema validation as certified law
- Nested/open tool catalogs (extend closed set + re-mine)
- Residual “JSON-looking” text without host validate
