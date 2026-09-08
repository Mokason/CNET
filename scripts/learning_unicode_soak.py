#!/usr/bin/env python3
"""One-shot, owner-private Unicode workload observation; not product acceptance.

Uses the existing learning CLI and canonical source/capsule formats. No network,
model labels, allocator fitting, automatic restart or renewal of an elapsed run.
The systemd owner MUST bind its lifetime to this monitor; see the runbook.
"""
import hashlib
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time

RAW_HASH = '75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4'
NUMERIC = ('unicode17_upper_latin1', 'unicode17_lower_latin1')
SYMBOLIC = ('ascii_category', 'ascii_bidi')
DATASETS = NUMERIC + SYMBOLIC
STAGE_TIMES = (0, 86400, 172800)
TRANSITION_SECONDS = 180
INTERVAL = 60
MAX_GAP = 300
RUN_SECONDS = 259800  # 72 h + 10 min; first full observation must precede +180 s.
MAX_ROUNDS = 4400
MAX_RECEIPTS = 4500  # Each <=16 KiB: evidence outside work/ is bounded to <71 MiB.
MANAGED = ('cnet-control.dll','cnet-control.deps.json','cnet-control.runtimeconfig.json',
           'Microsoft.Data.Sqlite.dll','SQLitePCLRaw.core.dll','SQLitePCLRaw.batteries_v2.dll',
           'SQLitePCLRaw.provider.e_sqlite3.dll','runtimes/linux-x64/native/libe_sqlite3.so')
NATIVE = ('cnet_table_capsule','cnet_table_verify','cnet_learning_snapshot','cnet_capsulectl',
          'cnetd','libcnet_capsule_core.so')


class Refused(RuntimeError):
    """Fixed diagnostic code, not payload/exception prose."""


def require(value, code):
    if not value: raise Refused(code)


def digest(data): return hashlib.sha256(data).hexdigest()


def decode(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'duplicate_json')
            result[key] = value
        return result
    def constant(_): raise Refused('nonfinite_json')
    try:
        return json.loads(data, object_pairs_hook=unique, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError):
        raise Refused('invalid_json') from None


def private_directory(path):
    require(path.is_absolute() and path.resolve() == path, 'directory_path')
    info = path.lstat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
            and info.st_mode & 0o077 == 0, 'directory_boundary')


def read_private(path, limit):
    private_directory(path.parent)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
                and info.st_nlink == 1 and info.st_mode & 0o077 == 0
                and 0 < info.st_size <= limit, 'file_boundary')
        data = stream.read(limit+1)
        require(len(data) == info.st_size, 'file_changed')
        return data


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try: os.fsync(fd)
    finally: os.close(fd)


def publish(path, data):
    private_directory(path.parent)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data); stream.flush(); os.fsync(stream.fileno())
    sync_directory(path.parent)


def replace_source(path, previous, data):
    require(read_private(path,4096) == previous, 'source_compare_failed')
    fd, name = tempfile.mkstemp(prefix='.soak-source-', dir=path.parent)
    # A failed publication retains its temporary artifact for investigation.
    with os.fdopen(fd,'wb') as stream:
        stream.write(data); stream.flush(); os.fsync(stream.fileno())
    require(read_private(path,4096) == previous, 'source_compare_failed')
    os.replace(name,path); sync_directory(path.parent)


def tables(raw, stage):
    require(type(stage) is int and stage in range(3) and digest(raw) == RAW_HASH, 'source_authority')
    rows = [line.split(';') for line in raw.decode('ascii').splitlines()]
    require(len(rows) == 256 and all(len(row)==15 and row[0]==f'{n:04X}'
                                  for n,row in enumerate(rows)), 'source_shape')
    result = {}
    for name, field in zip(NUMERIC,(12,13)):
        values = [(int(row[0],16),int(row[field],16)) for row in rows if row[field]]
        values = values[:(16,32,len(values))[stage]]
        result[name] = canonical(name,values,False)
    printable = sorted((row[1].replace(' ','_'),row) for row in rows[32:127])
    for name,field in zip(SYMBOLIC,(2,4)):
        result[name] = canonical(name,[(token,row[field]) for token,row in printable],True)
    return result


def canonical(name, values, symbolic):
    header = ['CNET_LOCAL_SYMBOLS_V1' if symbolic else 'CNET_LOCAL_TABLE_V1',
              'dataset '+name,'authority verified_tool','input_bits 8','output_bits 16',
              'rows '+str(len(values))]
    return ('\n'.join(header+[str(key)+'\t'+str(value) for key,value in values])+'\n').encode('ascii')


def clock():
    return (Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
            time.clock_gettime_ns(time.CLOCK_BOOTTIME)/1_000_000_000)


def check_clock(boot, last, current_boot, now, gap):
    require(current_boot == boot and last <= now <= last+gap, 'clock_or_gap_failed')


