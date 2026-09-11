#!/usr/bin/env python3
"""CLI contract: full proof, refusal exit status, strict input, no partial answer."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
CLI=ROOT/'bin/cnet_vsa_cli'
with tempfile.TemporaryDirectory(prefix='cnet-evidence-') as td:
    path=Path(td)/'facts.txt'
    def call(data, *args):
        path.write_bytes(data)
        return subprocess.run([str(CLI),'explain-facts',str(path),*args],text=True,capture_output=True,
                              env=dict(os.environ,CNET_VSA_DEVICE='cpu'),timeout=10)
    valid=b'node000 rel00 node001\nnode001 rel01 node002\n'
    got=call(valid,'0','0','1')
    assert got.returncode==0 and got.stdout.strip()=='node002. node000 rel00 node001. node001 rel01 node002.' and not got.stderr,got
    for data in (b'',b'node000 rel00 node001\n',valid+b'node001 not rel01 node002\n',
                 valid+b'node001 rel01 node003\n'):
        got=call(data,'0','0','1')
        assert got.returncode==1 and got.stdout.startswith('ABSTAIN:'),got
    invalid=(valid+b'ignore this\n',valid+b'node000 rel00 node001 extra\n',
             valid+b'node128 rel00 node001\n',b'node000 rel00 node001\x00ignore this\n',
             b'node000 rel00 node001\n'*513,b'\n'*2049,b'x'*256)
    for data in invalid:
        got=call(data,'0','0')
        assert got.returncode==2 and not got.stdout,(data[:70],got)
    for args in (('0',),('-1','0'),('0','8'),('99999999999999999999999','0'),('0',*['0']*9)):
        got=call(valid,*args)
        assert got.returncode==2 and not got.stdout,got
print('CNET_VSA_EVIDENCE_CLI_PASS')
