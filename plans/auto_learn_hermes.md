# Automatic learn-from-Hermes (2026-07-21)

## Loop
1. Hermes/MCP miss or freeform `request_capability` → `gap_inbox_note_no_plan`
2. **CNET_AUTO_LEARN=1** rewrites freeform ports → `w_cur` → `tk{id}q{id}` (window teachable)
3. Personal-AI gap lane binds LM teachers (priority = times_hit), drains, certifies, seals
4. CNB generation advances; MCP reloads; next similar request can hit certified unit

## Controls
- `CNET_AUTO_LEARN=0` disables rewrite
- `CNET_AUTO_LEARN_W` / `CNET_AUTO_LEARN_K` window and top-k
- `ACQUIRE_MAX_ORACLES` raised to 32; teacher bind sorted by hit count

## Gate
`make auto_learn` → `AUTO_LEARN_PASS`
