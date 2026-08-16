# Speech capsule (`pack_speech_io`)

Speak-out modality for CNET. **Delivery only — never CERT.**

## Pieces

| Piece | Role |
|--------|------|
| `pack_speech_io` | CERT skills explaining speech law / how-to |
| `tools/cnet_speech_say.py` | edge-tts → mp3 |
| `POST /api/speak` | web TTS |
| Cockpit **Speak** | plays last reply |

## CLI

```bash
# prefer venv with edge-tts
CNET_PYTHON=$HOME/.hermes/hermes-agent/venv/bin/python3 \
  python3 tools/cnet_speech_say.py "Hello from Marble"

# ask cnetd then speak
$CNET_PYTHON tools/cnet_speech_say.py --q "who are you" --play
```

## Web

```bash
curl -X POST http://100.x.x.x:8642/api/speak \
  -H 'Content-Type: application/json' \
  -d '{"text":"Hello from the speech capsule"}'
# → {ok, path, url: /api/speech/...mp3}
```

## Law

- Speaking does **not** seal knowledge.
- Teacher drafts may be voiced; still `auto_cert=false`.
- No STT/listen capsule in v1 (speak-out only).
