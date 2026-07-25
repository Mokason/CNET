# CNET autonomous governor v4 (A-path)

## Single production engine
`scripts/governor_autonomous.py` (engine `governor_autonomous_v4`) via systemd.

## Caps closed toward A/A+
| Cap | Fix |
|---|---|
| Noisy Hermes logs | `governor_hermes_structured.py` → `state.db` messages |
| Flat projects | `governor_goal_graph.json` multi-node + transfer edges |
| Flashy meta | `stable_evolve` min cycles, clamp, min_dt |
| No sandbox | `scripts/dev-sandbox.sh` Hermes-style isolated venv/state |
| No novel path | `novel_curriculum.jsonl` bounded proposals |

## Sandbox (like Hermes dev-sandbox)
```bash
scripts/dev-sandbox.sh --persistent --from-prod
scripts/dev-sandbox.sh --persistent python3 scripts/governor_autonomous.py --test
scripts/dev-sandbox.sh --delete
```
State under `.cnet-sandbox/` (gitignored). Code from live tree.

## Gates
```bash
make governor_v4
make governor_a_gate    # GOVERNOR_A_GATE_PASS
make governor_sandbox
```
