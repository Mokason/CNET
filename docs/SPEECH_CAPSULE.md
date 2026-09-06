# Speech output

[pack_speech_io](../packs/pack_speech_io/) contains speech-related guidance.
The executable TTS path is [cnet_speech_say.py](../tools/cnet_speech_say.py),
which uses `edge_tts` to synthesize audio and optional local players.
This path can send text to an external service; it is not offline speech
synthesis or a new certified capability.

The CLI accepts explicit text or `--q` to query the selected daemon, and
`--play` requests playback. Query mode checks the returned utterance and
voice policy. Explicit user-supplied text is different from a verified answer.

The web `POST /api/speak` route requires the same authentication boundary as
the [web client](CNET_WEB.md). Do not use old unauthenticated network examples
or machine-specific Python paths. Keep credentials out of URLs and logs.

Audio files go to the configured speech directory. Select an isolated output
for tests and obtain authority before transmitting private text.
Speaking a draft does not certify it. This TTS adapter is speak-out;
other voice-input experiments have separate protocols and gates.
