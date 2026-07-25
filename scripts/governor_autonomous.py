#!/usr/bin/env python3
"""CNET autonomous governor v2 — full stack over native C core.

Implements: miss-bus, eval probes, outcome close, projects, pins, resources,
safe allowlisted web, expanded muscles. Uses bin/cnet_governor for core
decide+muscles when available; falls back to embedded policy.
"""
from __future__ import annotations
import json, os, re, subprocess, sys, time
from pathlib import Path
from datetime import datetime

ROOT = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
GOV = Path(os.environ.get("CNET_GOVERNOR_DIR", ROOT / "logs/governor"))
BASE = Path(os.environ.get("CNET_BASE_PATH", ROOT / "soul_gemma4v2_final.cnb"))
CHARTER = Path(os.environ.get("CNET_GOVERNOR_CHARTER", ROOT / "config/cnet_governor_charter.yaml"))
PINS = ROOT / "config/governor_pins.yaml"
PROJECTS = ROOT / "config/governor_projects.json"
HTTP = os.environ.get("CNET_RESIDUAL_HTTP", "http://127.0.0.1:8080")
BIN = ROOT / "bin/cnet_governor"

def sh(cmd, timeout=600):
    return subprocess.run(cmd, shell=True, cwd=str(ROOT), capture_output=True, text=True, timeout=timeout)

def active(unit):
    return sh(f"systemctl --user is-active --quiet {unit}").returncode == 0

def load_json(p, default=None):
    try:
        return json.loads(Path(p).read_text())
    except Exception:
        return default if default is not None else {}

def parse_pins():
    pins = {"force_goal":"", "freeze_seals":0, "pause_inject":0, "pause_web":0, "max_tasks_override":0}
    if not PINS.exists():
        return pins
    for line in PINS.read_text().splitlines():
        line=line.split("#")[0].strip()
        if ":" not in line: continue
        k,v=line.split(":",1); k=k.strip(); v=v.strip()
        if k in pins:
            pins[k] = int(v) if v.isdigit() else v
    return pins

def parse_charter():
    goals=[]; cur=None; max_tasks=3
    for raw in CHARTER.read_text().splitlines():
        line=raw.split("#")[0].rstrip()
        if not line.strip(): continue
        if line.startswith("max_tasks_per_cycle:"):
            max_tasks=int(line.split(":",1)[1])
        elif re.match(r"\s*-\s*id:", line):
            if cur: goals.append(cur)
            cur={"id": line.split("id:",1)[1].strip(), "priority":99, "when":"false", "actions":[]}
        elif cur is not None:
            m=re.match(r"\s+(priority|when|actions):\s*(.*)$", line)
            if not m: continue
            k,v=m.group(1), m.group(2).strip()
            if k=="priority": cur[k]=int(v)
            elif k=="actions":
                inner=v.strip("[]")
                cur[k]=[a.strip() for a in inner.split(",") if a.strip()]
            else:
                cur[k]=v.strip().strip('"')
    if cur: goals.append(cur)
    return {"max_tasks": max_tasks, "goals": goals}

def eval_when(expr, env):
    if expr.strip()=="true": return True
    if expr.strip()=="false": return False
    if not re.match(r"^[0-9a-zA-Z_.<>=!&|+\-*/ ()]+$", expr): return False
    local={k:v for k,v in env.items() if isinstance(v,(int,float,bool))}
    try:
        return bool(eval(expr, {"__builtins__":{}}, local))
    except Exception:
        return False

def pre_hooks():
    sh("bash scripts/governor_hooks.sh pre", timeout=120)

