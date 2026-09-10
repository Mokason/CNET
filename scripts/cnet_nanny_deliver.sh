#!/usr/bin/env bash
# After gpt-sol improver lands a brick/organ/capsule, speak to last Discord
# origin. CERT 0. Never FAQ. Never prints the bot token.
set -eu
WITNESS="${1:-}"
MIN="${CNET_MINIMAL_ROOT:-$HOME/.local/share/cnet-minimal/current}"
LAST="$MIN/var/discord_last.json"
TOKEN_FILE="${CNET_DISCORD_TOKEN_FILE:-$HOME/.config/cnet/discord_token}"
if [[ -z "$WITNESS" || ! -s "$WITNESS" ]]; then
  echo "deliver skip: no witness"
  exit 0
fi
KIND="$(awk -F= '/^KIND=/{print $2; exit}' "$WITNESS" | tr -d '\r')"
case "$KIND" in
  brick|organ|capsule) ;;
  *) echo "deliver skip: KIND=$KIND"; exit 0 ;;
esac
SPOKEN="$(awk -F= '/^SPOKEN=/{sub(/^SPOKEN=/,""); print; exit}' "$WITNESS" | tr -d '\r')"
if [[ -z "$SPOKEN" ]]; then
  LUT="$(grep -oE '/[^ ]+\.lut' "$WITNESS" | tail -n1 || true)"
  TAG="$(basename "${LUT:-}" .lut)"
  if [[ -n "$TAG" ]]; then
    SPOKEN="Landed $TAG (park CERT). Ask: $TAG 3"
  else
    SPOKEN="Improve landed ($KIND). Not FAQ. Ask the new organ or brick."
  fi
fi
export CNET_NANNY_SPOKEN="$SPOKEN"
export CNET_NANNY_LAST="$LAST"
export CNET_NANNY_TOKEN_FILE="$TOKEN_FILE"
python3 - <<'PY'
import json, os, urllib.error, urllib.request
from pathlib import Path

spoken = os.environ.get("CNET_NANNY_SPOKEN", "").strip()
last_p = Path(os.environ["CNET_NANNY_LAST"])
tok_p = Path(os.environ["CNET_NANNY_TOKEN_FILE"])
if not spoken:
    raise SystemExit(0)

def api(tok, method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(
        "https://discord.com/api/v10" + path,
        data=data,
        method=method,
        headers={
            "Authorization": "Bot " + tok,
            "Content-Type": "application/json",
            "User-Agent": "CNET-Marble-Peer (local, 1.0)",
        },
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read().decode()
        return json.loads(raw) if raw else {}

cid = ""
if last_p.is_file():
    try:
        cid = str(json.loads(last_p.read_text()).get("channel_id") or "")
    except json.JSONDecodeError:
        cid = ""
tok = tok_p.read_text(encoding="utf-8").strip() if tok_p.is_file() else os.environ.get("CNET_DISCORD_TOKEN", "").strip()
if tok and not cid:
    try:
        guilds = api(tok, "GET", "/users/@me/guilds")
        best_ts, best_cid = "", ""
        for g in guilds or []:
            gid = g.get("id")
            if not gid:
                continue
            chans = api(tok, "GET", f"/guilds/{gid}/channels")
            for ch in chans or []:
                if ch.get("type") not in (0, 5):  # text / announce
                    continue
                hid = ch.get("id")
                if not hid:
                    continue
                try:
                    msgs = api(tok, "GET", f"/channels/{hid}/messages?limit=1")
                except urllib.error.HTTPError:
                    continue
                if not msgs:
                    continue
                ts = msgs[0].get("timestamp") or ""
                if ts >= best_ts:
                    best_ts, best_cid = ts, str(hid)
        cid = best_cid
        if cid:
            last_p.parent.mkdir(parents=True, exist_ok=True)
            last_p.write_text(json.dumps({"channel_id": cid, "author": "scan", "ts": 0}) + "\n")
    except Exception as e:
        print(f"deliver scan fail {type(e).__name__}")
if not tok or not cid:
    print(f"deliver skip: token={bool(tok)} cid={bool(cid)}")
    Path("/tmp/cnet-nanny-spoken.txt").write_text(spoken + "\n")
    raise SystemExit(0)
try:
    api(tok, "POST", f"/channels/{cid}/messages", {"content": spoken[:1900]})
    print(f"deliver ok n={len(spoken)}")
except Exception as e:
    print(f"deliver fail {type(e).__name__}")
    Path("/tmp/cnet-nanny-spoken.txt").write_text(spoken + "\n")
    raise SystemExit(1)
PY
