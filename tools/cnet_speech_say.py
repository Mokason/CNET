#!/usr/bin/env python3
"""cnet_speech_say — TTS speak-out for CNET (delivery only, never CERT).

Prefers C-native utterance from cnetd. Default never_voice_llm: only LOCAL.

  python3 tools/cnet_speech_say.py "Hello from Marble"
  python3 tools/cnet_speech_say.py --q "who are you" --play
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
from shutil import which

ROOT = Path(__file__).resolve().parents[1]


def speech_dir() -> Path:
    d = os.environ.get("CNET_SPEECH_DIR", "").strip()
    p = Path(d) if d else ROOT / "artifacts" / "speech"
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


def cnetd_ask_full(q: str) -> dict:
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
        return json.loads(data.decode(errors="replace").strip().splitlines()[0])
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
    if which("ffplay"):
        subprocess.run(
            ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", str(path)],
            check=False,
        )
        return
    if which("mpv"):
        subprocess.run(["mpv", "--no-video", str(path)], check=False)
        return
    if path.suffix.lower() == ".wav" and which("aplay"):
        subprocess.run(["aplay", "-q", str(path)], check=False)
        return
    print(f"(no player; file saved: {path})", file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(description="CNET speech TTS — prefers C utterance")
    ap.add_argument("text", nargs="?", default="")
    ap.add_argument("--text", dest="text_opt", default="")
    ap.add_argument("--q", default="", help="Ask cnetd; speak utterance if may_voice")
    ap.add_argument("--voice", default=os.environ.get("CNET_TTS_VOICE", "en-US-JennyNeural"))
    ap.add_argument("--play", action="store_true")
    ap.add_argument("--out", default="")
    ap.add_argument("--allow-voice-llm", action="store_true")
    args = ap.parse_args()

    text = (args.text_opt or args.text or "").strip()
    meta: dict = {
        "source": "direct",
        "never_self_cert": True,
        "modality": "tts",
        "composer": None,
        "may_voice": True,
    }
    never_voice_llm = os.environ.get("CNET_NEVER_VOICE_LLM", "1") not in ("0", "false", "no")
    if args.allow_voice_llm:
        never_voice_llm = False

    if args.q.strip():
        rep = cnetd_ask_full(args.q.strip())
        src = (rep.get("source") or "").upper()
        may = rep.get("may_voice")
        if may is None:
            may = src == "LOCAL"
        if never_voice_llm and not may:
            print(
                json.dumps(
                    {
                        "ok": False,
                        "error": "never_voice_llm",
                        "source": src,
                        "may_voice": False,
                        "hint": "CNET voices only LOCAL C utterances by default",
                    }
                )
            )
            return 3
        text = (rep.get("utterance") or rep.get("answer") or "").strip()
        text = re.sub(r"^\[llm-(live|untrusted)\]\s*", "", text, flags=re.I)
        meta.update(
            {
                "source": src or "cnetd",
                "query": args.q.strip(),
                "composer": rep.get("composer") or "cnet_utterance",
                "may_voice": bool(may),
                "skill": rep.get("skill"),
            }
        )

    if not text:
        print("usage: cnet_speech_say.py 'text' | --q 'ask'", file=sys.stderr)
        return 2
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
        "never_voice_llm": never_voice_llm,
    }
    with (speech_dir() / "speech_log.jsonl").open("a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    print(json.dumps(rec, ensure_ascii=False))
    if args.play:
        play(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
