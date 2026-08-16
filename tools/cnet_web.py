#!/usr/bin/env python3
"""cnet-web — thin Tailscale/localhost HTTP cockpit over cnetd UNIX socket.

No npm. Stdlib only. Does not auto-CERT or write pack_personal.

  python3 tools/cnet_web.py
  CNET_WEB_HOST=100.x.x.x CNET_WEB_PORT=8642 python3 tools/cnet_web.py

Env:
  CNET_SOCK, CNET_MINIMAL_ROOT, CNET_PACKS_ROOT
  CNET_WEB_HOST (default: tailscale0 IP or 127.0.0.1)
  CNET_WEB_PORT (default: 8642)
  CNET_WEB_TOKEN      REQUIRED. Sent as `Authorization: Bearer <token>`.
                      The ?token= query form was removed (it leaks into logs,
                      browser history and Referer headers). The server refuses
                      to start without a token unless CNET_WEB_NO_AUTH=1.
  CNET_WEB_NO_AUTH    set to 1 to serve with NO authentication, on purpose.
  CNET_WEB_ALLOWED_HOSTS  comma-separated extra Host: values to accept.
                      Requests whose Host is not the bind address (or a
                      loopback spelling) are refused, to block DNS rebinding.
  CNET_WEB_MAX_BODY   max request body in bytes (default 1 MiB).
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import threading
import urllib.parse
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATIC = ROOT / "web" / "cnet-cockpit.html"
PORT = int(os.environ.get("CNET_WEB_PORT", "8642"))
TOKEN = os.environ.get("CNET_WEB_TOKEN", "").strip()
# Cap the request body. BaseHTTPRequestHandler will happily read whatever
# Content-Length claims, so an unauthenticated caller could previously make
# the process allocate arbitrarily much before auth was even consulted.
MAX_BODY = int(os.environ.get("CNET_WEB_MAX_BODY", str(1 << 20)))
# Filled in by main() once the bind address is known.
ALLOWED_HOSTS: set[str] = set()


def packs_root() -> Path:
    p = os.environ.get("CNET_PACKS_ROOT", "").strip()
    if p:
        return Path(p)
    m = os.environ.get("CNET_MINIMAL_ROOT", "").strip()
    if m:
        return Path(m) / "data" / "roe_daily_packs"
    return ROOT / "artifacts" / "roe_daily_packs"


def sock_path() -> str:
    s = os.environ.get("CNET_SOCK", "").strip()
    if s:
        return s
    xdg = os.environ.get("XDG_RUNTIME_DIR", "").strip()
    if xdg:
        p = Path(xdg) / "cnet" / "cnet.sock"
        if p.exists() or True:
            return str(p)
    return str(Path.home() / ".local/share/cnet-minimal/run/cnet.sock")


def tailscale_ip() -> str | None:
    try:
        out = subprocess.check_output(
            ["tailscale", "ip", "-4"], text=True, timeout=3, stderr=subprocess.DEVNULL
        ).strip()
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("100."):
                return line
    except Exception:
        pass
    # ip -4 addr show tailscale0
    try:
        out = subprocess.check_output(
            ["ip", "-4", "-o", "addr", "show", "dev", "tailscale0"],
            text=True,
            timeout=3,
            stderr=subprocess.DEVNULL,
        )
        for part in out.split():
            if part.startswith("100.") and "/" in part:
                return part.split("/")[0]
    except Exception:
        pass
    return None


def bind_host() -> str:
    h = os.environ.get("CNET_WEB_HOST", "").strip()
    if h:
        return h
    ts = tailscale_ip()
    return ts if ts else "127.0.0.1"


def cnetd_ask(q: str, as_json: bool = True) -> dict | str:
    path = sock_path()
    if not Path(path).exists():
        return {"ok": False, "error": f"cnetd socket missing: {path}"}
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(120)
    try:
        s.connect(path)
        if as_json:
            payload = json.dumps({"op": "ask", "q": q}, ensure_ascii=False) + "\n"
        else:
            payload = "ASK " + q + "\n"
        s.sendall(payload.encode())
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if as_json and data.endswith(b"\n"):
                break
            if not as_json and b"\nEND\n" in data:
                break
        text = data.decode(errors="replace")
        if as_json:
            try:
                return json.loads(text.strip().splitlines()[0])
            except json.JSONDecodeError:
                return {"ok": False, "error": "bad_json", "raw": text[:500]}
        return text
    except OSError as e:
        return {"ok": False, "error": str(e)}
    finally:
        try:
            s.close()
        except OSError:
            pass


def cnetd_status() -> dict:
    path = sock_path()
    if not Path(path).exists():
        return {"ok": False, "error": "no_socket", "sock": path}
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path)
        s.sendall(b"STATUS\n")
        data = s.recv(4096).decode(errors="replace")
        return {"ok": True, "status": data.strip(), "sock": path}
    except OSError as e:
        return {"ok": False, "error": str(e), "sock": path}
    finally:
        try:
            s.close()
        except OSError:
            pass


def tail_jsonl(path: Path, n: int = 40, organic_only: bool = False) -> list:
    if not path.is_file():
        return []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    rows = []
    for ln in lines[-(n * 3) :]:
        ln = ln.strip()
        if not ln:
            continue
        try:
            r = json.loads(ln)
        except json.JSONDecodeError:
            continue
        if organic_only and (
            r.get("shortcircuit")
            or "novel fact" in (r.get("query") or "").lower()
            or "mystic" in (r.get("query") or "").lower()
        ):
            continue
        rows.append(r)
    return rows[-n:]


def read_neuromod() -> dict:
    p = ROOT / "logs" / "governor" / "neuromod_state.json"
    if not p.is_file():
        p = ROOT / "logs" / "governor" / "schedule_gate.json"
    if not p.is_file():
        return {}
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def read_autonomous_kpi() -> dict:
    p = ROOT / "logs" / "marble_24_7" / "AUTONOMOUS_CYCLE.json"
    if not p.is_file():
        return {}
    try:
        d = json.loads(p.read_text(encoding="utf-8"))
        return {"ts": d.get("ts"), "kpi": d.get("kpi"), "duration_s": d.get("duration_s")}
    except json.JSONDecodeError:
        return {}


def explore_queue(n: int = 30) -> list:
    p = packs_root() / "curriculum_explore.jsonl"
    return tail_jsonl(p, n=n)


def approve_explore(explore_id: str, query: str, answer: str) -> dict:
    """Write gold file only — never pack_personal / never CERT seal."""
    import hashlib
    import re

    pr = packs_root()
    gold = pr / "gold"
    gold.mkdir(parents=True, exist_ok=True)
    nq = re.sub(r"\s+", " ", (query or "").strip().lower())[:200]
    h = hashlib.sha1(nq.encode()).hexdigest()[:16]
    path = gold / f"{h}.txt"
    path.write_text((answer or "").replace("\n", " ").strip()[:800] + "\n", encoding="utf-8")
    # mark review log
    rev = pr / "explore_review.jsonl"
    with rev.open("a", encoding="utf-8") as f:
        f.write(
            json.dumps(
                {
                    "action": "approve_gold",
                    "explore_id": explore_id,
                    "query": query,
                    "gold_sha": h,
                    "auto_cert": False,
                    "writes_pack_personal": False,
                },
                ensure_ascii=False,
            )
            + "\n"
        )
    return {
        "ok": True,
        "gold_path": str(path),
        "gold_sha": h,
        "auto_cert": False,
        "note": "gold_file only; evolve may promote later under charter",
    }


def reject_explore(explore_id: str, query: str) -> dict:
    pr = packs_root()
    rev = pr / "explore_review.jsonl"
    with rev.open("a", encoding="utf-8") as f:
        f.write(
            json.dumps(
                {
                    "action": "reject",
                    "explore_id": explore_id,
                    "query": query,
                    "auto_cert": False,
                },
                ensure_ascii=False,
            )
            + "\n"
        )
    return {"ok": True, "rejected": explore_id}


def speech_out_dir() -> Path:
    d = os.environ.get("CNET_SPEECH_DIR", "").strip()
    if d:
        p = Path(d)
    else:
        p = ROOT / "artifacts" / "speech"
    p.mkdir(parents=True, exist_ok=True)
    return p


def python_bin() -> str:
    for c in (
        os.environ.get("CNET_PYTHON", "").strip(),
        str(Path.home() / ".hermes/hermes-agent/venv/bin/python3"),
        "/usr/bin/python3",
        "python3",
    ):
        if not c:
            continue
        p = Path(c) if c.startswith("/") else None
        if p and p.is_file():
            return c
        if not c.startswith("/"):
            return c
    return "python3"


def run_speech_say(text: str = "", q: str = "", play: bool = False, allow_voice_llm: bool = False) -> dict:
    """Delivery-only TTS. Prefers C utterance; never_voice_llm by default."""
    import subprocess

    script = ROOT / "tools" / "cnet_speech_say.py"
    if not script.is_file():
        return {"ok": False, "error": "cnet_speech_say.py missing"}
    cmd = [python_bin(), str(script)]
    if q.strip():
        cmd.extend(["--q", q.strip()])
    elif text.strip():
        cmd.extend(["--text", text.strip()])
    else:
        return {"ok": False, "error": "empty_text"}
    if play:
        cmd.append("--play")
    if allow_voice_llm:
        cmd.append("--allow-voice-llm")
    env = os.environ.copy()
    env["CNET_SPEECH_DIR"] = str(speech_out_dir())
    env.setdefault("CNET_NEVER_VOICE_LLM", "1")
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=env, cwd=str(ROOT))
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "tts_timeout"}
    out = (p.stdout or "").strip().splitlines()
    last = out[-1] if out else ""
    try:
        rec = json.loads(last) if last.startswith("{") else {"ok": p.returncode == 0, "raw": last, "stderr": (p.stderr or "")[:300]}
    except json.JSONDecodeError:
        rec = {"ok": False, "error": "bad_tts_json", "stderr": (p.stderr or "")[:300], "raw": last[:200]}
    rec["auto_cert"] = False
    rec["never_self_cert"] = True
    rec["web_promote"] = False
    if rec.get("ok") and rec.get("path"):
        name = Path(rec["path"]).name
        rec["url"] = f"/api/speech/{name}"
    return rec


class Handler(BaseHTTPRequestHandler):
    server_version = "cnet-web/1.0"

    def log_message(self, format, *args):  # noqa: A003 — BaseHTTPRequestHandler API
        # Strip the query string before logging. The request line reaches this
        # verbatim, so while ?token= was still accepted every authenticated
        # request wrote the bearer token to stderr -- and from there into the
        # systemd journal, which has a different (wider) audience than the
        # token does.
        msg = format % args
        if "?" in msg:
            head, _, tail = msg.partition("?")
            rest = tail.split(" ", 1)
            msg = head + "?<redacted>" + ((" " + rest[1]) if len(rest) > 1 else "")
        sys.stderr.write("%s - %s\n" % (self.address_string(), msg))

    def _host_ok(self) -> bool:
        """Reject requests whose Host header is not the address we bound.

        Without this the cockpit answers to any name that resolves here, which
        is what makes DNS-rebinding work: a page on an attacker's origin points
        a hostname at 100.x, the browser treats the response as same-origin
        with the attacker, and every authenticated endpoint is reachable from
        their JavaScript.
        """
        host = (self.headers.get("Host") or "").strip()
        if not host:
            return False
        # Strip the port, handling bracketed IPv6.
        if host.startswith("["):
            name = host[1:host.index("]")] if "]" in host else host
        else:
            name = host.rsplit(":", 1)[0] if host.count(":") == 1 else host
        return name.lower() in ALLOWED_HOSTS

    def _auth_ok(self) -> bool:
        # TOKEN is mandatory (main() refuses to start without one), so there is
        # deliberately no "no token configured -> allow" branch here any more.
        auth = self.headers.get("Authorization", "")
        expected = f"Bearer {TOKEN}"
        # compare_digest, not ==: a plain comparison returns on the first
        # differing byte, which leaks the token prefix to anyone who can time
        # requests. Cheap to do right.
        if len(auth) == len(expected) and hmac.compare_digest(auth, expected):
            return True
        # The ?token= form is GONE. Authorization: Bearer already works, and a
        # token in the query string lands in logs, browser history, and any
        # Referer sent to a third party.
        return False

    def _send(self, code: int, body: bytes, ctype: str = "application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, obj: dict | list):
        self._send(code, json.dumps(obj, ensure_ascii=False).encode(), "application/json; charset=utf-8")

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")
        self.end_headers()

    def do_GET(self):
        if not self._host_ok():
            self._json(421, {"ok": False, "error": "bad_host"})
            return
        if not self._auth_ok():
            self._json(401, {"ok": False, "error": "unauthorized"})
            return
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        if path in ("/", "/index.html"):
            html = STATIC.read_bytes() if STATIC.is_file() else b"<h1>cnet-web missing cockpit html</h1>"
            self._send(200, html, "text/html; charset=utf-8")
            return
        if path == "/api/health":
            self._json(
                200,
                {
                    "ok": True,
                    "service": "cnet-web",
                    "never_self_cert": True,
                    "speech_capsule": True,
                },
            )
            return
        if path.startswith("/api/speech/"):
            name = path.split("/api/speech/", 1)[-1]
            name = Path(name).name  # no path traversal
            fp = speech_out_dir() / name
            if not fp.is_file() or fp.suffix.lower() not in (".mp3", ".wav", ".ogg"):
                self._json(404, {"ok": False, "error": "not_found"})
                return
            data = fp.read_bytes()
            ctype = "audio/mpeg" if fp.suffix.lower() == ".mp3" else "application/octet-stream"
            self._send(200, data, ctype)
            return
        if path == "/api/status":
            nm = read_neuromod()
            levels = nm.get("levels") or nm
            self._json(
                200,
                {
                    "ok": True,
                    "cnetd": cnetd_status(),
                    "neuromod": {
                        "dopamine": levels.get("dopamine"),
                        "serotonin": levels.get("serotonin"),
                        "adenosine": levels.get("adenosine"),
                    },
                    "schedule": nm if "pause_grow_probes" in nm else {},
                    "autonomous": read_autonomous_kpi(),
                    "packs": str(packs_root()),
                    "never_self_cert": True,
                },
            )
            return
        if path == "/api/miss_log":
            n = int(qs.get("n", ["40"])[0] or 40)
            organic = qs.get("organic", ["0"])[0] in ("1", "true", "yes")
            miss = packs_root() / "miss_log.jsonl"
            # prefer shared var log if present
            var = Path.home() / ".local/share/cnet-minimal/var/miss_log.jsonl"
            if var.is_file():
                miss = var
            self._json(200, {"ok": True, "rows": tail_jsonl(miss, n=n, organic_only=organic)})
            return
        if path == "/api/explore":
            n = int(qs.get("n", ["30"])[0] or 30)
            self._json(200, {"ok": True, "rows": explore_queue(n), "auto_cert": False})
            return
        self._json(404, {"ok": False, "error": "not_found"})

    def do_POST(self):
        if not self._host_ok():
            self._json(421, {"ok": False, "error": "bad_host"})
            return
        if not self._auth_ok():
            self._json(401, {"ok": False, "error": "unauthorized"})
            return
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            self._json(400, {"ok": False, "error": "bad_content_length"})
            return
        if length < 0 or length > MAX_BODY:
            self._json(413, {"ok": False, "error": "payload_too_large",
                             "max_bytes": MAX_BODY})
            return
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw.decode() or "{}")
        except json.JSONDecodeError:
            self._json(400, {"ok": False, "error": "bad_json"})
            return

        if path == "/api/ask":
            q = (body.get("q") or body.get("query") or "").strip()
            if not q:
                self._json(400, {"ok": False, "error": "empty_query"})
                return
            # hard law: web cannot request promote
            if body.get("promote") or body.get("accept"):
                self._json(
                    403,
                    {
                        "ok": False,
                        "error": "promote_forbidden",
                        "law": "never_self_cert; use explore approve→gold only",
                    },
                )
                return
            rep = cnetd_ask(q, as_json=True)
            if isinstance(rep, dict):
                rep["never_self_cert"] = True
                rep["web_promote"] = False
            self._json(200 if isinstance(rep, dict) and rep.get("ok", True) else 502, rep if isinstance(rep, dict) else {"ok": False, "raw": rep})
            return

        if path == "/api/explore/approve":
            # gold only
            r = approve_explore(
                body.get("explore_id") or "",
                body.get("query") or "",
                body.get("answer") or "",
            )
            self._json(200, r)
            return
        if path == "/api/explore/reject":
            r = reject_explore(body.get("explore_id") or "", body.get("query") or "")
            self._json(200, r)
            return
        if path == "/api/speak":
            if body.get("promote") or body.get("accept"):
                self._json(403, {"ok": False, "error": "promote_forbidden", "law": "never_self_cert"})
                return
            text = (body.get("text") or body.get("answer") or body.get("utterance") or "").strip()
            q = (body.get("q") or body.get("query") or "").strip()
            play = bool(body.get("play"))
            allow = bool(body.get("allow_voice_llm"))
            # Prefer re-ask via q so C utterance is used
            r = run_speech_say(text=text, q=q, play=play, allow_voice_llm=allow)
            self._json(200 if r.get("ok") else 502, r)
            return

        self._json(404, {"ok": False, "error": "not_found"})


def main() -> int:
    global ALLOWED_HOSTS

    host = bind_host()
    # refuse 0.0.0.0 unless explicitly forced
    if host in ("0.0.0.0", "::") and os.environ.get("CNET_WEB_ALLOW_PUBLIC") != "1":
        print("refusing public bind; set CNET_WEB_HOST=100.x or 127.0.0.1", file=sys.stderr)
        host = "127.0.0.1"

    # FAIL CLOSED. CNET_WEB_TOKEN used to be optional, and when unset _auth_ok
    # returned True for everything -- so the default posture of a cockpit bound
    # on a Tailscale 100.x address was "no authentication at all", reachable by
    # every device on the tailnet. It can read miss logs and drive /api/ask.
    # Refusing to start is the only safe default; an operator who genuinely
    # wants it open must say so out loud.
    if not TOKEN:
        if os.environ.get("CNET_WEB_NO_AUTH") == "1":
            print("WARNING: CNET_WEB_NO_AUTH=1 — serving with NO authentication",
                  file=sys.stderr)
        else:
            print("refusing to start: CNET_WEB_TOKEN is not set.\n"
                  "  export CNET_WEB_TOKEN=\"$(python -c "
                  "'import secrets;print(secrets.token_urlsafe(32))')\"\n"
                  "  (or set CNET_WEB_NO_AUTH=1 to serve unauthenticated on purpose)",
                  file=sys.stderr)
            return 2

    # Host header allowlist: exactly what we bound, plus the loopback spellings
    # a local browser will send.
    ALLOWED_HOSTS = {host.lower(), "localhost", "127.0.0.1", "::1", "[::1]"}
    extra = os.environ.get("CNET_WEB_ALLOWED_HOSTS", "").strip()
    if extra:
        ALLOWED_HOSTS |= {h.strip().lower() for h in extra.split(",") if h.strip()}

    if not STATIC.is_file():
        print(f"missing cockpit {STATIC}", file=sys.stderr)
        return 1
    httpd = ThreadingHTTPServer((host, PORT), Handler)
    print(f"cnet-web http://{host}:{PORT}/  sock={sock_path()}  token={'set' if TOKEN else 'OFF'}")
    print(f"allowed_hosts={sorted(ALLOWED_HOSTS)}  max_body={MAX_BODY}")
    print("never_self_cert=1  promote_via_web=forbidden")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
