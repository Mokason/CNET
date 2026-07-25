#!/usr/bin/env bash
# Safe web explore: ONLY URLs in config/governor_verified_urls.txt
# Fetches one URL per call (round-robin), stores text note under logs/governor/web_notes/
set -euo pipefail
ROOT="${CNET_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
ALLOW="${CNET_GOVERNOR_URLS:-$ROOT/config/governor_verified_urls.txt}"
DIR="${CNET_GOVERNOR_DIR:-$ROOT/logs/governor}"
NOTES="$DIR/web_notes"
STATE="$DIR/web_rr_state"
mkdir -p "$NOTES"

if [[ ! -f "$ALLOW" ]]; then
  echo "SAFE_WEB_FAIL no allowlist" >&2
  exit 2
fi

mapfile -t URLS < <(grep -E '^https://' "$ALLOW" | sed 's/[[:space:]]*$//')
n=${#URLS[@]}
if [[ "$n" -eq 0 ]]; then
  echo "SAFE_WEB_FAIL empty allowlist" >&2
  exit 2
fi

idx=0
if [[ -f "$STATE" ]]; then
  idx=$(cat "$STATE" 2>/dev/null || echo 0)
fi
idx=$((idx % n))
URL="${URLS[$idx]}"
echo $(( (idx+1) % n )) > "$STATE"

# SSRF / safety: must be exact allowlist match
ok=0
for u in "${URLS[@]}"; do
  if [[ "$u" == "$URL" ]]; then ok=1; break; fi
done
if [[ "$ok" -ne 1 ]]; then
  echo "SAFE_WEB_FAIL not allowlisted" >&2
  exit 3
fi

# block non-https and credentials
if [[ "$URL" != https://* ]] || [[ "$URL" == *"@"* ]]; then
  echo "SAFE_WEB_FAIL scheme" >&2
  exit 3
fi

host=$(python3 - <<PY
from urllib.parse import urlparse
print(urlparse("$URL").hostname or "")
PY
)
# basic host check
if [[ -z "$host" ]]; then
  echo "SAFE_WEB_FAIL host" >&2
  exit 3
fi

ts=$(date +%Y%m%d_%H%M%S)
slug=$(echo "$host" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-40)
out="$NOTES/${ts}_${slug}.txt"

# fetch: curl only, follow max 3 redirects, size cap, timeout
# --proto-redir =https keeps redirects on https
code=$(curl -sS -L --max-redirs 3 --proto-redir =https \
  --max-time 25 --max-filesize 1500000 \
  -A "CNET-GovernorSafeWeb/1.0" \
  -w "%{http_code}" -o "$out.raw" "$URL" || echo "000")

if [[ "$code" != 200 && "$code" != 301 && "$code" != 302 ]]; then
  rm -f "$out.raw"
  echo "SAFE_WEB_FAIL http=$code url=$URL" >&2
  exit 4
fi

# strip tags lightly to text
python3 - <<PY
from pathlib import Path
import re, html
raw=Path("$out.raw").read_bytes()
# refuse if looks like binary
if b"\x00" in raw[:2000]:
    raise SystemExit("binary")
text=raw.decode("utf-8", errors="replace")
text=re.sub(r"(?is)<script[^>]*>.*?</script>", " ", text)
text=re.sub(r"(?is)<style[^>]*>.*?</style>", " ", text)
text=re.sub(r"(?s)<[^>]+>", " ", text)
text=html.unescape(text)
text=re.sub(r"\s+", " ", text).strip()
text=text[:12000]
Path("$out").write_text(f"URL: $URL\nHTTP: $code\n\n{text}\n")
Path("$out.raw").unlink(missing_ok=True)
print("SAFE_WEB_OK", "$URL", "chars", len(text), "note", "$out")
PY

# index count
nnotes=$(ls -1 "$NOTES"/*.txt 2>/dev/null | wc -l | tr -d ' ')
echo "$nnotes" > "$DIR/web_notes_count"
python3 - <<PY
import json, time
from pathlib import Path
p=Path("$DIR/web_last.json")
p.write_text(json.dumps({
  "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
  "url": "$URL",
  "note": "$out",
  "notes_count": int("$nnotes"),
}, indent=2)+"\n")
PY
