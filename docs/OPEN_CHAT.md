# Open chat (Teacher residual)

## Behavior

```text
CERT hit     → LOCAL (0 tokens)
Probe tags   → ASK_USER short-circuit (no Teacher)
Organic miss → Teacher (Ollama) draft → miss_log learnable=true
             → NEVER pack_personal / never auto_cert
```

## Enable

Set on `cnetd.service` / `cnet-minimal.env`:

```bash
CNET_OPEN_CHAT=1
ROE_LLM=1
ROE_LLM_MODEL=minimax-m3:cloud   # or local model
ROE_LLM_THINK=0
```

```bash
systemctl --user restart cnetd.service
```

## Learn later

Miss rows with `learnable:true` + `open_chat:true` feed weekly
`gold_curriculum_harvest` / reviewer — not automatic CERT.

## Disable

`CNET_OPEN_CHAT=0` and `ROE_LLM=0`, restart cnetd.
