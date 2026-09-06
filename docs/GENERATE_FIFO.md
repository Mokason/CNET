# Bound-unit generation and FIFO output

The [generation API](../include/cnet_generate_fifo.h) binds a state to a
certified unit, executes that unit and queues its output. A missing binding or
coverage miss abstains. The implementation is
[cnet_generate_fifo.c](../src/serve/cnet_generate_fifo.c).

Two state modes have different limits:

| Mode | State update | What it can establish |
| --- | --- | --- |
| Markov | Previous output selects the next state | Finite transition behavior; can collapse into repetitions |
| Position | Increment the position | Exact replay of a supplied finite sequence; stops at the board limit |

The public engine has at most 256 bindings and a default FIFO capacity of
4096. These limits are not the capsule-core inventory limit.

```sh
make generate_fifo generate_fifo_long generate_fifo_quality
make skill_capsule_generate
```

The comparison baseline re-scores all bound units each step. Report forwards,
emitted length, repetition and coverage refusal alongside elapsed time.
Position-mode agreement of 1.000 names a supplied sequence replay, not
unseen prose generation. Portable replay additionally needs capsule
export/import verification.

Earlier host-specific timings and the recorded Markov collapse are retained in
the [documentation archive](MAINTENANCE.md); they are not fresh benchmarks.
