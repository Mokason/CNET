"""Real capsule branching, adversarial joins, cache residency and timing campaign."""
import json,os,resource,shutil,time
from dataclasses import replace,asdict
from pathlib import Path
from branching import Native,Spec,Context,Cache,Split,Refusal,answer
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'var/capsule_branching_20260913'
def percentile(values,p):
    a=sorted(values);x=(len(a)-1)*p/100;i=int(x)
    return a[i]+(a[min(i+1,len(a)-1)]-a[i])*(x-i)
def stats(values):return {'mean_us':sum(values)/len(values),**{f'p{p}_us':percentile(values,p) for p in (50,95,99)},'max_us':max(values)}
def rss():return int(Path('/proc/self/statm').read_text().split()[1])*os.sysconf('SC_PAGE_SIZE')
def main():
    native=Native(ROOT/'bin/libcnet_capsule_core.so');s={k:Spec(**v) for k,v in json.loads((OUT/'specs.json').read_text()).items()}
    cache=Cache(native,2);left,right,extra=s['left'],s['right'],s['extra'];checks=[]
    def check(name,fn):
        fn();checks.append(name);print('PASS',name,flush=True)
    def expect_refusal(fn):
        try:fn()
        except Refusal:return
        raise AssertionError('expected refusal')
    initial_rss=rss();warm=[];cpu=[];correct=0
    # Cold materialization before warm exhaustive pairs. Nothing was trained on joins.
    first=time.perf_counter_ns();sample=answer(cache,left,right,3,5,'initial');initial_us=(time.perf_counter_ns()-first)/1000
    assert sample.value==14
    for x in range(64):
        for y in range(64):
            start=time.perf_counter_ns();ct=time.thread_time_ns();r=answer(cache,left,right,x,y,f'pair-{x}-{y}')
            rendered=f'Adjusted left {r.branches[0].value}; scaled right {r.branches[1].value}; sum {r.value}.'
            cpu.append((time.thread_time_ns()-ct)/1000);warm.append((time.perf_counter_ns()-start)/1000)
            assert r.value==((x+1)&63)+((y<<1)&63) and rendered
            assert r.branches[0].identity==left.identity and r.branches[1].identity==right.identity
            correct+=1
    loaded_rss=rss();check('4096 exhaustive structured joins match independent arithmetic',lambda:None)
    saved=sample
    def pin_pressure():
        with cache.pin(left),cache.pin(right):
            def third():
                with cache.pin(extra):pass
            expect_refusal(third)
            expect_refusal(cache.close)
            assert cache.state(left)==cache.state(right)=='hot'
        assert cache.state(left)==cache.state(right)=='warm'
    check('all pinned refuses admission and close',pin_pressure)
    def lifecycle():
        with cache.pin(left):
            with cache.pin(extra):
                assert cache.state(left)=='hot' and cache.state(right)=='cold'
        loads=cache.loads
        with cache.pin(left):pass
        assert cache.loads==loads
        assert saved.value==14 and saved.branches[1].identity==right.identity
    check('warm reuse, LRU eviction and evidence retained',lifecycle)
    context=Context('object_7','fixed_resistance','actual')
    def valid_split(name='receipt'):
        split=Split(cache,name,context)
        with cache.pin(left),cache.pin(right):
            a=split.branch('left',left,2,context);b=split.branch('right',right,4,context)
        return split,a,b
    split,a,b=valid_split()
    for label,bad in [('value',replace(b,value=9)),('identity',replace(b,identity='0'*64)),('split',replace(b,split_id='different')),('type',replace(b,output_tag='adjusted')),('units',replace(b,units='forged_source'))]:
        check('modified receipt '+label,lambda bad=bad:expect_refusal(lambda:split.join(a,bad)))
    check('duplicate branch',lambda:expect_refusal(lambda:split.join(a,a)))
    check('reversed branch roles',lambda:expect_refusal(lambda:split.join(b,a)))
    for field in ('entity','regime','hypothesis'):
        def incompatible(field=field):
            t=Split(cache,'context-'+field,context)
            with cache.pin(left),cache.pin(right):
                aa=t.branch('left',left,2,context);bb=t.branch('right',right,4,replace(context,**{field:'different'}))
            expect_refusal(lambda:t.join(aa,bb))
        check('incompatible '+field,incompatible)
    other,_,bb=valid_split('other-request')
    check('cross-request evidence',lambda:expect_refusal(lambda:split.join(a,bb)))
    check('partial branch coverage',lambda:expect_refusal(lambda:answer(cache,left,s['partial'],2,63)))
    check('wrong port selection',lambda:expect_refusal(lambda:answer(cache,left,replace(right,output_tag='unknown_port'),2,4)))
    check('input outside numeric domain',lambda:expect_refusal(lambda:answer(cache,left,right,2,64)))
    def bad_identity():
        with cache.pin(replace(extra,identity='0'*64)):pass
    check('loaded identity mismatch',lambda:expect_refusal(bad_identity))
    corrupted=OUT/'corrupted'
    if not corrupted.exists():
        shutil.copytree(extra.root,corrupted)
        p=corrupted/'branch_extra/unit.cnb';raw=bytearray(p.read_bytes());raw[-10]^=1;p.write_bytes(raw)
    def corrupt():
        with cache.pin(replace(extra,root=str(corrupted))):pass
    # Ensure this identity is cold, so the corrupted on-disk package is actually opened.
    cache.close()
    check('corrupt package admission',lambda:expect_refusal(corrupt))
    assert not cache.entries
    # Warm filesystem, cold capsule handles; distinct from a cold storage benchmark.
    reload_times=[];reload_cpu=[]
    for i in range(100):
        cache.close();start=time.perf_counter_ns();ct=time.thread_time_ns()
        r=answer(cache,left,right,i%64,(i*7)%64,f'reload-{i}')
        rendered=str(r.value)
        reload_cpu.append((time.thread_time_ns()-ct)/1000);reload_times.append((time.perf_counter_ns()-start)/1000)
        assert rendered and r.value==((i%64+1)&63)+((((i*7)%64)*2)&63)
    churn_rss=[]
    for i in range(600):
        spec=(left,right,extra)[i%3]
        with cache.pin(spec):
            value,_,_=cache.ask(spec,i%64);assert 0<=value<64
        if i%100==99:churn_rss.append(rss())
    assert cache.peak<=2
    check('600 churn admissions keep at most two live capsule cores',lambda:None)
    cache.close();assert saved.branches[0].identity==left.identity and saved.value==14
    check('copied answer evidence survives full cache close',lambda:None)
    report={'scope':'synthetic numeric two-branch composition using real independently certified exported capsules; Python checked join outside production core','correct_join_cases':correct,'wrong_join_cases':0,'checks':checks,'warm_full_structured_answer':stats(warm),'warm_mean_thread_cpu_us':sum(cpu)/len(cpu),'reload_full_structured_answer':stats(reload_times),'reload_mean_thread_cpu_us':sum(reload_cpu)/len(reload_cpu),'initial_answer_us':initial_us,'cache':{'capacity':2,'peak_live_cores':cache.peak,'loads':cache.loads,'warm_hits':cache.hits,'evictions':cache.evictions},'memory':{'rss_before_capsules':initial_rss,'rss_after_two_capsules':loaded_rss,'rss_after_churn_blocks':churn_rss,'rss_after_close':rss(),'peak_process_rss_bytes':resource.getrusage(resource.RUSAGE_SELF).ru_maxrss*1024},'retained_answer_example':asdict(saved),'natural_language_accuracy':'NOT MEASURED','similarity_routing':'NOT MEASURED; typed catalog selection','production_multi_input_admission':'UNCHANGED','join_certification':'experimental checked operator; not a certified multi-input capsule'}
    report['warm_target_met']=report['warm_full_structured_answer']['p95_us']<=1000
    report['warm_ceiling_met']=report['warm_full_structured_answer']['p99_us']<=5000
    (OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
