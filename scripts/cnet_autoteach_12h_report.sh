#!/usr/bin/env bash
# 12h progress report for the 24/7 auto-teach loop.
#
# Every metric here used to be a byte-marker proxy: units_proxy counted
# b"acq_"/b"json_toolcall"/b"hyb_struct" substrings in the base and read 355 on
# both sides of a window in which the real unit count moved 102 -> 120, so the
# report announced "zero progress" during an hour of genuine learning. It also
# counted raw fault_lines as activity when those lines were duplicate synthetic
# seed records.
#
# Now: authoritative unit count from cnb_audit, gap states from the ledger,
# DISTINCT fault pairs rather than lines, and closure totals from the lane's own
# hill-climb ledger.
set -euo pipefail
REPO=/home/marble/AI/CNET
cd "$REPO"
python3 - <<'PY'
import json, re, subprocess, time
from pathlib import Path
from datetime import datetime

repo = Path('/home/marble/AI/CNET')
base = repo / 'soul_gemma4v2_final.cnb'
gov = repo / 'logs/autoteach'
gov.mkdir(parents=True, exist_ok=True)


def units() -> int:
    audit = repo / 'bin/cnb_audit'
    if audit.exists() and base.exists():
        try:
            out = subprocess.run([str(audit), str(base), '--count'],
                                 capture_output=True, text=True, timeout=120).stdout
            m = re.search(r'units=(\d+)', out)
            if m:
                return int(m.group(1))
        except Exception:
            pass
    return 0


def gap_states() -> dict:
    """Partition the ledger: every row lands in exactly one bucket.

    waiting_oracle is an ANNOTATION on a row, not a third state — the state
    lives in field 1 (1=open, 2=closed) and an annotated row still carries it.
    Without the `else` this reader counted such a row twice, once as waiting
    and again as open/closed, so the totals exceeded the row count: on an
    819-row ledger it read open=23 waiting=10 closed=796 (sum 829) where the
    governor's partitioning reader read open=14 deferred=10 closed=795. The
    9 rows that were both state=1 and waiting inflated outstanding work by 38%
    and made the report and the scoreboard contradict each other.

    Same bug, same fix, as governor_miss_ingest.sh — see
    tests/test_metric_honesty.py::TestParsersAgree, which pins all three
    readers to governor_autonomous.collect() as the reference.
    """
    p = Path(str(base) + '.gaps.txt')
    st = {'open': 0, 'closed': 0, 'waiting': 0}
    if not p.exists():
        return st
    for i, line in enumerate(p.read_text(errors='replace').splitlines()):
        if i < 2:
            continue
        if 'waiting_oracle' in line or 'waiting_charter' in line:
            st['waiting'] += 1
        else:
            parts = line.split()
            if len(parts) > 1 and parts[1] == '1':
                st['open'] += 1
            elif len(parts) > 1 and parts[1] == '2':
                st['closed'] += 1
    return st


def faults() -> dict:
    p = repo / 'logs/cnet_faults.jsonl'
    if not p.exists():
        return {'lines': 0, 'distinct': 0, 'units': 0}
    pairs, us, n = set(), set(), 0
    for line in p.open(errors='replace'):
        line = line.strip()
        if not line:
            continue
        n += 1
        try:
            d = json.loads(line)
        except Exception:
            continue
        us.add(d.get('unit'))
        pairs.add((d.get('unit'), tuple(d.get('in') or []), tuple(d.get('tgt') or [])))
    return {'lines': n, 'distinct': len(pairs), 'units': len(us)}


def lane_totals(since_unix: float) -> dict:
    p = Path(str(base) + '.hill_climb.jsonl')
    tot = {'examined': 0, 'closed': 0, 'deferred': 0, 'no_oracle': 0, 'ticks': 0}
    if not p.exists():
        return tot
    for line in p.open(errors='replace'):
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except Exception:
            continue
        if float(r.get('ts') or 0) < since_unix:
            continue
        tot['ticks'] += 1
        for k in ('examined', 'closed', 'deferred', 'no_oracle'):
            tot[k] += int(r.get(k) or 0)
    return tot


def moe() -> dict:
    """Primary learning substrate: held-out CE against an analytic floor."""
    p = repo / 'artifacts/moe/state.json'
    if not p.exists():
        return {}
    try:
        d = json.loads(p.read_text())
    except Exception:
        return {}
    return {k: d.get(k) for k in
            ('step', 'heldout_ce', 'best_ce', 'h1', 'h2', 'gap_to_floor',
             'below_h1', 'certified', 'eval_n', 'device')}


def snapshot() -> dict:
    f = faults()
    return {
        'moe': moe(),
        'ts': datetime.now().astimezone().isoformat(timespec='seconds'),
        't_unix': time.time(),
        'units': units(),
        'gaps': gap_states(),
        'fault_lines': f['lines'],
        'fault_distinct': f['distinct'],
        'lora_files': len(list((repo / 'logs/lora_store').glob('*')))
        if (repo / 'logs/lora_store').is_dir() else 0,
        'base_size': base.stat().st_size if base.exists() else 0,
        'services': {
            'bonsai': subprocess.getoutput('systemctl --user is-active bonsai-server'),
            'lane': subprocess.getoutput('systemctl --user is-active cnet-personal-ai-lane'),
            'autoteach_timer': subprocess.getoutput('systemctl --user is-active cnet-autoteach.timer'),
            'governor_timer': subprocess.getoutput('systemctl --user is-active cnet-governor.timer'),
        },
        'schema': 3,
    }


