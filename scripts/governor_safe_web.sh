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

# hostname between :// and next / or : or end
host="${URL#https://}"
host="${host%%/*}"
host="${host%%:*}"
if [[ -z "$host" ]]; then
  echo "SAFE_WEB_FAIL host" >&2
  exit 3
fi

ts=$(date +%Y%m%d_%H%M%S)
slug=$(echo "$host" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-40)
out="$NOTES/${ts}_${slug}.txt"

code=$(curl -sS -L --max-redirs 3 --proto-redir =https \
  --max-time 25 --max-filesize 1500000 \
  -A "CNET-GovernorSafeWeb/1.0" \
  -w "%{http_code}" -o "$out.raw" "$URL" || echo "000")

if [[ "$code" != 200 && "$code" != 301 && "$code" != 302 ]]; then
  rm -f "$out.raw"
  echo "SAFE_WEB_FAIL http=$code url=$URL" >&2
  exit 4
fi

# refuse binary (NUL in first 2k); strip tags lightly to text
if head -c 2000 "$out.raw" 2>/dev/null | grep -q $'\0'; then
  rm -f "$out.raw"
  echo "SAFE_WEB_FAIL binary" >&2
  exit 4
fi

text=$(
  sed -E \
    -e 's/<script[^>]*>.*<\/script>/ /gI' \
    -e 's/<style[^>]*>.*<\/style>/ /gI' \
    -e 's/<[^>]+>/ /g' \
    "$out.raw" \
  | tr '\n\r\t' '   ' \
  | sed -E 's/  +/ /g; s/^ //; s/ $//' \
  | head -c 12000
)
{
  echo "URL: $URL"
  echo "HTTP: $code"
  echo
  echo "$text"
} >"$out"
rm -f "$out.raw"
chars=${#text}
echo "SAFE_WEB_OK $URL chars $chars note $out"

nnotes=$(ls -1 "$NOTES"/*.txt 2>/dev/null | wc -l | tr -d ' ')
echo "$nnotes" > "$DIR/web_notes_count"
ts_iso=$(date +%Y-%m-%dT%H:%M:%S%z)
jq -n \
  --arg ts "$ts_iso" \
  --arg url "$URL" \
  --arg note "$out" \
  --argjson notes_count "$nnotes" \
  '{ts: $ts, url: $url, note: $note, notes_count: $notes_count}' \
  >"$DIR/web_last.json"
