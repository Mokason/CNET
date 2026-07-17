#!/usr/bin/env python3
"""CNET external voice teacher (closed-set speech commands).

Protocol (line-oriented, one call per line) — matches
external_teacher_bind_subprocess:

  stdin:  IN <dim> d0 d1 ... d{dim-1}
  stdout: OUT <n_cmd> o0 o1 ... o{n_cmd-1}   # one-hot command

Modes:
  hermetic  — L2 match of frozen frontend features (no ML deps; gate default)
  whisper   — real ASR via faster-whisper / openai-whisper when available;
              feature vectors are still mapped by hermetic frontend for mine
              probes; use --wav / --label-wav for real audio labeling

Also supports offline wav labeling (not the line protocol):
  python3 tools/voice_teacher.py --mode whisper --label-wav path.wav
  → prints command name + class index

Env:
  CNET_VOICE_TEACHER_PYTHON  preferred interpreter (optional)
  CNET_VOICE_WHISPER_MODEL   default tiny
"""
from __future__ import annotations

import argparse
import os
import sys
from typing import List, Optional, Sequence, Tuple

COMMANDS: Tuple[str, ...] = (
    "yes",
    "no",
    "up",
    "down",
    "left",
    "right",
    "on",
    "off",
    "stop",
    "go",
)
FEAT_DIM = 16
N_CMD = len(COMMANDS)


def hermetic_features(class_id: int) -> List[float]:
    """Mirror src/modality_voice.c cnet_voice_hermetic_features."""
    if class_id < 0 or class_id >= N_CMD:
        class_id = 0
    name = COMMANDS[class_id]
    h = 2166136261
    for ch in name.encode("utf-8"):
        h ^= ch
        h = (h * 16777619) & 0xFFFFFFFF
    h ^= (class_id * 0x9E3779B9) & 0xFFFFFFFF
    h &= 0xFFFFFFFF
    out: List[float] = []
    for k in range(FEAT_DIM):
        bit = (h >> (k % 32)) & 1
        out.append(0.85 if bit else 0.15)
        h = (h * 1664525 + 1013904223) & 0xFFFFFFFF
    return out


# Precompute once — classify is O(n_cmd) with fixed refs (not re-hashed each call).
_HERMETIC_TABLE: Tuple[Tuple[float, ...], ...] = tuple(
    tuple(hermetic_features(c)) for c in range(N_CMD)
)
_ONEHOT_TABLE: Tuple[Tuple[float, ...], ...] = tuple(
    tuple(1.0 if i == c else 0.0 for i in range(N_CMD)) for c in range(N_CMD)
)


def hermetic_classify(feat: Sequence[float]) -> int:
    best, best_d = 0, 1e300
    for c, ref in enumerate(_HERMETIC_TABLE):
        d = 0.0
        for a, b in zip(feat, ref):
            e = float(a) - float(b)
            d += e * e
        if d < best_d:
            best_d = d
            best = c
    return best


def onehot(c: int) -> List[float]:
    if 0 <= c < N_CMD:
        return list(_ONEHOT_TABLE[c])
    return [1.0 if i == c else 0.0 for i in range(N_CMD)]


def map_transcript_to_class(text: str) -> Optional[int]:
    t = (text or "").strip().lower()
    if not t:
        return None
    # Prefer whole-word match over substring order.
    words = [w.strip(".,!?;:\"'") for w in t.replace("-", " ").split()]
    for w in words:
        if w in COMMANDS:
            return COMMANDS.index(w)
    for i, cmd in enumerate(COMMANDS):
        if cmd in t:
            return i
    return None


def load_whisper(model_name: str):
    """Return (backend_name, model) or raise."""
    # Prefer faster-whisper (available in Hermes venv).
    try:
        from faster_whisper import WhisperModel  # type: ignore

        model = WhisperModel(model_name, device="cpu", compute_type="int8")
        return "faster_whisper", model
    except Exception:
        pass
    try:
        import whisper  # type: ignore

        model = whisper.load_model(model_name)
        return "openai_whisper", model
    except Exception as e:
        raise RuntimeError(
            "whisper backend unavailable (install faster-whisper or openai-whisper)"
        ) from e


