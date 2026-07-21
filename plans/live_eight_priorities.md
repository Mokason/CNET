# Live Eight Priorities Campaign (2026-07-21)

## Gate
`make live_eight_campaign` → `LIVE_EIGHT_CAMPAIGN_PASS`

Works on a **copy** of `soul_gemma4v2_final.cnb` under `artifacts/` unless
`CNET_LIVE_MUTATE_PRODUCTION=1`.

## What it proves
1. Serve path via `soul_request` increments `certified_serves` and persists reopen
2. Multi-step non-token distill (`le8_chunk`) into working base
3. Oracle deploy policy smoke (v2_only, attestation detection)
4. Gap park: empty-oracle drain → `waiting_oracle`
5. `json_toolcall_v2` sealed
6. Hermetic residual / gap note on novel goal
7. Local measured proxy + TruthfulQA present but full suite withheld
8. Hygiene report

## Production actions (this session)
- `scripts/json_toolcall_seal.sh soul_gemma4v2_final.cnb --force` → `json_toolcall_v2` on live CNB (units 393→394)
- Config: `config/personal-ai.env` + `cnet-deploy.env` oracle/curiosity knobs
- Scripts: JTC detection prefers v2
- **MCP recycle required:** run `hermes gateway restart` from an external shell (cannot restart from inside gateway)

## Safety
No reserved-GPU service start. Residual default hermetic in campaign.
