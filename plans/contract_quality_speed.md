# Contract Quality and Speed Optimization

## Place
Candidate promotion at `contract_better_if()`, between two implementations that both satisfy the same behavior contract.

## Dilemma
The documented raw-output MSE comparison canonicalizes outputs before scoring. Since certified outputs already canonicalize exactly to the targets, both losses are always zero. The function therefore cannot distinguish a fragile 0.76 output from a robust 0.99 output and performs two redundant full forward sweeps.

## Consequence
Quality selection falls through to unrelated historical reliability, while promotion costs twice the necessary inference work. Systematic component: certified minimum margin. Irreducible noise: runtime timing; acceptance uses exact forward-call counts rather than a flaky wall-clock threshold.

## Tracer
RED: two equally reliable certified adapters with different margins must select the stronger candidate; each model must be evaluated once.
GREEN: compare `CertifyReport.min_margin` from the existing certification replay and remove redundant canonicalized-MSE sweeps.