def asr_wav(backend: str, model, wav_path: str) -> str:
    if backend == "faster_whisper":
        segments, _info = model.transcribe(wav_path, beam_size=1, language="en")
        parts = [s.text for s in segments]
        return " ".join(parts).strip()
    # openai-whisper
    result = model.transcribe(wav_path, language="en", fp16=False)
    return (result.get("text") or "").strip()


def classify_feat(mode: str, feat: Sequence[float], whisper_state) -> int:
    """Feature-path teacher: hermetic always for feature vectors.
    Whisper mode still uses hermetic on feature probes (encoder stand-in);
    real audio uses --label-wav.
    """
    del mode, whisper_state
    return hermetic_classify(feat)


def serve_line_protocol(mode: str, model_name: str) -> int:
    whisper_state = None
    if mode == "whisper":
        # Lazy-load only if operator forces feature-path with whisper (no-op
        # for features); real value is --label-wav / readiness check.
        try:
            whisper_state = load_whisper(model_name)
            print(
                f"# voice_teacher whisper ready backend={whisper_state[0]} "
                f"model={model_name}",
                file=sys.stderr,
            )
        except Exception as e:
            print(f"# voice_teacher whisper load failed: {e}", file=sys.stderr)
            print("# falling back to hermetic feature teacher", file=sys.stderr)
            mode = "hermetic"

    for raw in sys.stdin:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 2 or parts[0] != "IN":
            print("ERR bad_line", flush=True)
            continue
        try:
            dim = int(parts[1])
        except ValueError:
            print("ERR bad_dim", flush=True)
            continue
        nums = parts[2:]
        if len(nums) < dim:
            print("ERR short_in", flush=True)
            continue
        feat = [float(x) for x in nums[:dim]]
        # Pad/truncate to FEAT_DIM for hermetic match
        if len(feat) < FEAT_DIM:
            feat = feat + [0.0] * (FEAT_DIM - len(feat))
        elif len(feat) > FEAT_DIM:
            feat = feat[:FEAT_DIM]
        cls = classify_feat(mode, feat, whisper_state)
        out = onehot(cls)
        sys.stdout.write(f"OUT {N_CMD}")
        for v in out:
            sys.stdout.write(f" {v:.17g}")
        sys.stdout.write("\n")
        sys.stdout.flush()
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    p = argparse.ArgumentParser(description="CNET voice external teacher")
    p.add_argument(
        "--mode",
        choices=("hermetic", "whisper"),
        default=os.environ.get("CNET_VOICE_TEACHER_MODE", "hermetic"),
    )
    p.add_argument(
        "--model",
        default=os.environ.get("CNET_VOICE_WHISPER_MODEL", "tiny"),
        help="Whisper model size/name (tiny, base, ...)",
    )
    p.add_argument(
        "--label-wav",
        metavar="PATH",
        help="Transcribe wav and map to closed-set command (whisper mode)",
    )
    p.add_argument(
        "--check",
        action="store_true",
        help="Print backend readiness and exit",
    )
    args = p.parse_args(argv)

    if args.check:
        print(f"mode={args.mode} n_cmd={N_CMD} feat_dim={FEAT_DIM}")
        print("commands=" + ",".join(COMMANDS))
        if args.mode == "whisper":
            try:
                backend, _m = load_whisper(args.model)
                print(f"whisper_ok backend={backend} model={args.model}")
                return 0
            except Exception as e:
                print(f"whisper_unavailable: {e}")
                return 1
        print("hermetic_ok")
        return 0

    if args.label_wav:
        if args.mode != "whisper":
            print("label-wav requires --mode whisper", file=sys.stderr)
            return 2
        backend, model = load_whisper(args.model)
        text = asr_wav(backend, model, args.label_wav)
        cls = map_transcript_to_class(text)
        print(f"transcript={text!r}")
        if cls is None:
            print("class=-1 command=UNKNOWN")
            return 3
        print(f"class={cls} command={COMMANDS[cls]}")
        return 0

    return serve_line_protocol(args.mode, args.model)


if __name__ == "__main__":
    sys.exit(main())
