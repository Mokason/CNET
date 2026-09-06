# Utterance composition

[cnet_utterance.h](../include/cnet_utterance.h) and
[cnet_utterance.c](../src/serve/cnet_utterance.c) compose speakable template
text from answer, skill, query and control-state fields.
[utterance_phrases.tsv](../config/utterance_phrases.tsv) is an optional overlay.

```sh
make cnet_utterance
bin/cnet_utterance --test
```

Composition does not call a teacher and does not certify new knowledge.
The daemon's default self-answer path may produce `source=CNET` /
`utter_self` presentation text on a miss. That is distinct from a covered,
verified capsule result.

The current daemon hard-disables its legacy teacher-on-miss branch.
`CNET_TEACHER_ON_MISS=1` does not restore it; the old restart recipe was
incorrect. Other teacher integrations are separate.

Voice policy and `may_voice` decide what the speech client may speak;
voiceability is not certification. See [speech output](SPEECH_CAPSULE.md),
[daemon](CNETD.md) and [teacher paths](TEACH_PATH.md).
