# Modality context inputs

Gap-lane goal tags can select pinned modality context: `voice_<cmd>_...`
for a command family and `vision_<class>_...` for a visual class.

Optional `<name>.ids` files contain teacher token IDs. They are data and
must match the intended teacher vocabulary; do not rewrite them as prose.
Pure voice/vision feature-port units do not require token windows.

The [external-teacher plan](../../plans/multimodal_external_teachers.md)
retains the Voice v0 / Vision v0 contracts. Each modality needs its own
coverage and held-out evidence; a tag or pinned context does not supply it.
