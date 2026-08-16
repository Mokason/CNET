# CNET + Marble 24/7 (outside Hermes)

Hermes is an **optional client**. CNET, ROE packs (including Marble SOUL),
autoteach, governor, and evolve tick keep running when Hermes updates,
restarts, or is stopped.

## One command

```bash
cd ~/AI/CNET
scripts/cnet_marble_24_7.sh install
scripts/cnet_marble_24_7.sh start
scripts/cnet_marble_24_7.sh status
scripts/cnet_marble_24_7.sh doctor
```

## What stays up (no Hermes)

| Unit | Role |
|------|------|
| `cnet-marble.target` | Umbrella (Hermes-free) |
| `cnet-personal-ai-lane.service` | Learn/seal from gap inbox |
| `bonsai-server.service` | Local residual/teacher HTTP |
| `cnet-autoteach.timer` | Curriculum + PEFT tick |
| `cnet-governor.timer` | Autonomous governor |
| `cnet-janitor.timer` | Library hygiene |
| `cnet-personal-ai-ops.timer` | Ops heal tick |
| `roe-evolve-tick.timer` | Miss→gold/reviewer→`pack_personal` |
| `cnet-autonomous-cycle.timer` | Probe curriculum → teacher → evolve KPI |
| `cnet-marble-health.timer` | Snapshot `logs/marble_24_7/status.json` |
| `marble-heartbeat` / `marble-embeddings` | Optional Marble sidecars |

**Not required:** `hermes-gateway`, `hermes-dashboard`, Hermes MCP.

## Autonomy (no go / accept spam)

```bash
make cnet_autonomous
# or timer (every 20m, part of cnet-marble.target):
systemctl --user status cnet-autonomous-cycle.timer
cat logs/marble_24_7/AUTONOMOUS_CYCLE.json
```

Cycle: curriculum probes → front_door → miss_log → teacher (optional) →
evolve (gold / multi_stable + **reviewer**) → `pack_personal`.

Edit probes/gold: `config/autonomous_curriculum.jsonl`

Not AGI: never self-CERT; floors unchanged; Hermes optional.

## Isolation rules

1. **WorkingDirectory** = `~/AI/CNET` (repo), not Hermes home  
2. **Env** from `config/personal-ai.env`, `roe-teacher-*.env`, `roe-reviewer-*.env`  
3. **Linger=yes** so user systemd survives logout  
4. Hermes upgrade must not `systemctl stop cnet-marble.target`  
5. Teacher/reviewer via **Ollama** (`:11434`) or **Bonsai** (`:8080`) — not Hermes model router  

## Health

```bash
scripts/cnet_marble_24_7.sh health
cat logs/marble_24_7/status.json
```

Fields: `hermes_required: false`, core unit actives, ports, pack paths.

## After reboot

With linger enabled:

```bash
loginctl show-user $USER -p Linger   # Linger=yes
systemctl --user status cnet-marble.target
```

## Hermes still useful as

- Chat UI / tools front door  
- Optional MCP that writes the same `*.inbox`  

If Hermes is down, ROE evolve + autoteach + personal-ai lane **continue**.

## Stop only CNET stack (leave Hermes)

```bash
scripts/cnet_marble_24_7.sh stop
```
