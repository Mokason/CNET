# Narrative Coherence Contract

## Purpose

`NarrativeCoherenceContract` is a lightweight rubric contract for creative/story outputs. It is designed to catch narrative flattening symptoms during compression and routing, especially loss of voice, moral tension, delayed consequences, and concrete folklore texture.

## Native Surface

Header: `include/contract/narrative_coherence.h`
Implementation: `src/contract/narrative_coherence.c`

Primary API:

```c
void narrative_coherence_contract_default(NarrativeCoherenceContract *contract);
int narrative_coherence_score_text(const NarrativeCoherenceContract *contract,
                                   const char *prompt,
                                   const char *text,
                                   NarrativeCoherenceScore *score);
int narrative_coherence_passes(const NarrativeCoherenceContract *contract,
                               const NarrativeCoherenceScore *score);
```

## Dimensions

- `voice_consistency`: prompt-grounded terms plus stable narrative voice markers.
- `moral_ambiguity`: moral language and contrast markers that avoid single-step resolution.
- `delayed_consequence`: temporal/consequential markers that preserve cause over distance.
- `folklore_texture`: concrete place, ritual, object, and communal texture markers.

The default weighted score is:

```text
0.25 * voice + 0.30 * moral + 0.25 * delayed + 0.20 * folklore
```

`flatness_risk` is `1.0 - overall`. A continuation passes the default contract when `overall >= 0.60` and `flatness_risk <= 0.40`.

## Routing Contract

Branches can now be tagged with `CCE_SPECIALIST_NARRATIVE`. Existing routing remains unchanged through `cce_router_route()`. Creative/task-aware routing uses `cce_router_route_for_task()`, which applies a bounded preference to narrative specialists when the task text contains story, testimony, folklore, moral, consequence, or related creative cues.

## MCP Testimony Contract

`cnet_generate_testimony` writes narrative coherence metadata into testimony YAML:

```yaml
specialist_type: NARRATIVE_SPECIALIST
narrative_coherence_overall: ...
narrative_voice_consistency: ...
narrative_moral_ambiguity: ...
narrative_delayed_consequence: ...
narrative_folklore_texture: ...
narrative_flatness_risk: ...
narrative_missing_dimensions: ...
```

## Limits

This first slice is deterministic and heuristic. It is suitable as a cheap compression/routing guard and regression signal, not as a substitute for a human or LLM-as-judge narrative quality evaluation.
