# Multimodal External Teachers (Vision / Voice)

**Place:** Boundary between foreign frameworks (Whisper, CLIP, HF, ONNX, …)
and CNET's certified Specialist / gap-lane loop.

**Dilemma:** Reimplementing foundation models in pure C would delay multimodal
competence for years; pulling foreign stacks into the planner would break
certification and freeze/compound discipline.

**Consequence:** Foreign models are **teachers (oracles)** only. CNET mines
finite typed contracts, admits students through `specialist_admit`, and serves
from CNB. Self-improve stays short cycles after a domain spine exists.

## Phases

| Phase | Deliverable | Gate |
|---|---|---|
| Bridge | `external_teacher` bind + identity + table/callback/**subprocess** modes | `make multimodal_v0` |
| Voice v0 | Closed-set speech commands from hermetic teacher → certified unit | same |
| Voice real teacher | Subprocess ABI + `tools/voice_teacher.py` (hermetic gate; Whisper optional) | `make voice_real_teacher` |
| Vision v0 | Fixed-label image classifier + optional EVIDENCE | same |
| Campaign helper | `tools/multimodal_campaign.sh` prepare/env | same |
| Self-improve | modality tags `voice_*` / `vision_*` for gap-lane contexts | config |

## Quality floor

- No lowering of certify / Wilson bars.
- Teacher artifact_digest must be nonzero (identity never guessed).
- Hermetic gates use fake teachers; real Whisper/CLIP bind the same ABI later.

## Result

**H1 (hermetic, 2026-07-17):**

```
make multimodal_v0  →  MULTIMODAL_V0_PASS (39 checks)
  external_teacher bind/table/register
  voice_cmd_v0 mine+admit (10 classes)
  vision_class_v0 mine+admit (4 classes)
  prepare artifacts via tools/multimodal_campaign.sh
```

### Bind a real framework (voice)

**H2 (subprocess + Whisper path, 2026-07-17):**

```
make voice_real_teacher → VOICE_REAL_TEACHER_PASS
  external_teacher_bind_subprocess (IN/OUT line protocol)
  tools/voice_teacher.py --mode hermetic | whisper
  cnet_voice_bind_subprocess / bind_from_env / mine_admit_teacher
```

1. **Subprocess (preferred for foreign stacks):**
   ```bash
   # hermetic ABI proof
   export CNET_VOICE_TEACHER_CMD="python3 tools/voice_teacher.py --mode hermetic"
   # real ASR (faster-whisper when installed)
   export CNET_VOICE_TEACHER_CMD="python3 tools/voice_teacher.py --mode whisper --model tiny"
   # or: tools/multimodal_campaign.sh env-voice
   ```
   Then `cnet_voice_bind_from_env` / `cnet_voice_mine_admit_teacher`.
2. **Callback:** implement `CnetOracleFn` that maps `speech_feat` → `speech_cmd` one-hot; bind with nonzero `artifact_digest`.
3. Offline wav labeling: `voice_teacher.py --mode whisper --label-wav clip.wav`.
4. Serve the sealed student without the heavy teacher.

Self-improve: after the spine exists, only misses re-teach (short cycles).