def ready(status, stage, elapsed):
    require(status.get('event') == 'learning_status' and status.get('run_state') in
            {'running','budget_complete'}, 'run_failed')
    # Successful budget cleanup deliberately persists paused=true.
    require(status.get('paused') is False or
            (status.get('paused') is True and status['run_state']=='budget_complete'), 'run_paused')
    jobs = status.get('jobs')
    require(type(jobs) is int and 0 <= jobs <= 4+2*stage, 'reservation_count')
    require(elapsed >= STAGE_TIMES[stage], 'early_stage')
    settled = (jobs == 4+2*stage and status.get('pending_intent') is None
               and status.get('outstanding_state') is None)
    require(settled or elapsed <= STAGE_TIMES[stage]+TRANSITION_SECONDS, 'transition_expired')
    return settled


def check_run(status, boot, now, previous_start, previous_last, previous_ticks):
    run=status.get('run')
    require(isinstance(run,dict) and run.get('Boot')==boot and
            all(type(run.get(key)) is int for key in ('StartNanoseconds','LastNanoseconds','TickCount'))
            and run.get('State')==status.get('run_state'), 'run_identity')
    start=run['StartNanoseconds']/1_000_000_000
    last=run['LastNanoseconds']/1_000_000_000
    ticks=run['TickCount']
    require(0<=start<=last<=now and now-last<=120 and ticks>=0
            and (previous_start is None or start==previous_start)
            and (previous_last is None or last>=previous_last)
            and (previous_ticks is None or ticks>=previous_ticks), 'owner_heartbeat')
    return start,last,ticks


def check_receipt(value, name, count, source, pins, boot, start, end):
    expected = dict(event='learning_live_verification',dataset=name,checked_keys=256,
                    correct_answers=count,correct_abstentions=256-count,missing_answers=0,
                    wrong_answers=0,symbol_keys=95 if name in SYMBOLIC else 0,
                    correct_symbol_answers=95 if name in SYMBOLIC else 0,
                    correct_symbol_abstentions=1 if name in SYMBOLIC else 0,
                    missing_symbol_answers=0,wrong_symbol_answers=0,passed=True,
                    source_sha256=source,boot=boot,**pins)
    for key, wanted in expected.items():
        require(type(value.get(key)) is type(wanted) and value[key]==wanted, 'verification_failed')
    require(type(value.get('start_ns')) is int and type(value.get('end_ns')) is int
            and start <= value['start_ns'] <= value['end_ns'] <= end, 'verification_clock')
    active=value.get('active_sha256')
    require(isinstance(active,str) and len(active)==64 and all(c in '0123456789abcdef' for c in active)
            and type(value.get('revision')) is int and value['revision']>=0, 'verification_identity')


def finish(status, stages, first, last):
    require(status.get('run_state')=='budget_complete' and status.get('jobs')==8
            and status.get('pending_intent') is None and status.get('outstanding_state') is None
            and stages=={0,1,2} and first is not None and last-first>=259200, 'completion_withheld')
    return dict(unicode_soak='passed',product_acceptance=False,
                allocator_improvement='withheld',demand_origin='synthetic_operational_workload')