now = snapshot()
bp = gov / 'baseline_12h.json'
b0 = json.loads(bp.read_text()) if bp.exists() else {}
if b0.get('schema') != 3:
    # schema 1 used byte-marker proxies; schema 2 double-counted waiting rows
    # as open. Neither is comparable to a partitioned count, and differencing
    # across the change would print a phantom gaps_open drop that no lane tick
    # earned. Re-baseline rather than print a fabricated delta.
    b0 = dict(now)
    b0['horizon_h'] = 12
    b0['due_unix'] = now['t_unix'] + 12 * 3600
    b0['rebaselined_from_proxy_schema'] = True
    bp.write_text(json.dumps(b0, indent=2) + '\n')

elapsed_h = round((now['t_unix'] - float(b0['t_unix'])) / 3600, 2)
lane = lane_totals(float(b0['t_unix']))
delta = {
    'units': now['units'] - b0['units'],
    'gaps_closed': now['gaps']['closed'] - b0['gaps']['closed'],
    'gaps_open': now['gaps']['open'] - b0['gaps']['open'],
    'gaps_waiting': now['gaps']['waiting'] - b0['gaps']['waiting'],
    'fault_lines': now['fault_lines'] - b0['fault_lines'],
    'fault_distinct': now['fault_distinct'] - b0['fault_distinct'],
    'lora_files': now['lora_files'] - b0['lora_files'],
    'base_size': now['base_size'] - b0['base_size'],
}
units_per_h = round(delta['units'] / elapsed_h, 2) if elapsed_h > 0 else 0.0

rep = {'baseline': b0, 'now': now, 'delta': delta,
       'elapsed_h': elapsed_h, 'units_per_h': units_per_h, 'lane_since_baseline': lane}
(gov / 'report_12h.json').write_text(json.dumps(rep, indent=2) + '\n')

close_rate = (lane['closed'] / lane['examined']) if lane['examined'] else 0.0

m0, m1 = b0.get('moe') or {}, now.get('moe') or {}
if m1:
    def _d(k):
        a, b = m0.get(k), m1.get(k)
        return f"{a} → {b}" if a is not None else str(b)
    moe_md = f"""## Learning substrate: transformer-MoE vs analytic entropy floor

The metric that cannot be gamed by memorisation — the markov2 stream is sampled
fresh every sequence and H2 is computed from the source, so CE falls only on
genuine generalisation. Certified means held-out CE below H1, the order-1
plateau, which requires the attention to use the previous token.

- step: {_d('step')}
- held-out CE: {_d('heldout_ce')}   (uniform ~4.159)
- floors: H1 {m1.get('h1')} (order-1 plateau) → H2 {m1.get('h2')} (true floor)
- gap to floor: {_d('gap_to_floor')}
- below H1 (certified): {m1.get('below_h1')} / {m1.get('certified')}
- eval_n {m1.get('eval_n')} on {m1.get('device')}
"""
else:
    moe_md = "## Learning substrate\n\n- no MoE state yet (run `make moe_tick`)\n"

(gov / 'report_12h.md').write_text(f"""# CNET auto-teach 12h report

- when: {now['ts']}
- elapsed_h: {elapsed_h}

{moe_md}
## Lookup coverage (gap lane — secondary)
- units: {b0['units']} → {now['units']} ({delta['units']:+d})  [{units_per_h}/h]
- gaps closed: {b0['gaps']['closed']} → {now['gaps']['closed']} ({delta['gaps_closed']:+d})
- gaps open: {b0['gaps']['open']} → {now['gaps']['open']} ({delta['gaps_open']:+d})
- gaps waiting oracle: {b0['gaps']['waiting']} → {now['gaps']['waiting']} ({delta['gaps_waiting']:+d})

## Lane activity since baseline
- ticks {lane['ticks']}, examined {lane['examined']}, closed {lane['closed']}, \
deferred {lane['deferred']}, no_oracle {lane['no_oracle']}
- close rate: {close_rate:.0%}

## Fault bus
- lines: {b0['fault_lines']} → {now['fault_lines']} ({delta['fault_lines']:+d})
- DISTINCT (unit,in,tgt): {b0['fault_distinct']} → {now['fault_distinct']} ({delta['fault_distinct']:+d})
- lora adapters: {b0['lora_files']} → {now['lora_files']} ({delta['lora_files']:+d})

Line growth without distinct growth means duplicate records, not new signal.

## Services now
- bonsai: {now['services']['bonsai']}
- lane: {now['services']['lane']}
- autoteach_timer: {now['services']['autoteach_timer']}
- governor_timer: {now['services']['governor_timer']}
""")
print('WROTE', gov / 'report_12h.json')
print(json.dumps({'elapsed_h': elapsed_h, 'units_per_h': units_per_h,
                  'delta': delta, 'lane': lane}, indent=2))
PY
