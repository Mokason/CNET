# Generate FIFO — chess-fetch text (not parrot CE)

## Idea

```
place CERT neurons (chess)
  → each step FETCH bound unit for state (no vocab sort)
  → run BTN
  → FIFO emit char
  → fail-closed abstain if no piece
```

Vs parrot baseline: **re-score every bound unit every step**.

## Run

```bash
cd /home/marble/AI/CNET && make generate_fifo
# GENERATE_FIFO_PASS
# logs/generate_fifo.log
```

## Measured (200 reps × 48 steps, 17 neurons)

| path | wall | btn_forwards | emit |
|---|---:|---:|---:|
| **chess-fetch + FIFO** | **0.0018s** | **9600** | 9600 |
| re-sort all units/step | 0.0322s | 163200 | 9600 |

- **~18× wall speedup**
- **~17× fewer forwards** (matches alphabet size)
- no pieces → abstain (not fill)

## Honest text note

Per-symbol Markov pieces (last teacher transition wins) → chains can **collapse** to a repeating letter. That is neuron locality, not LLM prose. Richer text needs richer state (bigram/position pieces), still fetch-not-sort.

## Long-run length probe

```bash
make generate_fifo_long
# logs/generate_fifo_long.log
```

| steps | len full? | collapse (run≥8) | unique chars | chess wall |
|---|---|---|---:|---:|
| 48 | yes | index **1** (`l`) | 2 | ~0.00s |
| 512 | yes | index **1** | 2 | ~0.0001s |
| 4096 | yes | index **1** | 2 | ~0.0007s |

Length scales with `n_steps`. Content collapses immediately (per-char Markov).

## Quality lever: position board (not char-Markov)

```bash
make generate_fifo_quality
# GENERATE_FIFO_QUALITY_PASS
```

| mode | state | match_teacher | unique | collapse | long ask=4096 |
|---|---|---:|---:|---|---|
| Markov | char id | **0.057** | 2 | index 1 `l` | more `l`s |
| **Position** | board square | **1.000** | 17 | none | **stops at 53**, abstain |

```
out    = ello_cnet_chess_fifo_neurons_generate_text_not_parrot
expect = ello_cnet_chess_fifo_neurons_generate_text_not_parrot
```

API:
```c
cnet_gen_engine_set_dims(e, n_positions, alphabet);
cnet_gen_engine_set_advance(e, CNET_GEN_ADVANCE_POSITION, n_positions);
// bind square i -> CERT unit pos_i : onehot(i) -> onehot(char)
// fetch square, emit, state++
```

Still **not a parrot**: pieces placed on the board; generate = fetch+run+FIFO.

## Skill capsules E2E (portable specialized skills)

```bash
make skill_capsule_generate
# SKILL_CAPSULE_GENERATE_PASS
# artifacts/skill_pos_capsules/skill_pos_XX/{unit.cnb,manifest.cknow}
```

Loop:
```
teacher → seal skill_pos_* (CERT+coverage) → export CNU1 capsules
      → fresh import → cnb_load_registry → generate_fifo position
```

### Bench (53 skills, this host)

| path | wall_s |
|---|---:|
| seal+train+admit | **1.86** |
| capsule export | **0.037** |
| capsule import | **0.005** |
| generate (mem) | **0.0001** |
| generate (capsule import) | **0.0001** |
| export+import+gen | **0.042** |

Quality: **mem = capsule = exact teacher path**; long ask=4096 stops at board + abstain.

## API

`include/cnet_generate_fifo.h` · `src/cnet_generate_fifo.c`
