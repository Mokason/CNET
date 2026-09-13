import ctypes as ct,json,subprocess,hashlib,sys
from pathlib import Path
from branching import Native
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'var/capsule_branching_20260913'
def main():
    OUT.mkdir(exist_ok=True);native=Native(ROOT/'bin/libcnet_capsule_core.so');specs={};receipts=[]
    for role,pin,pout in [('left','raw_left','adjusted'),('right','raw_right','scaled'),('extra','raw_extra','other'),('partial','raw_right','scaled')]:
        root=OUT/role;root.mkdir(exist_ok=True,mode=0o700)
        rows=subprocess.run([sys.executable,str(Path(__file__).with_name('oracle.py')),role],capture_output=True,check=True).stdout
        source=OUT/(role+'.tsv');source.write_bytes(rows)
        argv=[str(ROOT/'bin/cnet_capsule_core'),'teach',str(root),'branch_'+role,pin,pout,'6','6','verified_tool',str(source)]
        call=subprocess.run(argv,capture_output=True,text=True,check=True,timeout=60)
        (OUT/(role+'_build.log')).write_text(call.stdout+call.stderr)
        assert 'CAPSULE_TEACH_PASS' in call.stdout and 'method=finite_domain_compile' in call.stdout
        error=ct.create_string_buffer(160);h=native.lib.cnet_capsule_core_open(str(root).encode(),error,len(error))
        if not h:raise RuntimeError(error.value)
        try:identity=native.identity(h)
        finally:native.close(h)
        specs[role]={'key':role,'root':str(root),'input_tag':pin,'output_tag':pout,'identity':identity}
        receipts.append({'argv':argv,'rows_sha256':hashlib.sha256(rows).hexdigest(),'identity':identity})
    (OUT/'specs.json').write_text(json.dumps(specs,indent=2)+'\n');(OUT/'build_receipts.json').write_text(json.dumps(receipts,indent=2)+'\n')
    print('BRANCH_FIXTURES_READY',len(specs))
if __name__=='__main__':
    main()