def bounded_call(argv, cwd, seconds=35):
    boot,started=clock()
    output=[bytearray(),bytearray()]
    child=subprocess.Popen(argv,cwd=cwd,env={},stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(child.stdout,selectors.EVENT_READ,0)
            selector.register(child.stderr,selectors.EVENT_READ,1)
            last=started
            while selector.get_map() or child.poll() is None:
                current,now=clock(); check_clock(boot,last,current,now,seconds)
                require(now-started<seconds,'command_timeout'); last=now
                for key,_ in selector.select(min(.1,seconds-(now-started))):
                    chunk=os.read(key.fd,32769)
                    if not chunk: selector.unregister(key.fileobj)
                    else:
                        require(len(output[key.data])+len(chunk)<=32768,'command_output_limit')
                        output[key.data].extend(chunk)
            return child.wait(),bytes(output[0]),bytes(output[1])
    finally:
        # The installed CLI and its fixed native children belong to this new
        # process group. Never target the caller's group or unrelated services.
        try: os.killpg(child.pid,signal.SIGKILL)
        except ProcessLookupError: pass
        child.wait()
        child.stdout.close(); child.stderr.close()


class Observer:
    def __init__(self, root):
        self.owns=False
        self.evidence=[]
        self.root = root
        private_directory(root)
        for parent in root.parents:
            info=parent.lstat()
            require(stat.S_ISDIR(info.st_mode) and info.st_uid in {0,os.getuid()}
                    and info.st_mode & 0o022 == 0, 'untrusted_ancestor')
        require(Path(__file__).resolve()==root/'monitor.py', 'monitor_location')
        self.config_bytes=read_private(root/'soak.json',8192)
        self.config=decode(self.config_bytes)
        require(isinstance(self.config,dict) and set(self.config)=={'schema_version','dotnet','files'}
                and self.config['schema_version']==1, 'configuration')
        inventory = {'managed/'+name for name in MANAGED} | {'native/'+name for name in NATIVE}
        inventory |= {'managed.json','runtime.json','policy.json','monitor.py','UnicodeData-Latin1.txt'}
        require(isinstance(self.config['files'],dict) and set(self.config['files'])==inventory,
                'installation_inventory')
        host=Path(self.config['dotnet'])
        require(host.is_absolute() and host.resolve()==host and host.is_file(), 'dotnet_host')
        self.pins={name+'_sha256':self.config['files'][file] for name,file in
                   [('managed','managed.json'),('native','runtime.json'),('policy','policy.json')]}
        self.observation=root/'observation'
        private_directory(self.observation)
        self.sequence=0
        self.chain='0'*64
        self.check_installation()
        self.sources=[tables(read_private(root/'UnicodeData-Latin1.txt',16384),stage) for stage in range(3)]

    def check_installation(self):
        require(read_private(self.root/'soak.json',8192)==self.config_bytes, 'configuration_changed')
        for name, expected in self.config['files'].items():
            require(digest(read_private(self.root/name,64*1024*1024))==expected, 'installation_changed')
        policy=decode(read_private(self.root/'policy.json',16384))
        require(policy.get('max_run_seconds')==RUN_SECONDS and policy.get('allocator_enabled') is False
                and {d['id'] for d in policy['datasets']}==set(DATASETS), 'campaign_policy')

    def command(self, verb, *args):
        code,stdout,stderr=bounded_call([self.config['dotnet'],str(self.root/'managed/cnet-control.dll'),
                                       'learning',verb,str(self.root),*args],self.root)
        if code != 0:
            # Preserve the bounded structured refusal, never arbitrary stderr.
            detail=decode(stderr or stdout)
            self.record('command_refused',verb=verb,receipt=detail)
            raise Refused('command_refused')
        require(not stderr, 'command_stderr')
        value=decode(stdout)
        require(isinstance(value,dict), 'command_protocol')
        return value

    def record(self, event, **fields):
        require(self.sequence<MAX_RECEIPTS, 'receipt_count')
        boot,now=clock()
        body=json.dumps(dict(event=event,sequence=self.sequence,previous_sha256=self.chain,
                        boot=boot,at_seconds=now,product_acceptance=False,**fields),
                        sort_keys=True,separators=(',',':')).encode()+b'\n'
        require(len(body)<=16384, 'receipt_size')
        publish(self.observation/f'{self.sequence:05d}.json',body)
        self.chain=digest(body); self.sequence+=1
        print(json.dumps(dict(event=event,sequence=self.sequence-1,receipt_sha256=self.chain,
                              product_acceptance=False)),flush=True)

    def demand(self, stage):
        for name in DATASETS if stage==0 else NUMERIC:
            rows=self.sources[stage][name].decode().splitlines()[6:]
            key=rows[-1].split('\t')[0]
            expected=rows[-1].split('\t')[1]
            value=self.command('lookup' if name in SYMBOLIC else 'ask',name,key)
            self.record('synthetic_demand',dataset=name,stage=stage,key=key,receipt=value)
            require(type(value.get('verified')) is bool, 'demand_protocol')
            require(value.get('dataset')==name and value.get('event')==
                    ('learning_symbol_answer' if name in SYMBOLIC else 'learning_answer'), 'demand_protocol')
            if name in SYMBOLIC:
                require(value.get('token')==key and (not value['verified'] or
                        value.get('label')==expected), 'demand_answer_mismatch')
            else:
                require(type(value.get('key')) is int and value['key']==int(key) and
                        (not value['verified'] or (type(value.get('value')) is int and
                         value['value']==int(expected))), 'demand_answer_mismatch')

    def run(self):
        boot,started=clock()
        # Exclusive permanent marker: a killed/failed observer is never restarted
        # into apparently continuous evidence. Retain all files for diagnosis.
        publish(self.observation/'started.json',json.dumps(dict(boot=boot,start=started,
                soak_sha256=digest(self.config_bytes))).encode())
        self.owns=True
        require(len(list(self.observation.iterdir()))==1, 'observation_not_empty')
        self.record('observation_started',sources=[{name:digest(data) for name,data in stage.items()}
                                                  for stage in self.sources])
        notify=os.environ.get('NOTIFY_SOCKET')
        if notify:
            with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as channel:
                channel.connect('\0'+notify[1:] if notify.startswith('@') else notify)
                channel.sendall(b'READY=1')
        stage=0; stages=set(); run_start=None; first=None; last_good=None; last=started
        last_owner=None; owner_ticks=None
        for iteration in range(MAX_ROUNDS):
            self.evidence=[]
            current,now=clock(); check_clock(boot,last,current,now,MAX_GAP)
            if last_good is not None: check_clock(boot,last_good,current,now,MAX_GAP)
            round_start=now
            self.check_installation()
            status=self.command('status')
            current,now=clock(); check_clock(boot,round_start,current,now,MAX_GAP)
            require(all(status.get(key)==value for key,value in self.pins.items()), 'status_pins')
            if status.get('run_state')=='not_started':
                require(run_start is None and now-started<=60, 'owner_start_missing')
                self.record('awaiting_owner',status=status)
                time.sleep(1); continue
            observed_start,last_owner,owner_ticks=check_run(status,boot,now,run_start,last_owner,owner_ticks)
            require(started<=observed_start, 'run_predates_observer')
            if run_start is None:
                run_start=observed_start
                self.demand(0)
            elapsed=now-run_start
            if stage<2 and elapsed>=STAGE_TIMES[stage+1]:
                require(stage in stages and ready(status,stage,elapsed), 'prior_stage_unsettled')
                require(elapsed<=STAGE_TIMES[stage+1]+MAX_GAP, 'stage_schedule_missed')
                self.record('source_transition_intent',stage=stage+1,
                            deadline_run_seconds=STAGE_TIMES[stage+1]+TRANSITION_SECONDS,
                            previous={n:digest(self.sources[stage][n]) for n in NUMERIC},
                            next={n:digest(self.sources[stage+1][n]) for n in NUMERIC})
                for name in NUMERIC:
                    replace_source(self.root/'work/data'/ (name+'.tsv'),
                                   self.sources[stage][name],self.sources[stage+1][name])
                    self.record('source_replaced',stage=stage+1,dataset=name,
                                source_sha256=digest(self.sources[stage+1][name]))
                stage+=1; self.demand(stage)
                status=self.command('status')
                require(all(status.get(key)==value for key,value in self.pins.items()), 'status_pins')
                current,now=clock(); check_clock(boot,round_start,current,now,MAX_GAP)
                _,last_owner,owner_ticks=check_run(status,boot,now,run_start,last_owner,owner_ticks)
                elapsed=now-run_start
            for name,source in self.sources[stage].items():
                require(read_private(self.root/'work/data'/(name+'.tsv'),4096)==source, 'source_changed')
            receipts=[]
            if ready(status,stage,elapsed):
                for name,source in self.sources[stage].items():
                    receipt=self.command('verify',name)
                    self.evidence.append(receipt)
                    current,finish_time=clock()
                    check_clock(boot,round_start,current,finish_time,MAX_GAP)
                    check_receipt(receipt,name,len(source.splitlines())-6,digest(source),self.pins,
                                  boot,int(round_start*1_000_000_000)-1024,
                                  int(finish_time*1_000_000_000)+1024)
                    receipts.append(receipt)
                require(len({(r['active_sha256'],r['revision']) for r in receipts})==1,'mixed_generation')
                stages.add(stage)
                if first is None:
                    require(finish_time-run_start<=TRANSITION_SECONDS,'initial_observation_late')
                    first=finish_time
                if last_good is not None: check_clock(boot,last_good,current,finish_time,MAX_GAP)
                last_good=finish_time
            current,last=clock(); check_clock(boot,round_start,current,last,MAX_GAP)
            self.record('workload_observation',stage=stage,run_elapsed_seconds=last-run_start,
                        first_success_seconds=first,last_success_seconds=last_good,
                        status=status,qualified=bool(receipts),receipts=receipts)
            if status['run_state']=='budget_complete':
                require(bool(receipts), 'terminal_not_verified')
                result=finish(status,stages,first,last_good)
                self.record('observation_finished',result=result)
                return 0
            time.sleep(max(0,INTERVAL-(last-round_start)))
        raise Refused('observation_budget')


def main():
    observer=None
    try:
        require(len(sys.argv)==2, 'usage')
        observer=Observer(Path(sys.argv[1]))
        return observer.run()
    except (Refused,OSError,ValueError,TypeError,KeyError,subprocess.SubprocessError) as error:
        # systemd BindsTo additionally handles SIGKILL/OOM and stop ordering.
        if observer is not None and observer.owns:
            code=str(error) if isinstance(error,Refused) else 'observer_io_or_protocol'
            try: observer.record('observation_failed',code=code,receipts=observer.evidence)
            except (Refused,OSError): pass
            try: observer.command('pause')
            except (Refused,OSError,ValueError,subprocess.SubprocessError): pass
        print('{"event":"unicode_soak_refused","product_acceptance":false}',file=sys.stderr)
        return 2


if __name__=='__main__': sys.exit(main())
