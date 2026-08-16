/* test_json_escape.c -- the one JSON escaper must produce parseable JSON.
 *
 * make json_escape -> JSON_ESCAPE_PASS
 *
 * Three escapers had diverged (see include/cnet_json_escape.h). The two broken
 * ones failed on exactly the inputs nobody hand-tests: a raw C0 byte, and a
 * string ending in a backslash. Both produce a document that a strict parser
 * rejects, and both were reachable from model output.
 *
 * Every case here asserts the ESCAPED FORM, then tests/test_json_escape.py
 * feeds the emitted document to a real json.loads so the check is not just
 * "our escaper agrees with our expectations".
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_json_escape.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-56s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void check_esc(const char *in, const char *want, const char *name) {
    char got[512];
    int rc = cnet_json_escape(in, got, sizeof got);
    int ok = (rc == 0) && strcmp(got, want) == 0;
    checks++;
    printf("  %-56s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        printf("      want [%s]\n      got  [%s] rc=%d\n", want, got, rc);
        failures++;
    }
}

int main(void) {
    printf("== cnet_json_escape ==\n");

    check_esc("", "", "empty string");
    check_esc("plain", "plain", "no escaping needed");
    check_esc("say \"hi\"", "say \\\"hi\\\"", "double quote");

    /* A string ending in a backslash. Emitted raw this closes as  "...\"  and
       swallows the closing quote, corrupting every field after it. */
    check_esc("ends with\\", "ends with\\\\", "trailing backslash");
    check_esc("a\\b", "a\\\\b", "embedded backslash");

    check_esc("line1\nline2", "line1\\nline2", "newline -> \\n");
    check_esc("a\rb", "a\\rb", "carriage return -> \\r");
    check_esc("a\tb", "a\\tb", "tab -> \\t");

    /* THE REGRESSION. cnetd passed C0 bytes through verbatim (invalid JSON per
       RFC 8259) and cnet_fault silently DELETED them (data loss). Neither is
       acceptable: the byte must survive, as \u00XX. */
    check_esc("a\x01" "b", "a\\u0001b", "raw 0x01 -> \\u0001 (not dropped, not raw)");
    check_esc("\x1f", "\\u001f", "0x1f -> \\u001f");
    check_esc("a\x0b" "b", "a\\u000bb", "vertical tab -> \\u000b");

    /* 0x7f is NOT a C0 control and needs no escape under RFC 8259. */
    check_esc("a\x7f" "b", "a\x7f" "b", "0x7f passes through (not a C0 control)");

    /* High bytes are UTF-8 payload, passed through untouched. */
    check_esc("caf\xc3\xa9", "caf\xc3\xa9", "UTF-8 bytes pass through");

    /* Truncation must be SIGNALLED and must not leave a half-written escape:
       a dst holding `\` alone would corrupt the document. */
    {
        char small[4];
        int rc = cnet_json_escape("aaaa", small, sizeof small);
        check(rc == -1 && small[0] == '\0',
              "overflow returns -1 and empties dst (no partial escape)");
    }
    {
        char tiny[2];
        /* One quote needs 2 bytes + NUL = 3 > 2, so this must refuse. */
        int rc = cnet_json_escape("\"", tiny, sizeof tiny);
        check(rc == -1 && tiny[0] == '\0', "escape that cannot fit is refused");
    }
    {
        char exact[3];
        /* One quote needs exactly 2 bytes + NUL. */
        int rc = cnet_json_escape("\"", exact, sizeof exact);
        check(rc == 0 && strcmp(exact, "\\\"") == 0, "exact fit succeeds");
    }

    check(cnet_json_escape(NULL, (char[8]){0}, 8) == 0, "NULL src treated as empty");
    check(cnet_json_escape("x", NULL, 8) == -1, "NULL dst refused");
    { char d[4]; check(cnet_json_escape("x", d, 0) == -1, "zero cap refused"); }

    /* Emit a document containing every hostile input at once, for the Python
       side to parse. stdout is the artifact. */
    {
        char a[512], b[512], c[512];
        (void)cnet_json_escape("quote\" back\\ slash", a, sizeof a);
        (void)cnet_json_escape("ctrl\x01\x02 end\\", b, sizeof b);
        (void)cnet_json_escape("nl\ntab\tcr\r", c, sizeof c);
        printf("JSON_ESCAPE_DOC {\"a\":\"%s\",\"b\":\"%s\",\"c\":\"%s\"}\n", a, b, c);
    }

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) printf("JSON_ESCAPE_PASS checks=%d\n", checks);
    return failures ? 1 : 0;
}
