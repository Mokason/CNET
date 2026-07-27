#!/usr/bin/env bash
# PASCAL VOC 2007 fetch: public, resumable, checksummed. Never committed.
set -euo pipefail
D=data/voc2007
mkdir -p "$D" logs/vision
declare -A MD5=( [VOCtrainval_06-Nov-2007.tar]=c52e279531787c972589f7e41ab4ae64
                 [VOCtest_06-Nov-2007.tar]=b6e924de25625d8de591ea690078ad9f )
for f in "${!MD5[@]}"; do
  if [ ! -f "$D/$f" ]; then
    aria2c -c -x4 -s4 --retry-wait=5 -m8 -d "$D" -o "$f" \
      "http://host.robots.ox.ac.uk/pascal/VOC/voc2007/$f" \
      "https://data.brainchip.com/dataset-mirror/voc/$f"
  fi
  got=$(md5sum "$D/$f" | cut -d' ' -f1)
  [ "$got" = "${MD5[$f]}" ] || { echo "VISION_FETCH_FAIL checksum $f got=$got"; exit 1; }
  echo "ok $f md5=$got"
done
[ -d "$D/VOCdevkit" ] || for f in "${!MD5[@]}"; do tar xf "$D/$f" -C "$D"; done
echo "VISION_FETCH_OK devkit=$D/VOCdevkit/VOC2007"
