# Scenario-layer KPI floors — what is gated, what is WITHHELD

Date: 2026-08-17. Applies to `cnet_agi_scenario`, `cnet_agi_scenario2`,
`cnet_agi_scenario3` (target names are pre-existing identifiers, not claims).

## The problem this closes

The three scenario layers printed a KPI line each and the gates grepped only
the binary markers (`..._PASS`, `layerN=1`, `parrot_mouth=0`). Every rate —
`cert_rate`, `honesty`, `goal_rate`, `gather_rate`, `chain_rate`, `synth_prec`,
`committee_rate` — was reported to a human and checked by nothing. `synth_prec`
could have fallen 0.600 → 0.100 with the gate still green. That is a vanity
metric in the exact sense **AGENTS.md #3** forbids.

More seriously, **`residual_auto_cert=0` was printed and unenforced in all
three layers.** That is not a quality score; it is the CORE law that residual
output never auto-certifies. It is now an exact-match check, not a floor.

## Enforcement

`tests/scenario_kpi_floor_check.sh <log> <name> key>=floor | key==exact`

- `key==exact` — law flags. Any deviation fails.
- `key>=floor` — quality floors. Regression below the measured value fails.
- **A missing key fails.** A renamed or deleted metric breaks the gate rather
  than silently dropping out of it.

Floors are set at **measured** values. The layers are deterministic: three
consecutive runs produce byte-identical KPI lines (only `ms` varies), so exact
floors carry no flake risk. Establishing a floor where none existed is not
lowering one; **AGENTS.md #1 still applies** — none of these may be relaxed to
make a gate pass. If a floor legitimately moves, move it in the commit that
earns it and say why.

## What each layer now gates

| Layer | Law (exact) | Floors (measured) |
|---|---|---|
| 1 | `residual_auto_cert=0` `parrot_mouth=0` `given_info=1` `value_miss=1` `prove_or_abstain=1` `evolve=1` `compose=1` | `cert_rate≥0.800` `honesty≥0.867` |
| 2 | `residual_auto_cert=0` `parrot_mouth=0` `goals=1` `active_gather=1` `chains=1` `persist=1` `transfer=1` | `cert_rate≥0.875` `honesty≥0.800` `chain_rate≥1.000` `goal_rate≥0.500` `gather_rate≥0.500` |
| 3 | `residual_auto_cert=0` `parrot_mouth=0` `plan_synth=1` `cert_only_plans=1` `specialists=1` `committee=1` | `cert_rate≥0.889` `honesty≥0.909` `synth_prec≥0.600` `committee_rate≥1.000` |

31 KPIs enforced in total (9 / 12 / 10), against 0 before.

## What these numbers are

Each layer is a **fixed, hand-authored scenario script** run against the CORE
bus in-process. The floors certify that *this scripted scenario* keeps
behaving as measured. They are regression detectors.

## WITHHELD — not claimed by these gates

- **No general-capability claim of any kind.** These are scripted scenarios
  with fixed turn lists, not held-out evaluation. A passing board says the
  script did not regress; it says nothing about unseen tasks.
- `goal_rate=0.500` and `gather_rate=0.500` are **half**. They are floored at
  what they measure so they cannot silently rot — flooring a weak number is not
  endorsing it.
- `synth_prec=0.600` likewise: 2 of 5 candidate plans are rejected by design in
  the script, so this is a property of the fixture, not a measured precision on
  any real distribution.
- `chain_rate=1.000` and `committee_rate=1.000` are **1.000 over 1 and 2 trials
  respectively** — sample sizes far too small to support a reliability claim.
  Per AGENTS.md #3, a `1.000` must name what was tested: it is tested here on
  the scripted chain/committee turns only.
- No statement about the live 24/7 stack. These binaries do not touch cnetd,
  the gap lane, or any residual endpoint.

## Re-run

```bash
make cnet_agi_scenario cnet_agi_scenario2 cnet_agi_scenario3
```
