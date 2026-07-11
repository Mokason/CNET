#!/usr/bin/env python3
"""Deterministic coherence checks for cnet_llama_eval JSONL output."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def final_answer(response: str) -> tuple[str, bool]:
    if "<think>" not in response:
        return response.strip(), True
    if "</think>" not in response:
        return "", False
    return response.split("</think>", 1)[1].strip(), True


def sentence_count(text: str) -> int:
    return len([part for part in re.split(r"(?<=[.!?])\s+", text.strip()) if part])


def recommends_banned_method(text: str, stem: str) -> bool:
    """Detect a recommendation, not an explicit warning to avoid a method."""
    for match in re.finditer(re.escape(stem), text):
        prefix = text[max(0, match.start() - 48):match.start()]
        # Permit up to two modifiers between the avoidance instruction and the
        # method, e.g. "away from direct sunlight" or "do not apply high heat".
        if re.search(
            r"(?:avoid(?:ing)?|away from|out of|do not|don't|never|no|without)"
            r"(?:\W+\w+){0,2}\W*$",
            prefix,
        ):
            continue
        return True
    return False


def score_case(row: dict) -> dict:
    case_id = row["id"]
    answer, reasoning_closed = final_answer(row.get("response", ""))
    checks: dict[str, bool] = {
        "reasoning_closed": reasoning_closed,
        "nonempty_final": bool(answer),
    }

    lower = answer.lower()
    if case_id == "causal_reasoning":
        checks.update(
            {
                "exactly_three_sentences": sentence_count(answer) == 3,
                "shadow_cause": any(
                    term in lower
                    for term in (
                        "sun is lower",
                        "sun's angle",
                        "sun angle",
                        "sun's lower angle",
                        "oblique angle",
                        "more obliquely",
                    )
                ),
                "mass_constant": "mass" in lower and any(term in lower for term in ("unchanged", "constant", "same")),
            }
        )
    elif case_id == "constraint_following":
        matches = list(re.finditer(r"(?:^|\s)([1-4])\.\s+", answer))
        steps = []
        if len(matches) == 4 and [match.group(1) for match in matches] == ["1", "2", "3", "4"]:
            for index, match in enumerate(matches):
                end = matches[index + 1].start() if index + 1 < len(matches) else len(answer)
                steps.append(answer[match.end():end].strip())
        numbered = len(steps) == 4
        checks.update(
            {
                "exactly_four_numbered_steps": numbered,
                "one_sentence_per_step": numbered and all(
                    sentence_count(step) == 1 for step in steps
                ),
                "avoids_banned_methods": not any(
                    recommends_banned_method(lower, banned)
                    for banned in ("heat", "sunlight", "laminat")
                ),
            }
        )
    elif case_id == "narrative_continuity":
        words = re.findall(r"\b[\w'-]+\b", answer)
        paragraphs = [part for part in re.split(r"\n\s*\n", answer) if part.strip()]
        choice_present = (
            "mara" in lower
            and "map" in lower
            and "stranger" in lower
            and any(
                term in lower
                for term in (
                    "pulled the stranger",
                    "helped the stranger",
                    "save the stranger",
                    "saved the stranger",
                    "dragged the stranger",
                    "chose the stranger",
                    "grabbed the stranger",
                    "hauling him",
                    "dragged the man",
                    "helping him",
                    "guiding him",
                )
            )
        )
        last_sentence = re.split(r"(?<=[.!?])\s+", answer.strip())[-1].lower() if answer else ""
        map_loss_consequence = any(
            term in last_sentence
            for term in ("map", "route", "coordinate", "direction", "navigate", "location", "without it")
        )
        checks.update(
            {
                "single_paragraph": len(paragraphs) == 1,
                "within_140_words": len(words) <= 140,
                "choice_preserved": choice_present,
                "consequence_caused_by_map_loss": map_loss_consequence,
                "no_magic": "magic" not in lower and "spell" not in lower,
            }
        )
    else:
        checks["known_case"] = False

    passed = all(checks.values())
    return {
        "id": case_id,
        "passed": passed,
        "checks": checks,
        "final_answer": answer,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    raw_text = args.results.read_bytes().decode("utf-8", errors="replace")
    rows = [json.loads(line) for line in raw_text.splitlines() if line.strip()]
    scored = [score_case(row) for row in rows]
    passed = sum(1 for item in scored if item["passed"])
    average_tps = sum(float(row.get("tokens_per_second", 0.0)) for row in rows) / len(rows) if rows else 0.0
    report = {
        "samples": len(rows),
        "fully_passed": passed,
        "fully_failed": len(rows) - passed,
        "pass_fraction": (passed / len(rows)) if rows else 0.0,
        "average_tokens_per_second": average_tps,
        "verdict": "COHERENT" if rows and passed == len(rows) else "MIXED_OR_POOR",
        "cases": scored,
    }
    rendered = json.dumps(report, indent=2, ensure_ascii=False)
    print(rendered)
    if args.report:
        args.report.write_text(rendered + "\n")
    print(
        f"CNET_COHERENCE_SCORE passed={passed}/{len(rows)} "
        f"average_tok_s={average_tps:.2f} verdict={report['verdict']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
