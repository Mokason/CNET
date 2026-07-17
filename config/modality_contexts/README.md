# Modality context tags for gap-lane

Goal tags may use prefixes that select pinned contexts:

- `voice_<cmd>_…`  — speech-command family (closed set spine)
- `vision_<cls>_…` — visual class family

Place optional `<name>.ids` token-id files here when a language teacher is
also bound; pure voice/vision units use feature ports, not token windows.
