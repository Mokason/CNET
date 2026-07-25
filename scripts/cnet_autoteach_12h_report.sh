#!/usr/bin/env bash
set -euo pipefail
REPO=/home/marble/AI/CNET
cd "$REPO"
python3 - <<'PY'
import json, time, subprocess
from pathlib import Path
from datetime import datetime
repo = Path('/home/marble/AI/CNET')
base = repo / 'soul_gemma4v2_final.cnb'
b0 = json.loads((repo/'logs/autoteach/baseline_12h.json').read_text())
b = open(base,'rb').read() if base.exists() else b''
now = {
  'ts': datetime.now().astimezone().isoformat(timespec='seconds'),
  'elapsed_h': round((time.time()-b0['t_unix'])/3600, 2),
  'base_size': base.stat().st_size if base.exists() else 0,
  'fault_lines': sum(1 for _ in open(repo/'logs/cnet_faults.jsonl')) if (repo/'logs/cnet_faults.jsonl').exists() else 0,
  'lora_files': len(list((repo/'logs/lora_store').glob('*'))) if (repo/'logs/lora_store').exists() else 0,
  'hyb_struct': b.count(b'hyb_struct'),
  'units_proxy': b.count(b'acq_')+b.count(b'json_toolcall')+b.count(b'hyb_struct'),
  'services': {
    'bonsai': subprocess.getoutput('systemctl --user is-active bonsai-server'),
    'lane': subprocess.getoutput('systemctl --user is-active cnet-personal-ai-lane'),
    'autoteach_timer': subprocess.getoutput('systemctl --user is-active cnet-autoteach.timer'),
  },
  'last_tick': json.loads((repo/'logs/autoteach/last_tick.json').read_text()) if (repo/'logs/autoteach/last_tick.json').exists() else None,
}
delta = {
  'base_size_delta': now['base_size']-b0['base_size'],
  'fault_lines_delta': now['fault_lines']-b0['fault_lines'],
  'lora_files_delta': now['lora_files']-b0['lora_files'],
  'hyb_struct_delta': now['hyb_struct']-b0['hyb_struct'],
  'units_proxy_delta': now['units_proxy']-b0['units_proxy'],
}
rep = {'baseline': b0, 'now': now, 'delta': delta}
out = repo/'logs/autoteach/report_12h.json'
out.write_text(json.dumps(rep, indent=2)+'\n')
md = repo/'logs/autoteach/report_12h.md'
md.write_text(f"""# CNET auto-teach 12h report

- when: {now['ts']}
- elapsed_h: {now['elapsed_h']}

## Delta
- units_proxy: {b0['units_proxy']} → {now['units_proxy']} ({delta['units_proxy_delta']:+d})
- hyb_struct markers: {b0['hyb_struct']} → {now['hyb_struct']} ({delta['hyb_struct_delta']:+d})
- fault_lines: {b0['fault_lines']} → {now['fault_lines']} ({delta['fault_lines_delta']:+d})
- lora_files: {b0['lora_files']} → {now['lora_files']} ({delta['lora_files_delta']:+d})
- base_size: {b0['base_size']} → {now['base_size']} ({delta['base_size_delta']:+d} bytes)

## Services now
- bonsai: {now['services']['bonsai']}
- lane: {now['services']['lane']}
- autoteach_timer: {now['services']['autoteach_timer']}

## Last tick
```json
{json.dumps(now.get('last_tick'), indent=2)}
```
""")
print('WROTE', out)
print(json.dumps(delta, indent=2))
PY
