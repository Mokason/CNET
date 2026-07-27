#!/usr/bin/env bash
# External natural-image OOD probe: Kodak True Color Image Suite (24 photos),
# a long-standing public research image set. Public, resumable, checksummed,
# never committed.
set -uo pipefail
D=data/ood/kodak
mkdir -p "$D" logs/vision
for i in $(seq -w 1 24); do
  f="$D/kodim$i.png"
  [ -s "$f" ] && continue
  curl -sSL --retry 3 --retry-delay 2 -m 120 -o "$f" \
    "https://r0k.us/graphics/kodak/kodak/kodim$i.png" || rm -f "$f"
done
n=$(ls "$D"/*.png 2>/dev/null | wc -l)
sha256sum "$D"/*.png > logs/vision/ood_kodak_sha256.txt 2>/dev/null
echo "VISION_OOD_FETCH_OK kodak_images=$n checksums=logs/vision/ood_kodak_sha256.txt"
