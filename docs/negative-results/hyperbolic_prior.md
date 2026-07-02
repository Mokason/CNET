# Negative result — hyperbolic (Poincaré) rank prior

**Status:** built, measured, **rejected**. Code retained (`src/hyperbolic.c`,
`tests/test_hyperbolic.c`, `make test_hyperbolic`) as a quarantined artifact. **Not
wired into the router. No runtime authority.**

## The idea (and why it was tempting)

CNET's learned routing prior (`CircuitRankArtifact`) is keyed by an *exact* task
signature, so a never-seen task gets no prior. The proposal — inspired by the
"SONA" video's hyperbolic-geometry pitch — was to embed the capability/task
hierarchy in a metric space and let a new task borrow priors from its nearest
neighbors. Because that hierarchy is a *tree*, the theory says hyperbolic
(Poincaré-ball) geometry embeds it with far lower distortion than Euclidean at
low dimension (the cophenetic-distortion theorem). Elegant, well-cited, advanced.

## What was actually built

A correct, self-contained module:
- Poincaré distance + gradient, **verified against finite differences (1.3e-10)**.
- Nickel–Kiela neighborhood-ranking embedding (Riemannian SGD, burn-in).
- A geom switch (`HYP_GEOM_HYPERBOLIC` / `HYP_GEOM_EUCLIDEAN`) so the two can be
  compared on identical code.

## The measurement that killed it

Question: does hyperbolic's embedding advantage translate into better **routing-prior
generalization** (the actual use case) than a plain Euclidean kNN baseline?

Scaling sweep over complete binary trees, depth 4..9 (31..1023 nodes),
`scratchpad/scale_probe.c`, held-out prior recovery (mean abs error, lower=better):

| depth | N | prior-MAE k=1 hyp | k=1 **euc** | k=3 hyp | k=3 **euc** |
|------:|----:|---:|---:|---:|---:|
| 5 | 63 | 0.103 | **0.100** | 0.099 | **0.069** |
| 7 | 255 | 0.076 | **0.052** | 0.073 | **0.053** |
| 9 | 1023 | 0.070 | **0.053** | 0.070 | **0.053** |

**Euclidean kNN ties-or-beats hyperbolic at every depth tested, on every metric.**
Hyperbolic *does* embed the hierarchy more faithfully (adjacency / distance
correlation), but that fidelity **does not** translate into the downstream
scalar-prior-kNN task.

## The finding (one line)

> Hyperbolic geometry preserved hierarchy structure better, but did **not** improve
> the downstream CNET routing-prior task. Euclidean kNN was simpler and better.

The literature's hyperbolic wins are graph *reconstruction* / link prediction at
huge scale (WordNet, 80k+ nodes), not small-library scalar-prior regression.

## Why this file exists

To stop the idea from being rediscovered and re-implemented. It sounds advanced; it
measured worse than a one-line baseline at CNET's scale. If learned scoring is ever
pursued, a plain Euclidean embedding is the baseline to beat — and hyperbolic must
clear *this* bar (downstream prior recovery), not just the distortion theorem, before
earning a place. Retaining a falsified idea as audit history is the point.

See also: `~/.claude/projects/G--AI-CNET/memory/sona-ideas-topology-hyperbolic.md`
(the SHIPPED counterpart, the TDA contract-graph audit, is in `src/topology.c`).