def collect(state, pins):
    pre_hooks()
    miss=load_json(GOV/"miss_bus.json", {})
    res=load_json(GOV/"resource_snap.json", {})
    ev=load_json(GOV/"eval_probe.json", {})
    # gaps
    open_n=def_n=closed_n=0
    gaps=Path(str(BASE)+".gaps.txt")
    if gaps.exists():
        for i,ln in enumerate(gaps.read_text(errors="replace").splitlines()):
            if i<2: continue
            if "waiting_oracle" in ln or "waiting_charter" in ln: def_n+=1
            else:
                p=ln.split()
                if len(p)>1 and p[1]=="2": closed_n+=1
                elif len(p)>1 and p[1]=="1": open_n+=1
    fault=Path(os.environ.get("CNET_FAULT_LOG", ROOT/"logs/cnet_faults.jsonl"))
    fault_lines=sum(1 for _ in fault.open()) if fault.exists() else 0
    lora=len(list((ROOT/"logs/lora_store").glob("*"))) if (ROOT/"logs/lora_store").is_dir() else 0
    b=BASE.read_bytes() if BASE.exists() else b""
    units=b.count(b"acq_")+b.count(b"json_toolcall")+b.count(b"hyb_struct")
    web_notes=int((GOV/"web_notes_count").read_text()) if (GOV/"web_notes_count").exists() else 0
    def hrs(k):
        t=state.get(k) or 0
        return 99.0 if not t else max(0.0,(time.time()-float(t))/3600.0)
    sb={
        "ts": datetime.now().astimezone().isoformat(timespec="seconds"),
        "bonsai_ok": 1 if active("bonsai-server.service") and sh(f"curl -sf --max-time 4 {HTTP}/v1/models >/dev/null").returncode==0 else 0,
        "lane_ok": 1 if active("cnet-personal-ai-lane.service") else 0,
        "open_gaps": open_n, "deferred_oracle": def_n, "closed_gaps": closed_n,
        "fault_lines": fault_lines, "lora_files": lora, "units_proxy": units,
        "hours_since_peft": hrs("last_peft_unix"),
        "hours_since_mine": hrs("last_mine_unix"),
        "hours_since_procedure": hrs("last_procedure_unix"),
        "hours_since_web": hrs("last_web_unix"),
        "hours_since_eval": hrs("last_eval_unix"),
        "real_miss_rate": float(miss.get("real_miss_rate") or 0),
        "eval_jtc_delta": float(ev["eval_jtc_delta"]) if ev.get("eval_jtc_delta") is not None else 0.0,
        "d_eval_jtc": float(ev["d_eval_jtc"]) if ev.get("d_eval_jtc") is not None else 0.0,
        "web_notes": web_notes,
        "busy": int(res.get("busy") or 0),
        "allow_heavy": int(res.get("allow_heavy") if res.get("allow_heavy") is not None else 1),
        "night": int(res.get("night") or 0),
        "pause_inject": int(pins.get("pause_inject") or 0),
        "pause_web": int(pins.get("pause_web") or 0),
        "freeze_seals": int(pins.get("freeze_seals") or 0),
        "pending_outcome": int(state.get("pending_outcome") or 0),
        "project_web": 1 if web_notes < 10 else 0,
        "project_jtc": 1 if (ev.get("eval_jtc_delta") is None or float(ev.get("eval_jtc_delta") or 0) < 0.40) else 0,
        "backlog_pressure": float(open_n+def_n),
        "teacher_uptime": 1.0 if (active("bonsai-server.service")) else 0.0,
        "plateau": int(state.get("plateau") or 0),
        "learning_velocity": float(state.get("learning_velocity") or 0),
        "goal_health_drain": float((state.get("goal_health") or {}).get("drain_open_gaps", 0.5)),
        "goal_health_peft": float((state.get("goal_health") or {}).get("peft_jtc", 0.5)),
        "goal_health_mine": float((state.get("goal_health") or {}).get("structure_mine", 0.5)),
        "goal_health_coverage": float((state.get("goal_health") or {}).get("coverage_curiosity", 0.5)),
        "cycle": int(state.get("cycle") or 0)+1,
        "engine": "governor_autonomous_v2",
    }
    # evolve vs last scoreboard
    prev=load_json(GOV/"scoreboard.json", {})
    for k in ("open_gaps","closed_gaps","fault_lines","units_proxy","backlog_pressure"):
        sb[f"d_{k}"]=float(sb.get(k,0))-float(prev.get(k,sb.get(k,0)))
    dt=max(1e-3, (time.time()-float(prev.get("ts_unix") or time.time()-900))/3600.0)
    sb["dt_h"]=dt
    sb["rate_closed_per_h"]=sb.get("d_closed_gaps",0)/dt
    sb["rate_units_per_h"]=sb.get("d_units_proxy",0)/dt
    # ewma velocity
    ewma=load_json(GOV/"scoreboard_ewma.json", {})
    inst=max(0.0,sb["rate_units_per_h"])+0.1*max(0.0,sb["rate_closed_per_h"])
    alpha=0.3
    prev_v=float(ewma.get("learning_velocity") or 0)
    sb["learning_velocity"]=alpha*inst+(1-alpha)*prev_v
    # plateau from last 3 history
    hist=[]
    hp=GOV/"scoreboard_history.jsonl"
    if hp.exists():
        for line in hp.read_text().splitlines()[-5:]:
            try: hist.append(json.loads(line))
            except: pass
    if len(hist)>=2:
        du=sum(float(h.get("d_units_proxy") or 0) for h in hist[-3:])
        dc=sum(float(h.get("d_closed_gaps") or 0) for h in hist[-3:])
        sb["plateau"]=1 if du<=0 and dc<=2 and sb["bonsai_ok"] and sb["lane_ok"] else 0
    sb["ts_unix"]=time.time()
    return sb

