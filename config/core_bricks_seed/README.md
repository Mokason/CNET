# CORE brick seed (`.lut` only)

Live bank path: `$HOME/.local/share/cnet-bricks` (`CNET_CORE_BUS_BRICKS_DIR`).

Install/sync:
```bash
scripts/cnet_core_bricks_wire.sh
systemctl --user restart cnetd
cnet-sock-ask "q1_add16 3"   # expect LOCAL core brick
```

These LUTs are **CERT tables** from overnight AGI scenario factory (2026-08-17).
Serve path is light RESULT (no residual auto-CERT). GGUF teacher weights stay
out of this seed — factory rebuilds them under the live bricks dir when needed.
