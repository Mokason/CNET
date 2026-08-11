# Open chat (Teacher residual) — optional

Default product path is **CNET self-answer** (`CNET_SELF_ANSWER=1`):
misses get C utterance templates, not Ollama.

Teacher is residual only when:

```bash
CNET_TEACHER_ON_MISS=1
# or CNET_SELF_ANSWER=0 with ROE_LLM=1
systemctl --user restart cnetd.service
```

## Self-answer (default)

```text
CERT hit  → source=LOCAL (sealed skill)
Miss      → source=CNET  (utter_self template, may_voice=1)
Probe     → source=CNET  (utter_probe, short-circuit)
Teacher   → off unless CNET_TEACHER_ON_MISS=1
```

See also `docs/UTTERANCE.md`.

