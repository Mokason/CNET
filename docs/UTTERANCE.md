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

## Self-answer (default)

`CNET_SELF_ANSWER=1` (default): on CERT miss, **answer text is the C utterance**
(`source=CNET`, skill `utter_self`). Teacher is not called.

```bash
# restore Teacher residual on misses:
CNET_TEACHER_ON_MISS=1   # in cnet-minimal.env
systemctl --user restart cnetd
```

CNET self-answers are voiceable (`may_voice=1`). They still do **not** CERT.
