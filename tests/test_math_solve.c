/* High-school algebra math solve gate.
 * make math_solve → MATH_SOLVE_PASS
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../include/cnet_math_solve.h"
#include "../include/contract/mcp_math_eval.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int solve_has(const char *q, const char *needle) {
    CnetMathSolveConfig c;
    CnetMathSolveReport r;
    cnet_math_solve_config_defaults(&c);
    snprintf(c.skills_dir, sizeof c.skills_dir, "logs/test_math_skills");
    c.write_skill = 0;
    if (cnet_math_solve(q, &c, &r) != 0 || !r.verified) return 0;
    return strstr(r.answer, needle) != NULL;
}

static int solve_has2(const char *q, const char *n1, const char *n2) {
    return solve_has(q, n1) && solve_has(q, n2);
}

int main(void) {
    double v = 0;

    printf("== math_solve high-school algebra ==\n");

    /* Eval foundation */
    check(cnet_math_eval_expr("2+3*4", &v) == 0 && fabs(v - 14) < 1e-9, "eval precedence");
    check(cnet_math_eval_expr("2^10+3^5", &v) == 0 && fabs(v - 1267) < 1e-6, "eval powers sum");
    check(cnet_math_eval_expr("gcd(252,105)", &v) == 0 && fabs(v - 21) < 1e-9, "eval gcd");
    check(cnet_math_eval_expr("lcm(12,18)", &v) == 0 && fabs(v - 36) < 1e-9, "eval lcm");
    check(cnet_math_eval_expr("fact(5)", &v) == 0 && fabs(v - 120) < 1e-9, "eval fact");
    check(cnet_math_eval_expr("5!", &v) == 0 && fabs(v - 120) < 1e-9, "eval postfix !");
    check(cnet_math_eval_expr("asind(0.5)", &v) == 0 && fabs(v - 30) < 1e-6, "eval asind");

    /* Classic */
    check(solve_has("legs 3 and 4 right triangle hypotenuse", "5"), "pythag");
    check(solve_has("Is 17 a prime number?", "yes"), "prime");
    check(solve_has("x^2 - 5x + 6 = 0", "2") && solve_has("x^2 - 5x + 6 = 0", "3"),
          "quadratic monic");
    check(solve_has("2x^2 - 8x + 6 = 0", "1") && solve_has("2x^2 - 8x + 6 = 0", "3"),
          "quadratic non-monic");
    check(solve_has("what is 2^10 + 3^5", "1267"), "NL powers");
    check(solve_has("what is 23 * 19", "437"), "NL multiply");

    /* Linear & systems */
    check(solve_has("3x+5=20", "5"), "linear 3x+5=20");
    check(solve_has("2x+y=5 and x-y=1", "2") && solve_has("2x+y=5 and x-y=1", "1"),
          "system 2x+y=5, x-y=1");

    /* Number theory / discrete */
    check(solve_has("gcd of 252 and 105", "21"), "gcd");
    check(solve_has("lcm of 12 and 18", "36"), "lcm");
    check(solve_has("What is 15! ?", "1307674368000") || solve_has("What is 10!", "3628800"),
          "factorial");
    check(solve_has("factorial 6", "720"), "fact 6");
    check(solve_has("Fibonacci 10", "55"), "fib 10");

    /* Geometry / algebra extras */
    check(solve_has("15% of 80", "12"), "percent");
    check(solve_has("distance between (0,0) and (3,4)", "5"), "distance");
    check(solve_has2("midpoint of (0,0) and (4,6)", "2", "3"), "midpoint");
    check(solve_has("slope of (1,2) and (3,8)", "3"), "slope");
    check(solve_has("det of [[1,2],[3,4]]", "-2") || solve_has("determinant 1 2 3 4", "-2"),
          "det2");
    check(solve_has("sin theta = 0.5 acute angle in degrees", "30"), "asind");
    check(solve_has("Bayes P(D)=0.01 P(+|D)=0.9 P(+|~D)=0.1", "0.083") ||
              solve_has("Bayes P(D)=0.01 P(+|D)=0.9 P(+|~D)=0.1", "0.08"),
          "bayes numeric");

    printf("MATH_SOLVE_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
