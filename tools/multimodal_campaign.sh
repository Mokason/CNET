#!/bin/bash
# Multimodal domain campaign helper (voice / vision spines).
#
# Hermetic mine is `make multimodal_v0`. This script prepares env + tags for
# binding a real external teacher later and for gap-lane modality contexts.
#
# Usage:
#   tools/multimodal_campaign.sh prepare-voice
#   tools/multimodal_campaign.sh prepare-vision
#   tools/multimodal_campaign.sh env-voice
#   tools/multimodal_campaign.sh env-vision
set -euo pipefail
CNET_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CMD="${1:-}"

prepare_voice() {
    mkdir -p "$CNET_DIR/config/modality_contexts" "$CNET_DIR/artifacts"
    # Named context tag prefix for gap-lane: voice_*.ids optional later
    cat > "$CNET_DIR/config/modality_contexts/README.md" <<'EOF'
# Modality context tags for gap-lane

Goal tags may use prefixes that select pinned contexts:

- `voice_<cmd>_…`  — speech-command family (closed set spine)
- `vision_<cls>_…` — visual class family

Place optional `<name>.ids` token-id files here when a language teacher is
also bound; pure voice/vision units use feature ports, not token windows.
EOF
    cat > "$CNET_DIR/artifacts/voice_v0_commands.txt" <<'EOF'
# Closed-set speech commands (CNET voice v0 / speech_commands subset)
yes
no
up
down
left
right
on
off
stop
go
EOF
    echo "MULTIMODAL_PREPARE_VOICE_OK artifacts/voice_v0_commands.txt"
}

prepare_vision() {
    mkdir -p "$CNET_DIR/artifacts"
    cat > "$CNET_DIR/artifacts/vision_v0_classes.txt" <<'EOF'
# Fixed visual classes (CNET vision v0)
circle
square
triangle
line
EOF
    echo "MULTIMODAL_PREPARE_VISION_OK artifacts/vision_v0_classes.txt"
}

env_voice() {
    local vt="$CNET_DIR/bin/voice_teacher"
    cat <<EOF
# External voice teacher binding (subprocess ABI → bin/voice_teacher)
export CNET_MODALITY=voice
export CNET_EXT_TEACHER_KIND=subprocess
export CNET_EXT_TEACHER_NAME=voice_external
# Hermetic (gate / no Whisper weights). Whisper mode is WITHHELD (no Python).
export CNET_VOICE_TEACHER_CMD="$vt --mode hermetic"
# Check readiness:
#   $vt --check
export CNET_VOICE_N_CMD=10
export CNET_VOICE_FEAT_DIM=16
EOF
}

env_vision() {
    cat <<EOF
export CNET_MODALITY=vision
export CNET_EXT_TEACHER_KIND=callback
export CNET_EXT_TEACHER_NAME=vision_external
# export CNET_VISION_TEACHER_CMD="/path/to/clip_teacher --labels circle,square,triangle,line"
export CNET_VISION_N_CLASS=4
export CNET_VISION_PIXELS=64
EOF
}

case "$CMD" in
    prepare-voice) prepare_voice ;;
    prepare-vision) prepare_vision ;;
    prepare) prepare_voice; prepare_vision; echo "MULTIMODAL_PREPARE_PASS" ;;
    env-voice) env_voice ;;
    env-vision) env_vision ;;
    *)
        echo "usage: $0 prepare|prepare-voice|prepare-vision|env-voice|env-vision" >&2
        exit 2
        ;;
esac
