#!/usr/bin/env python3
"""Feed the C escaper's output to a REAL JSON parser.

tests/test_json_escape.c asserts that cnet_json_escape produces the byte
sequences we expect. That is necessary but not sufficient: it only proves the
escaper agrees with our own expectations. This script parses the emitted
document with json.loads and checks the values come back byte-for-byte, which
is the property that actually matters -- the two broken escapers this replaces
produced output that looked plausible and that no strict parser would accept.

Usage: python tests/test_json_escape.py ./bin/test_json_escape
Exit 0 = the emitted document is valid JSON and round-trips exactly.
"""
import json
import subprocess
import sys


def main() -> int:
    exe = sys.argv[1] if len(sys.argv) > 1 else "./bin/test_json_escape"
    try:
        out = subprocess.run([exe], stdout=subprocess.PIPE, check=False).stdout
    except OSError as e:
        print("FAIL: cannot run %s: %s" % (exe, e))
        return 1

    text = out.decode("utf-8", "replace")
    line = None
    for candidate in text.splitlines():
        if candidate.startswith("JSON_ESCAPE_DOC "):
            line = candidate[len("JSON_ESCAPE_DOC "):]
            break
    if line is None:
        print("FAIL: no JSON_ESCAPE_DOC line in output")
        return 1

    try:
        d = json.loads(line)
    except ValueError as e:
        print("FAIL: emitted document is not valid JSON: %s" % e)
        print("  doc: %s" % line)
        return 1

    fails = 0

    def check(ok, name, got=None):
        nonlocal fails
        print("  %-56s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            if got is not None:
                print("      got %r" % (got,))
            fails += 1

    check(d["a"] == 'quote" back\\ slash', "quote+backslash round-trips", d["a"])

    # The regression that mattered: cnetd emitted these raw (invalid JSON) and
    # cnet_fault deleted them outright (silent data loss). They must SURVIVE.
    check("\x01" in d["b"], "0x01 survived the round trip", d["b"])
    check("\x02" in d["b"], "0x02 survived the round trip", d["b"])
    check(d["b"].endswith("\\"), "trailing backslash intact", d["b"])
    check(d["b"] == "ctrl\x01\x02 end\\", "field b exact", d["b"])

    check(d["c"] == "nl\ntab\tcr\r", "newline/tab/CR exact", d["c"])

    if fails:
        print("JSON_ESCAPE_PY_FAIL failures=%d" % fails)
        return 1
    print("JSON_ESCAPE_PY_PASS: emitted document parsed by json.loads and "
          "round-tripped exactly")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
