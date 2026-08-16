# CORE → Brain admit mirror

Status: **LIVE OUTBOX — PENDING BRAIN VERIFY — NEVER SELF-CERT**

## Flow

```
cnet_hemi_ask_core / classify CORE cert
        │
        ▼
cnet_brain_mirror_core()
        │
        ├─ $CNET_BRAIN_MIRROR_DIR/core_admit.jsonl
        └─ $CNET_BRAIN_MIRROR_DIR/inbox/cand_*.json
                │
                ▼
cb_import_cnet_mirror <mirror> <brain_inbox>
        │
        ▼
Brain inbox seeds (auto_cert=0)
```

## Law

- Only **CORE + bound + claimed_cert** mirrors.
- **RESIDUAL / held / MAX** never writes the outbox.
- Mirror is **admit_candidate**, not CNET self-CERT.
- Brain still verify-admits (or human reviews seeds).

## Gates

```text
make cnet_brain_mirror   # CNET_BRAIN_MIRROR_PASS residual_never_mirrors=1
make cnet_hemi           # CORE path triggers mirror
```

## Env

| Var | Default |
|-----|---------|
| `CNET_BRAIN_MIRROR_DIR` | `$HERMES_HOME/brain_mirror` or `~/.hermes/brain_mirror` |