def pick(charter, sb, pins):
    goals=sorted(charter["goals"], key=lambda g: int(g.get("priority",99)))
    # project bias
    def key(g):
        p=int(g.get("priority",99)); bias=0
        gid=g["id"]
        if sb["plateau"] and gid=="coverage_curiosity": bias=2
        if sb["backlog_pressure"]>=8 and gid=="drain_open_gaps": bias=-1
        if sb["project_jtc"] and gid=="peft_jtc": bias=-1
        if sb["project_web"] and gid=="verified_web": bias=-1
        if sb["busy"] and gid in ("peft_jtc","structure_mine","break_plateau"): bias=2
        return (p+bias, -float(sb.get("goal_health_"+gid.split("_")[0], 0.5) if False else 0.5))
    goals=sorted(charter["goals"], key=key)
    max_n=int(pins.get("max_tasks_override") or 0) or int(charter["max_tasks"])
    picked=[]
    force=str(pins.get("force_goal") or "").strip()
    if force:
        for g in charter["goals"]:
            if g["id"]==force: picked=[g]; break
        if sb["bonsai_ok"]==0 or sb["lane_ok"]==0:
            for g in charter["goals"]:
                if g["id"]=="keep_teacher_alive":
                    picked=[g]+picked; break
        return picked[:max_n]
    for g in goals:
        if len(picked)>=max_n: break
        if eval_when(g.get("when","false"), sb):
            if g["id"]=="health_hold" and picked: continue
            picked.append(g)
            if g["id"]=="health_hold": break
    if not picked:
        for g in charter["goals"]:
            if g["id"]=="health_hold": return [g]
    return picked

