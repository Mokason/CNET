"""Measure the approved millisecond budget without rewriting historical 25us runs."""
import json
from pathlib import Path
import numpy as np
from verify_runtime import run,ROOT
OUT=ROOT/'var/passage_quality_ms_20260913'
def main():
    budget=json.loads(Path(__file__).with_name('answer_budget.json').read_text())
    questions=[r['q'] for r in json.loads((ROOT/'var/claude_scratch/answer_quality_q400_base.json').read_text())]
    _,times=run(ROOT/'bin/cnet_vsa_cli',questions*2,'budget_ms_baseline')
    def stats(t):
        total=t.sum(axis=1)
        return {f'p{p}_us':float(np.percentile(total,p)) for p in (50,95,99)}|{'max_us':float(total.max()),'mean_us':float(total.mean())}
    report={'budget':budget,'warm':stats(times[len(questions):]),'first_pass':stats(times[:len(questions)]),'queries':len(questions)}
    report['target_met']=report['warm']['p95_us']<=budget['target_p95_us']
    report['ceiling_met']=report['warm']['p99_us']<=budget['ceiling_p99_us']
    OUT.mkdir(exist_ok=True);(OUT/'baseline_budget.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
