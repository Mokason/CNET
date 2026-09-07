#!/usr/bin/env python3
"""Discord → CNET PEER adapter (IO only — not product brain).

Reads bot token from CNET_DISCORD_TOKEN or CNET_DISCORD_TOKEN_FILE.
Forwards guild/DM text to: cnet_peer --peer discord_<author> "<content>"
Posts ANSWER line back to the channel.

Env:
  CNET_DISCORD_TOKEN / CNET_DISCORD_TOKEN_FILE
  CNET_DISCORD_CHANNELS   comma channel snowflakes (empty = all)
  CNET_PEER_BIN           default cnet_peer
  CNET_DISCORD_PREFIX     if set, only handle messages starting with prefix (e.g. !m )
  CNET_DISCORD_CAPTURE_DIR / CNET_DISCORD_CAPTURE_OWNER / CNET_DISCORD_CAPTURE_CHANNEL
    Optional owner-only DM journal; all three are required when any is set.

Requires: MESSAGE CONTENT intent enabled in Discord Developer Portal.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import websocket  # Existing dependency; never install packages at runtime.
from journal import CaptureError, Journal, snowflake

API = "https://discord.com/api/v10"
UA = "CNET-Marble-Peer (local, 1.0)"


def load_token() -> str:
    t = os.environ.get("CNET_DISCORD_TOKEN", "").strip()
    if t:
        return t
    f = os.environ.get("CNET_DISCORD_TOKEN_FILE", "").strip()
    if not f:
        f = str(Path.home() / ".config/cnet/discord_token")
    return Path(f).read_text(encoding="utf-8").strip()


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def api(method: str, path: str, token: str, body: dict | None = None) -> dict:
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(
        API + path,
        data=data,
        method=method,
        headers={
            "Authorization": "Bot " + token,
            "User-Agent": UA,
            "Content-Type": "application/json",
        },
    )
    with urllib.request.build_opener(NoRedirect()).open(req, timeout=30) as r:
        raw = r.read(262145)
        if len(raw) > 262144:
            raise CaptureError("api_response_limit")
        return json.loads(raw) if raw else {}


def validate_capture_scope(token, owner, channel):
    if not snowflake(owner) or not snowflake(channel):
        raise CaptureError("scope_policy")
    app = api("GET", "/oauth2/applications/@me", token)
    dm = api("GET", "/channels/" + channel, token)
    if (app.get("team") or app.get("owner", {}).get("id") != owner
            or dm.get("id") != channel or dm.get("type") != 1
            or [u.get("id") for u in dm.get("recipients", [])] != [owner]):
        raise CaptureError("scope_verification")


def extract_answer(blob: str) -> str:
    """Discord mouth: speech only. Never protocol, never 'Not sealed.'"""
    draft = 0
    stage = ""
    utter = ""
    answer = ""
    for line in blob.splitlines():
        if line.startswith("STAGE_DRAFT "):
            try:
                draft = int(line[12:].strip() or "0")
            except ValueError:
                draft = 0
        elif line.startswith("STAGE "):
            stage = line[6:].strip()
        elif line.startswith("UTTERANCE "):
            utter = line[10:].strip()
        elif line.startswith("ANSWER "):
            answer = line[7:].strip()

    def jargon(s: str) -> bool:
        t = (s or "").strip()
        if not t or t == "-":
            return True
        low = t.lower()
        if low.startswith("not sealed"):
            return True
        if "logged for improve" in low or "leftover" in low:
            return True
        if "sealed skill" in low or "not certified" in low:
            return True
        if "cnet" in low or "roe-asi" in low or "roe asi" in low:
            return True
        if "brain float" in low or "mokason" in low or "hashtable" in low:
            return True
        if "real-time data" in low or "product lineup" in low:
            return True
        if "lut brick" in low or "capsule propose" in low:
            return True
        if low.startswith("claimed_cert") or low.startswith("stage_draft"):
            return True
        return False

    skill = ""
    for line in blob.splitlines():
        if line.startswith("SKILL "):
            skill = line[6:].strip()
            break
    if skill == "can_do_v1":
        return "I hold sealed skills. Ask a number, a room, or what's on your lane."

    if draft == 1:
        body = stage or utter
        if not jargon(body):
            return body
    if not jargon(utter):
        return utter
    if not jargon(answer):
        return answer
    return "I'm here. What's on your lane?"


def peer_ask(author: str, content: str) -> tuple[str, str]:
    """Forward message text only. Author goes in peer id, not the query.

    Stuffing \"user: text\" into the query broke CERT matching
    (\"mokaith what can you do\" never hits \"what can you do\").
    """
    peer_bin = os.environ.get("CNET_PEER_BIN", "cnet_peer")
    if not shutil.which(peer_bin) and Path("/home/marble/AI/CNET/bin/cnet_peer").is_file():
        peer_bin = "/home/marble/AI/CNET/bin/cnet_peer"
    # peer tag: discord_<author> (sanitized)
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", (author or "user"))[:40]
    peer = f"discord_{safe}" if safe else "discord"
    q = (content or "").strip()
    if not q:
        return "(empty message)", "peer_error"
    # The existing native client's CLI/line protocol has control commands and an
    # 8192-byte buffer. Never let Discord text select flags/commands or truncate.
    if (q in {"PING", "STATUS", "QUIT"} or q.startswith("-")
            or any(char in q for char in "\r\n\0") or len(q.encode("utf-8")) > 8000):
        return "(request cannot be forwarded safely)", "peer_error"
    try:
        p = subprocess.run(
            [peer_bin, "--peer", peer, q],
            capture_output=True,
            text=True,
            timeout=120,
        )
        blob = (p.stdout or "") + (p.stderr or "")
        return extract_answer(blob), "peer_ok" if p.returncode == 0 else "peer_error"
    except Exception:
        # The downstream may have accepted the request before a timeout or I/O error.
        return "(peer outcome unknown)", "peer_unknown"


def stamp_last_origin(cid: str, author: str) -> None:
    """Last Discord origin for nanny follow-up. Not CERT."""
    root = os.environ.get("CNET_MINIMAL_ROOT", str(Path.home() / ".local/share/cnet-minimal/current"))
    path = Path(root) / "var" / "discord_last.json"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps({"channel_id": str(cid), "author": author or "", "ts": int(time.time())})
            + "\n",
            encoding="utf-8",
        )
    except OSError:
        pass


def channel_allowed(cid: str) -> bool:
    raw = os.environ.get("CNET_DISCORD_CHANNELS", "").strip()
    if not raw:
        return True
    allow = {x.strip() for x in raw.split(",") if x.strip()}
    return cid in allow


class Gateway:
    def __init__(self, token: str, journal=None):
        self.token = token
        self.ws = None
        self.seq = None
        self.heartbeat_interval = 41.25
        self.me_id = None
        self._hb_stop = threading.Event()
        self.journal = journal
        self.fatal = False
        self._last_journal_heartbeat = None

    def _heartbeat(self):
        while not self._hb_stop.wait(self.heartbeat_interval):
            if self.ws:
                try:
                    self.ws.send(json.dumps({"op": 1, "d": self.seq}))
                except Exception:
                    break

    def on_message(self, _ws, message: str):
        try:
            self.dispatch(message)
        except Exception:
            # A dispatch or durable capture error must latch before another message.
            self.fatal = True
            print("[discord] fatal dispatch_or_capture_refusal", flush=True)
            self._hb_stop.set()
            self.ws.close()

    def dispatch(self, message: str):
        if self.fatal:
            return
        if len(message) > 262144:
            raise CaptureError("gateway_payload_limit")
        msg = json.loads(message)
        op = msg.get("op")
        t = msg.get("t")
        d = msg.get("d")
        s = msg.get("s")
        if s is not None:
            self.seq = s
            if self.journal:
                self.journal.observe_sequence(s)
        if op == 10:  # hello
            self.heartbeat_interval = float(d["heartbeat_interval"]) / 1000.0
            threading.Thread(target=self._heartbeat, daemon=True).start()
            # identify — intents: GUILDS(1) + GUILD_MESSAGES(512) + MESSAGE_CONTENT(32768) + DMs(4096)
            intents = 1 | 512 | 4096 | 32768
            self.ws.send(
                json.dumps(
                    {
                        "op": 2,
                        "d": {
                            "token": self.token,
                            "intents": intents,
                            "properties": {"os": "linux", "browser": "cnet", "device": "cnet"},
                        },
                    }
                )
            )
            print("[discord] identified intents=", intents, flush=True)
        elif op == 0 and t == "READY":
            self.me_id = d["user"]["id"]
            if self.journal:
                self.journal.event("gateway_ready")
            print("[discord] READY capture=", bool(self.journal), flush=True)
        elif op == 0 and t == "MESSAGE_CREATE":
            self.handle_message(d)
        elif op == 11 and self.me_id and self.journal:
            now = time.monotonic()
            if self._last_journal_heartbeat is None or now - self._last_journal_heartbeat >= 60:
                self.journal.event("gateway_heartbeat")
                self._last_journal_heartbeat = now
        elif op in (7, 9):
            # Reconnect as a new segment; never pretend an unresumed interval is complete.
            self.ws.close()

    def handle_message(self, d: dict):
        if self.fatal:
            return
        if not d or d.get("author", {}).get("bot"):
            return
        if str(d.get("author", {}).get("id")) == str(self.me_id):
            return
        cid = str(d.get("channel_id", ""))
        if not channel_allowed(cid):
            return
        content = (d.get("content") or "").strip()
        if not content:
            return
        prefix = os.environ.get("CNET_DISCORD_PREFIX", "").strip()
        if prefix:
            if not content.startswith(prefix):
                return
            content = content[len(prefix) :].strip()
            if not content:
                return
        captured = self.journal is not None and self.journal.selected(d)
        if captured and not self.journal.begin(d, content):
            print("[discord] duplicate_skipped", flush=True)
            return
        author = d.get("author", {}).get("username") or "user"
        stamp_last_origin(cid, author)
        print("[discord] request captured=", captured, flush=True)
        ans, peer_status = peer_ask(author, content)
        # Discord message max 2000
        if len(ans) > 1900:
            ans = ans[:1900] + "…"
        try:
            reply = api("POST", f"/channels/{cid}/messages", self.token,
                        {"content": ans, "allowed_mentions": {"parse": []}})
            reply_status = "reply_sent" if snowflake(reply.get("id")) else "reply_unknown"
        except urllib.error.HTTPError as e:
            # A 5xx may occur after acceptance. No automatic retries of any POST.
            reply_status = "reply_failed" if 400 <= e.code < 500 else "reply_unknown"
        except Exception:
            reply_status = "reply_unknown"
        if captured:
            self.journal.finish(d["id"], peer_status, reply_status)
        print("[discord] completion", peer_status, reply_status, flush=True)

    def run(self):
        g = api("GET", "/gateway/bot", self.token)
        url = g["url"] + "/?v=10&encoding=json"
        if g["url"] != "wss://gateway.discord.gg":
            raise CaptureError("gateway_endpoint")
        print("[discord] gateway_connect", flush=True)

        # WebSocketApp does not expose redirect_limit in the installed 1.9.0.
        # Its lower-level API does, but also needs an explicit status check.
        self.ws = websocket.WebSocket(enable_multithread=True)
        try:
            self.ws.connect(url, redirect_limit=0, timeout=30)
            if self.ws.getstatus() != 101:
                raise CaptureError("gateway_upgrade")
            self.ws.settimeout(60)
            while self.ws.connected and not self.fatal and not self._hb_stop.is_set():
                message = self.ws.recv()
                if not message:
                    break
                self.on_message(self.ws, message)
        except Exception:
            print("[discord] gateway_error", flush=True)
            if self.journal:
                self.journal.event("gateway_error")
            raise
        finally:
            print("[discord] gateway_close", flush=True)
            self._hb_stop.set()
            self.ws.close()
            if self.journal:
                try:
                    self.journal.event("gateway_close")
                except Exception:
                    self.fatal = True
                    print("[discord] fatal close_capture_refusal", flush=True)

def main() -> int:
    tok = load_token()
    if not tok:
        print("no token", file=sys.stderr)
        return 2
    me = api("GET", "/users/@me", tok)
    if not me.get("bot"):
        raise CaptureError("bot_identity")
    root = os.environ.get("CNET_DISCORD_CAPTURE_DIR", "")
    owner = os.environ.get("CNET_DISCORD_CAPTURE_OWNER", "")
    channel = os.environ.get("CNET_DISCORD_CAPTURE_CHANNEL", "")
    journal = None
    if any((root, owner, channel)):
        if not all((root, owner, channel)):
            raise CaptureError("incomplete_capture_policy")
        validate_capture_scope(tok, owner, channel)
        if not channel_allowed(channel):
            raise CaptureError("capture_channel_not_served")
        journal = Journal(root, owner, channel)
    gateway = Gateway(tok, journal)
    try:
        gateway.run()
    finally:
        if journal:
            journal.close()
    return 1 if gateway.fatal else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        print("[discord] fatal startup_or_runtime_refusal", file=sys.stderr, flush=True)
        raise SystemExit(1)