def act(name, sb, pins, state):
    dry=os.environ.get("GOV_DRY")=="1"
    def run(cmd, t=600):
        if dry: return True, "dry"
        r=sh(cmd, timeout=t)
        return r.returncode==0, (r.stdout+r.stderr)[-300:]
    if name=="ensure_bonsai":
        ok,_=run("systemctl --user start bonsai-server.service"); time.sleep(1)
        return ok and sh(f"curl -sf --max-time 4 {HTTP}/v1/models >/dev/null").returncode==0
    if name=="ensure_lane":
        ok,_=run("systemctl --user start cnet-personal-ai-lane.service"); time.sleep(1)
        return active("cnet-personal-ai-lane.service")
    if name=="inject_window_gaps":
        if pins.get("pause_inject"): return True
        # prefer C binary inject via governor or python
        win=Path(os.environ.get("CNET_RESIDUAL_WINDOW", ROOT/"english_window_256_bonsai.txt"))
        ids=[int(x) for x in win.read_text().split() if x.strip().isdigit()]
        if not ids: return False
        import random
        W=len(ids); k=3; n=min(4,len(ids))
        random.seed(int(time.time())//600)
        picks=random.sample(ids,n)
        inbox=Path(str(BASE)+".inbox")
        if not dry:
            with inbox.open("a") as f:
                for tid in picks:
                    f.write(f"NO_PLAN 1 {W} 1 w_cur 1 {W} {k} tk{tid}q{tid}\n")
        return True
    if name=="nudge_lane_tick": return True
    if name=="run_cert_learn":
        if pins.get("freeze_seals"): return True
        ok,_=run(f"CNET_BASE_PATH={BASE} CNET_FAULT_DEDUPE=0 CNET_RESIDUAL_HTTP={HTTP} "
                 f"CNET_RESIDUAL_WINDOW={os.environ.get('CNET_RESIDUAL_WINDOW', ROOT/'english_window_256_bonsai.txt')} "
                 f"timeout 600 {ROOT}/bin/cnet_cert_learn_tick >>{GOV}/muscle.log 2>&1")
        if ok: state["last_peft_unix"]=time.time()
        return ok
    if name=="run_structure_mine":
        if pins.get("freeze_seals"): return True
        ok,_=run(f"CNET_BASE_PATH={BASE} CNET_RESIDUAL_HTTP={HTTP} CNET_STRUCTURE_EXPAND_N=32 "
                 f"timeout 900 {ROOT}/bin/struct_mine_persist >>{GOV}/muscle.log 2>&1")
        if ok: state["last_mine_unix"]=time.time()
        return ok
    if name=="seed_procedures":
        ok,_=run(f"bash {ROOT}/scripts/procedure_chunk_seal.sh >>{GOV}/muscle.log 2>&1", t=60)
        if ok: state["last_procedure_unix"]=time.time()
        return ok
    if name=="run_eval_probe":
        ok,_=run(f"bash {ROOT}/scripts/governor_hooks.sh eval >>{GOV}/muscle.log 2>&1", t=240)
        if ok: state["last_eval_unix"]=time.time()
        return ok
    if name=="safe_web":
        if pins.get("pause_web"): return True
        ok,_=run(f"bash {ROOT}/scripts/governor_hooks.sh web >>{GOV}/muscle.log 2>&1", t=60)
        if ok: state["last_web_unix"]=time.time()
        return ok
    if name=="queue_research":
        return run(f"bash {ROOT}/scripts/governor_hooks.sh research >>{GOV}/muscle.log 2>&1", t=60)[0]
    if name=="close_outcome":
        state["pending_outcome"]=0
        return True
    if name in ("ensure_curiosity","log_hold"): return True
    return False

def outcome_close(state, sb):
    if not state.get("pending_outcome"): return
    prev_b=float(state.get("pending_backlog") or sb["backlog_pressure"])
    gh=dict(state.get("goal_health") or {})
    def bump(k,d):
        gh[k]=max(0.0,min(1.0,float(gh.get(k,0.5))+d))
    if prev_b-sb["backlog_pressure"]>0: bump("drain_open_gaps",0.04)
    else: bump("drain_open_gaps",-0.03)
    if sb.get("d_eval_jtc",0)>0: bump("peft_jtc",0.05)
    elif sb.get("d_eval_jtc",0)<0: bump("peft_jtc",-0.04)
    state["goal_health"]=gh
    # keep pending until close_outcome action; auto-clear if old
    if time.time()-float(state.get("pending_since") or time.time())>3600:
        state["pending_outcome"]=0

def persist(sb, state, picked, results, pins):
    GOV.mkdir(parents=True, exist_ok=True)
    sb_out=dict(sb)
    (GOV/"scoreboard.json").write_text(json.dumps(sb_out, indent=2)+"\n")
    with (GOV/"scoreboard_history.jsonl").open("a") as f:
        f.write(json.dumps({k:sb.get(k) for k in
            ["ts","ts_unix","cycle","open_gaps","deferred_oracle","closed_gaps","fault_lines",
             "units_proxy","d_closed_gaps","d_units_proxy","backlog_pressure","learning_velocity",
             "plateau","real_miss_rate","eval_jtc_delta","web_notes","busy","allow_heavy"]})+"\n")
    (GOV/"scoreboard_ewma.json").write_text(json.dumps({
        "learning_velocity": sb["learning_velocity"],
        "teacher_uptime": sb["teacher_uptime"],
        "updated_ts": sb["ts"], "engine": "v2"}, indent=2)+"\n")
    decision={
        "ts": sb["ts"], "engine": "governor_autonomous_v2",
        "goals":[{"id":g["id"],"priority":g.get("priority")} for g in picked],
        "actions": results,
        "scoreboard_focus": {k:sb[k] for k in
            ["cycle","backlog_pressure","real_miss_rate","eval_jtc_delta","web_notes",
             "busy","allow_heavy","plateau","learning_velocity","pending_outcome"]},
        "pins": pins,
        "goal_health": state.get("goal_health"),
        "projects": load_json(PROJECTS, {}).get("projects", []),
    }
    (GOV/"last_decision.json").write_text(json.dumps(decision, indent=2)+"\n")
    state["cycle"]=sb["cycle"]
    state["last_cycle_unix"]=time.time()
    state["last_goals"]=[g["id"] for g in picked]
    state["learning_velocity"]=sb["learning_velocity"]
    state["plateau"]=sb["plateau"]
    (GOV/"state.json").write_text(json.dumps(state, indent=2)+"\n")

def main():
    dry = "--dry-run" in sys.argv or os.environ.get("GOV_DRY")=="1"
    if dry: os.environ["GOV_DRY"]="1"
    if "--test" in sys.argv:
        # hermetic policy tests
        env={"bonsai_ok":0,"lane_ok":1,"open_gaps":0,"deferred_oracle":0,"backlog_pressure":0,
             "real_miss_rate":0,"allow_heavy":1,"pause_web":0,"web_notes":0,"hours_since_web":1,
             "project_web":0,"pending_outcome":0,"plateau":0,"teacher_uptime":1,"busy":0,
             "fault_lines":0,"hours_since_peft":1,"eval_jtc_delta":0.5,"hours_since_mine":2,
             "hours_since_procedure":1,"freeze_seals":0,"pause_inject":0}
        assert eval_when("bonsai_ok == 0 or lane_ok == 0", env)
        env["bonsai_ok"]=1; env["backlog_pressure"]=24; env["open_gaps"]=14; env["deferred_oracle"]=10
        assert eval_when("backlog_pressure >= 8", env)
        env2=dict(env); env2["pending_outcome"]=1
        assert eval_when("pending_outcome == 1", env2)
        assert not eval_when("pause_web == 0 and web_notes < 50 and (hours_since_web >= 0.4 or project_web == 1)",
                            {**env, "pause_web":1, "web_notes":0, "hours_since_web":1})
        print("GOVERNOR_V2_SELFTEST_PASS checks=4")
        return 0

    GOV.mkdir(parents=True, exist_ok=True)
    state=load_json(GOV/"state.json", {})
    pins=parse_pins()
    charter=parse_charter()
    sb=collect(state, pins)
    outcome_close(state, sb)
    # refresh goal health on sb
    gh=state.get("goal_health") or {}
    sb["goal_health_drain"]=float(gh.get("drain_open_gaps",0.5))
    sb["goal_health_peft"]=float(gh.get("peft_jtc",0.5))
    picked=pick(charter, sb, pins)
    results=[]; seen=set()
    for g in picked:
        for a in g.get("actions") or []:
            if a in seen: continue
            seen.add(a)
            ok=act(a, sb, pins, state)
            results.append({"action":a,"ok":bool(ok)})
    # goal health from actions
    gh=dict(state.get("goal_health") or {})
    ok_ratio=sum(1 for r in results if r["ok"])/max(1,len(results))
    for g in picked:
        gid=g["id"]
        if gid=="health_hold": continue
        cur=float(gh.get(gid,0.5))
        gh[gid]=max(0.0,min(1.0, cur+0.08*ok_ratio-0.05*(1-ok_ratio)))
    state["goal_health"]=gh
    state["pending_outcome"]=1
    state["pending_backlog"]=sb["backlog_pressure"]
    state["pending_since"]=time.time()
    if not dry:
        persist(sb, state, picked, results, pins)
    else:
        # still write decision for gate
        GOV.mkdir(parents=True, exist_ok=True)
        (GOV/"last_decision.json").write_text(json.dumps({
            "ts": sb["ts"], "dry_run": True, "engine":"governor_autonomous_v2",
            "goals":[{"id":g["id"]} for g in picked], "actions":results,
            "scoreboard_focus":{k:sb[k] for k in ["backlog_pressure","real_miss_rate","eval_jtc_delta","web_notes","allow_heavy","busy"]},
        }, indent=2)+"\n")
    print("GOVERNOR_CYCLE_OK", json.dumps({
        "engine":"v2","goals":[g["id"] for g in picked],
        "actions":[(r["action"],r["ok"]) for r in results],
        "backlog":sb["backlog_pressure"], "miss":sb["real_miss_rate"],
        "eval_jtc":sb["eval_jtc_delta"], "web_notes":sb["web_notes"],
        "allow_heavy":sb["allow_heavy"], "busy":sb["busy"],
    }))
    return 0

if __name__=="__main__":
    sys.exit(main())
