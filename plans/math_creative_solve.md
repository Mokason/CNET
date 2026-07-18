# Creative math solve — high-school algebra level

## Approach

Numbers only from `math_eval` (verified plans). Creativity = pick/try templates;
never invent answers.

```
question → collect plans (Tier A templates + Tier B alts + direct expr)
         → execute steps with math_eval
         → first verified plan wins
         → memorize + procedural SKILL.md
```

## Coverage (HS algebra band)

| Family | Examples |
|--------|----------|
| Arithmetic / powers / roots | `2^10+3^5`, nested expr, NL “what is …” |
| Linear | `3x+7=22`, `ax+b=c` |
| Systems 2×2 | `2x+y=5 and x-y=1` |
| Quadratic | monic + non-monic `2x^2-8x+6=0` |
| Pythagorean | legs → hypotenuse |
| Primes | is N prime |
| GCD / LCM / factorial / Fibonacci | |
| Percent, distance, midpoint, slope | |
| 2×2 determinant | |
| asin degrees | sin θ = 0.5 → 30° |
| Bayes numeric | three probabilities |

## Not yet (honest abstain)

- Calculus (integral, derivative, limits)
- Cubics / higher polynomials
- Full CAS / symbolic simplify
- Open word problems without a template

## Gates / tools

```bash
make math_solve          # MATH_SOLVE_PASS
bin/cnet_math_solve "…"
bin/cnet_learn_cycle "…" # routes math first
```

Sources: `src/contract/mcp_math_eval.c`, `src/cnet_math_solve.c`
