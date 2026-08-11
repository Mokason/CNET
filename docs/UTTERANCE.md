# CNET utterance — C-native sentence composition

CNET forms speakable lines **without Teacher**. TTS only voices those lines by default.

## Flow

```text
ask → CERT/Teacher answer
    → cnet_utterance compose (templates + neuromod/hit/skill state)
    → utterance + may_voice
    → TTS only if may_voice (LOCAL) unless CNET_NEVER_VOICE_LLM=0
```

## CLI

```bash
make cnet_utterance
./bin/cnet_utterance --test
./bin/cnet_utterance --when status --hit 0.86 --da 0.5 --ht 0.5 --ado 0.4 --miss 2

cnet-speech-say --q "who are you"          # speaks C utterance
cnet-speech-say --q "What is Tailscale?"   # refused: never_voice_llm
```

## Bank

- Built-in phrases in `src/cnet_utterance.c`
- Overlay: `config/utterance_phrases.tsv` (`id`, `when`, `template` with `{slots}`)

Slots: `name`, `answer`, `skill`, `domain`, `local_hit`, `da`, `ht`, `ado`, `miss_n`, `chain`, `law`, …

## Policy

| Env | Default | Effect |
|-----|---------|--------|
| `CNET_NEVER_VOICE_LLM` | `1` | Teacher drafts not spoken |
| override | `--allow-voice-llm` / API flag | allow TTS of LLM text |

Law: compose ≠ CERT. No pack_personal mint from utterance.
