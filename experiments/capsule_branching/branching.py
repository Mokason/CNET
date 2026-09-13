"""Experimental two-branch join over real, independently verified capsule cores.

Structured requests only. No production multi-port admission changes.
"""
import ctypes as ct
from contextlib import contextmanager
from dataclasses import dataclass

class Refusal(RuntimeError):pass

@dataclass(frozen=True)
class Spec:
    key:str
    root:str
    input_tag:str
    output_tag:str
    identity:str

@dataclass(frozen=True)
class Context:
    entity:str
    regime:str
    hypothesis:str

@dataclass(frozen=True)
class Evidence:
    split_id:str
    branch_id:str
    identity:str
    context:Context
    input_tag:str
    output_tag:str
    input_value:int
    value:int
    hops:int
    units:str

@dataclass(frozen=True)
class Joined:
    value:int
    operation:str
    context:Context
    branches:tuple

class Reply(ct.Structure):
    _fields_=[('verified',ct.c_int),('value',ct.c_uint),('hops',ct.c_size_t),('units',ct.c_char*512),('reason',ct.c_char*160)]
class Identity(ct.Structure):
    _fields_=[('unit',ct.c_char*64),('sha256',ct.c_char*65)]

class Native:
    def __init__(self,path):
        self.lib=ct.CDLL(str(path));l=self.lib
        l.cnet_capsule_core_open.argtypes=[ct.c_char_p,ct.c_void_p,ct.c_size_t];l.cnet_capsule_core_open.restype=ct.c_void_p
        l.cnet_capsule_core_close.argtypes=[ct.c_void_p];l.cnet_capsule_core_close.restype=None
        l.cnet_capsule_core_ask.argtypes=[ct.c_void_p,ct.c_char_p,ct.POINTER(Reply)]
        l.cnet_capsule_core_identities.argtypes=[ct.c_void_p,ct.POINTER(Identity),ct.c_size_t,ct.POINTER(ct.c_size_t)]
    def identity(self,handle):
        ids=(Identity*1)();count=ct.c_size_t()
        if self.lib.cnet_capsule_core_identities(handle,ids,1,ct.byref(count)) or count.value!=1:raise Refusal('expected exactly one capsule')
        return ids[0].sha256.decode()
    def open(self,spec):
        error=ct.create_string_buffer(160);h=self.lib.cnet_capsule_core_open(spec.root.encode(),error,len(error))
        if not h:raise Refusal(error.value.decode())
        try:
            if self.identity(h)!=spec.identity:raise Refusal('capsule identity mismatch')
        except BaseException:
            self.close(h);raise
        return h
    def close(self,h):self.lib.cnet_capsule_core_close(h)
    def ask(self,h,spec,value):
        if type(value) is not int or not 0<=value<64:raise Refusal('input outside fixture domain')
        # Tags come only from the test's fixed catalog, never unparsed prose.
        request=f'capsule {spec.input_tag} {spec.output_tag} {value}'
        r=Reply();rc=self.lib.cnet_capsule_core_ask(h,request.encode(),ct.byref(r))
        if rc or not r.verified:raise Refusal(r.reason.decode())
        return r.value,r.hops,r.units.decode()

class Cache:
    """Single-thread LRU; active leases cannot be evicted, and warm cores can.

Capacity counts actual open cores, including admission. On a failed admission
an evicted warm entry stays cold; immutable capsule files remain reloadable.
"""
    def __init__(self,native,capacity):
        if type(capacity) is not int or not 1<=capacity<=32:raise ValueError('unsupported cache capacity')
        self.native=native;self.capacity=capacity;self.entries={};self.tick=0
        self.loads=self.evictions=self.hits=self.peak=0
    def state(self,spec):
        e=self.entries.get(spec.identity)
        return 'cold' if e is None else ('hot' if e['pins'] else 'warm')
    @contextmanager
    def pin(self,spec):
        e=self.entries.get(spec.identity)
        if e is None:
            if len(self.entries)==self.capacity:
                warm=[(v['tick'],k) for k,v in self.entries.items() if not v['pins']]
                if not warm:raise Refusal('all resident capsules pinned')
                _,victim=min(warm);old=self.entries.pop(victim);self.native.close(old['handle']);self.evictions+=1
            h=self.native.open(spec);e={'handle':h,'pins':0,'tick':0};self.entries[spec.identity]=e;self.loads+=1
            self.peak=max(self.peak,len(self.entries))
        else:self.hits+=1
        self.tick+=1;e['tick']=self.tick;e['pins']+=1
        try:yield e['handle']
        finally:e['pins']-=1
    def ask(self,spec,value):
        e=self.entries.get(spec.identity)
        if e is None or not e['pins']:raise Refusal('branch must own a live lease')
        return self.native.ask(e['handle'],spec,value)
    def close(self):
        if any(e['pins'] for e in self.entries.values()):raise Refusal('cannot close pinned capsules')
        for e in self.entries.values():self.native.close(e['handle'])
        self.entries.clear()

class Split:
    """Two complementary branch receipts; alternative contexts cannot merge.

The join operation is an explicit experimental checked tool, not a certified
multi-input capsule. Receipts are checked against this request's issued values.
"""
    def __init__(self,cache,split_id,context):
        if not split_id or not all((context.entity,context.regime,context.hypothesis)):raise Refusal('missing context')
        self.cache=cache;self.split_id=split_id;self.context=context;self.issued={}
    def branch(self,branch_id,spec,value,context):
        if branch_id not in ('left','right') or branch_id in self.issued or len(self.issued)>=2:raise Refusal('duplicate or unsupported branch')
        result,hops,units=self.cache.ask(spec,value)
        e=Evidence(self.split_id,branch_id,spec.identity,context,spec.input_tag,spec.output_tag,value,result,hops,units)
        self.issued[branch_id]=e;return e
    def join(self,left,right):
        for role,e,tag in [('left',left,'adjusted'),('right',right,'scaled')]:
            if e.branch_id!=role or e!=self.issued.get(role):raise Refusal('unissued or modified evidence')
            if e.split_id!=self.split_id or e.context!=self.context:raise Refusal('incompatible branch context')
            if e.output_tag!=tag or not 0<=e.value<64 or not e.hops:raise Refusal('incompatible typed result')
        if left.identity==right.identity:raise Refusal('branches require distinct capsule identities')
        return Joined(left.value+right.value,'bounded_integer_sum_v1',self.context,(left,right))

def answer(cache,left,right,x,y,split_id='query'):
    context=Context('fixture_sample','steady','actual')
    split=Split(cache,split_id,context)
    with cache.pin(left),cache.pin(right):
        a=split.branch('left',left,x,context);b=split.branch('right',right,y,context)
        return split.join(a,b)
