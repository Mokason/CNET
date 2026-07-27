# Gap-lane plateau: root cause + bounded unblock (2026-07-27)

**Repo:** `/home/marble/AI/CNET` @ `441ab7e`
**Scope:** bounded diagnosis + fail-open fixes. No MoE campaign, no window redesign.

---

## TL;DR

The lane is **not stuck — it is finished**, and the metrics lied about it.

There are **zero** actually-open gaps. The governor was reading `status == 1` as OPEN, but
`GAP_OPEN = 0` and `GAP_DEFERRED = 1` (`include/acquire.h:212`). So 14 *terminally parked*
gaps were reported as drainable backlog, `backlog_pressure` read 24 against a target of 8,
and the governor chased `drain_open_gaps` for 20 straight cycles against work no drain can
touch. Units flat at 350 is **correct behaviour** for an exhausted teachable space, not a
failure.

---

## Root cause

### 1. Zero open gaps; every remaining gap is unservable

```
$ awk 'NR>2 && $2==0' soul_gemma4v2_final.cnb.gaps.txt | wc -l     # true GAP_OPEN
0
$ grep -c waiting_oracle soul_gemma4v2_final.cnb.gaps.txt          # revivable
10
$ awk 'NR>2 && $2==1' ...gaps.txt | grep -vc waiting_oracle        # terminally parked
14
```

Ledger field order is `kind status times_hit attempts in.family in.width in.count in.tag
goal.family goal.width goal.count goal.tag subject oracle defer_reason unit ...`
(`src/acquire.c:1744`). The 23 deferred gaps break down as:

| defer_reason | n | revivable? |
|---|---|---|
| `waiting_oracle` | 9 | yes — when a matching teacher binds |
| `tag_collision` | 7 | no |
| `unbounded_domain` | 4 | no |
| `certify_failed` | 2 | no |
| `register_refused` | 1 | no |

### 2. The 9 revivable gaps are the wrong shape for the only teacher

All 9 are **16-wide** legacy shapes:

```
in=16x1(grow_in)  goal=16x1(grow_tok_0..7)   waiting_oracle
in=16x1(res_tok)  goal=16x1(res_next)        waiting_oracle
```

The live HTTP teacher's window is **256** (`english_window_256_bonsai.txt`, 256 lines).
`http_shape_ok()` (`tests/gap_lane_run.c:425-436`) requires `(int)in.field_width == W`, so
none match → `bind_http_teachers` returns 0.

`acquire_drain` revives a parked NO_PLAN gap **only** when a matching oracle exists
(`src/acquire.c:1638-1644`):

```c
if (gap_waiting_oracle(g) && find_oracle(oracles, g->input_port, g->goal_port)) {
    g->status = GAP_OPEN;
```

No match → status stays DEFERRED → the drain loop `continue`s before `report->examined++`
→ the tick reports **`drained=0 closed=0 deferred=0 no_oracle=0`**. Four zeros that read as
"no work" but mean "work no teacher here can take". That exact signature is why `no_oracle`
was also 0 — the drain never examined them at all.

### 3. No new teachable work is being generated

`curiosity proposed=3 skipped_covered=99` every tick, and
`INJECT_WINDOW_EXHAUSTED ... covered=745`. The 256-token window's teachable space is
essentially covered (824 gaps closed). Curiosity re-proposes covered items, which dedupe.
So nothing new becomes OPEN, and units stay 350.

---

## Changes

| File | Change |
|---|---|
| `scripts/governor_autonomous.py:433-458` | Count `status` 0/1/2 apart instead of scoring DEFERRED as OPEN. New `parked_gaps` bucket for terminal reasons. |
| `scripts/governor_autonomous.py` (scoreboard) | Emit `parked_gaps` + `parked_pressure`; `backlog_pressure` is now **actionable** work only (open + revivable), not parked. |
| `tests/gap_lane_run.c` (tick loop) | Path-independent diagnostic: every 10 ticks, if there are retry candidates but none teachable by the live teacher, say so with the window/vocab that failed to match. |
| `tests/gap_lane_run.c:462-489` | `bind_http_teachers` counts and reports shape vs record-owned rejects when it binds nothing. |

No behavioural change to drain, certify or seal logic. Nothing was deleted from the ledger.

---

## Verification

**Governor accounting, before → after (same ledger):**

```
OLD: open_gaps=14 deferred=10 backlog_pressure=24
NEW: open_gaps=0  deferred_oracle=9  parked_gaps=14  backlog_pressure=9  parked_pressure=14
```

**Live scoreboard after the fix (`logs/governor/scoreboard.json`, cycle 160):**

```
cycle=160 open_gaps=0 deferred_oracle=9 parked_gaps=14 closed_gaps=824
backlog_pressure=9.0 parked_pressure=14.0 units_proxy=350 plateau=1
```

**Live lane, first tick after rebuild+restart** — the blockage now names itself:

```
Jul 27 07:51:22 gap_lane_run: HTTP teacher http://127.0.0.1:8080 window=256
Jul 27 07:51:22 gap_lane_run: retry_candidates=9 teachable=0 (shape unmatched by live
                teacher; lm_vocab=0 http_window=256) — drain will stay at 0
```

Reproduce:

```bash
systemctl --user restart cnet-personal-ai-lane.service
journalctl --user -u cnet-personal-ai-lane.service -n 20 --no-pager | grep retry_candidates
systemctl --user start cnet-governor.service
python3 -c "import json;print(json.load(open('logs/governor/scoreboard.json')))"
```

---

## Honest status of the done-criteria

- **closed > 0 or units > 350: NO, and that is correct.** Nothing is closable. The teachable
  space of the 256 window is exhausted (824 closed, `covered=745`), the 9 revivable gaps have
  no possible teacher, and the other 14 are parked on terminal reasons. Manufacturing a close
  here would mean lowering a certification bar.
- **backlog_pressure decreased: YES, 24 → 9**, and it is now true rather than an artefact of
  a status misread. `open_gaps` went 14 → 0, which is the actual state of the ledger.
- The governor logged `skip_evolve_small_dt` on the manual cycle (triggered too soon after a
  scheduled one), so goal re-ranking against the new pressure lands on the next natural tick.

---

## Residual backlog (not done, deliberately)

1. **Retire or re-shape the 9 legacy 16-wide gaps.** They will wait forever; no 16-wide
   teacher exists or is planned. This mutates the live ledger, so it is an operator decision,
   not mine. Inspect first:
   ```bash
   awk '$2==1 && /waiting_oracle/ {print}' soul_gemma4v2_final.cnb.gaps.txt
   ```
2. **`backlog_pressure=9` still exceeds the target of 8** purely because of those 9. Retiring
   them drops it to 0 and lets `drain_open_gaps` finally retire as a goal.
3. **The real lever for unit growth is new teachable material**, not draining: a wider/second
   window, or new skill families. That is a campaign, explicitly out of scope here.
4. The 14 terminally-parked gaps (`tag_collision`, `unbounded_domain`, `certify_failed`,
   `register_refused`) are now visible as `parked_gaps` — worth a separate look at whether
   `tag_collision` (7 of them, all `grow_in→grow_tok_*` at 256 wide) is a fixable naming
   clash rather than a dead end. Cheapest next real win if unit growth is wanted.
