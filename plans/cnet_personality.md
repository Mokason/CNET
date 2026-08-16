# CNET personality organ (homeostatic)

## Role
Artificial affect + durable persona on the scoreboard/governor spine.
**Biases will, never truth.** Below charter/pins/eval veto.

## Files
- `config/personality.yaml` — profiles + baselines + clamps
- `tools/governor_personality.c` — affect ← outcomes, traits, homeostasis
- `logs/governor/personality_state.json` — live state
- `logs/governor/personality_history.jsonl` — audit

## Affect (dopamine-like meters)
reward, frustration, calm, vigilance, integrity, correction  
driven by backlog/eval/Hermes/plateau/busy/veto

## Neuromod (DA / 5HT / ADO) — explicit biology metaphor
See `docs/NEUROMOD_PERSONALITY.md` and `scripts/cnet_neuromod.py`.

| Channel | When |
|---------|------|
| dopamine | task done right (LOCAL / promote) |
| serotonin | remember/pin vs sort queue |
| adenosine | consolidate WM / reorganize context |

```bash
make cnet_neuromod
python3 scripts/cnet_neuromod.py --tick
```

## Safety
- clamp `[0.15, 0.85]`
- max trait Δ / cycle
- homeostasis pull to baseline each cycle
- high vigilance suppresses boldness/curiosity
- no seal-path coupling

## Profiles
`memory_witness` (default), `builder`, `careful_janitor`

## Gate
```bash
./bin/governor_personality --test
./bin/governor_autonomous --test
```

## Marble imprint + Zen commit gate
- Active profile: `marble` (zen spine + light anime/VTuber delivery)
- Voice: `config/voice_marble.md` (+ `~/.hermes/identity/voice_marble.md`, SOUL pointer)
- Zen gate: `tools/governor_zen_reflect.c` — sit/see before heavy commit; never delay infra fires
- Decision log includes `persona` + `zen.principles`
