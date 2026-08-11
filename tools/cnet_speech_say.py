#!/usr/bin/env python3
"""cnet_speech_say — TTS speak-out for CNET (delivery only, never CERT).

Uses edge-tts (already in env). Writes mp3 under artifacts/speech/ or CNET_SPEECH_DIR.
Optional --play via ffplay/aplay.

  python3 tools/cnet_speech_say.py "Hello from Marble"
  python3 tools/cnet_speech_say.py --q "who are you" --play   # ask cnetd then speak
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def speech_dir() -> Path:
    d = os.environ.get("CNET_SPEECH_DIR", "").strip()
    if d:
        p = Path(d)
    else:
        p = ROOT / "artifacts" / "speech"
    p.mkdir(parents=True, exist_ok=True)
    return p


def sock_path() -> str:
    s = os.environ.get("CNET_SOCK", "").strip()
    if s:
        return s
    xdg = os.environ.get("XDG_RUNTIME_DIR", "")
    if xdg:
        return str(Path(xdg) / "cnet" / "cnet.sock")
    return str(Path.home() / ".local/share/cnet-minimal/run/cnet.sock")


def cnetd_ask(q: str) -> str:
    path = sock_path()
    if not Path(path).exists():
        raise SystemExit(f"cnetd socket missing: {path}")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(120)
    try:
        s.connect(path)
        s.sendall((json.dumps({"op": "ask", "q": q}) + "\n").encode())
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if data.endswith(b"\n"):
                break
        rep = json.loads(data.decode(errors="replace").strip().splitlines()[0])
        ans = (rep.get("answer") or "").strip()
        # strip untrusted prefix for cleaner speech
        ans = re.sub(r"^\[llm-(live|untrusted)\]\s*", "", ans, flags=re.I)
        return ans
    finally:
        s.close()


def slug(s: str, n: int = 40) -> str:
    s = re.sub(r"[^a-zA-Z0-9]+", "_", s.strip())[:n].strip("_")
    return s or "utterance"


async def synth(text: str, out: Path, voice: str) -> None:
    import edge_tts

    communicate = edge_tts.Communicate(text, voice)
    await communicate.save(str(out))


def play(path: Path) -> None:
    if shutil_which("ffplay"):
        subprocess.run(
            ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", str(path)],
            check=False,
        )
        return
    if shutil_which("mpv"):
        subprocess.run(["mpv", "--no-video", str(path)], check=False)
        return
    # mp3 via ffmpeg pipe to aplay if wav
    if path.suffix.lower() == ".wav" and shutil_which("aplay"):
        subprocess.run(["aplay", "-q", str(path)], check=False)
        return
    print(f"(no player; file saved: {path})", file=sys.stderr)


def shutil_which(cmd: str) -> str | None:
    from shutil import which

    return which(cmd)


def main() -> int:
    ap = argparse.ArgumentParser(description="CNET speech capsule TTS (never CERT)")
    ap.add_argument("text", nargs="?", default="", help="Text to speak")
    ap.add_argument("--text", dest="text_opt", default="", help="Text to speak")
    ap.add_argument("--q", default="", help="Ask cnetd first, then speak answer")
    ap.add_argument("--voice", default=os.environ.get("CNET_TTS_VOICE", "en-US-JennyNeural"))
    ap.add_argument("--play", action="store_true", help="Play on local audio")
    ap.add_argument("--out", default="", help="Output mp3 path")
    args = ap.parse_args()

    text = (args.text_opt or args.text or "").strip()
    meta = {"source": "direct", "never_self_cert": True, "modality": "tts"}
    if args.q.strip():
        text = cnetd_ask(args.q.strip())
        meta["source"] = "cnetd"
        meta["query"] = args.q.strip()
    if not text:
        print("usage: cnet_speech_say.py 'text' | --q 'ask cnetd'", file=sys.stderr)
        return 2

    # safety: don't speak huge blobs
    if len(text) > 2500:
        text = text[:2500] + "…"

    out = Path(args.out) if args.out else speech_dir() / f"{int(time.time())}_{slug(text)}.mp3"
    out.parent.mkdir(parents=True, exist_ok=True)

    try:
        asyncio.run(synth(text, out, args.voice))
    except Exception as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        return 1

    rec = {
        "ok": True,
        "path": str(out.resolve()),
        "bytes": out.stat().st_size,
        "voice": args.voice,
        "text_preview": text[:160],
        **meta,
        "auto_cert": False,
    }
    # sidecar log for harvest (delivery telemetry only)
    logp = speech_dir() / "speech_log.jsonl"
    with logp.open("a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    print(json.dumps(rec, ensure_ascii=False))
    if args.play:
        play(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
