"""Synthetic clock/state tests only: never real elapsed acceptance."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import time
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/learning_unicode_soak.py'
if not SCRIPT.exists():
    raise AssertionError('LEARNING_SOAK_RED bounded observation runner missing')
spec = importlib.util.spec_from_file_location('learning_soak', SCRIPT)
soak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(soak)


class SoakTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cnet-soak-test-')
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        self.raw = (SCRIPT.parents[1]/'data/unicode17/UnicodeData-Latin1.txt').read_bytes()

    def put(self, path, data):
        path.write_bytes(data)
        path.chmod(0o600)

    def test_real_source_stages_are_exact_subsets_and_symbols_never_relabel(self):
        stages = [soak.tables(self.raw, n) for n in range(3)]
        for stage, counts in zip(stages, ((16,16),(32,32),(58,56))):
            for name, count in zip(soak.NUMERIC, counts):
                lines = stage[name].decode().splitlines()
                self.assertEqual('rows '+str(count), lines[5])
                self.assertEqual(count, len(lines[6:]))
                self.assertTrue(set(lines[6:]) <= set(stages[-1][name].decode().splitlines()[6:]))
            for name in soak.SYMBOLIC:
                self.assertEqual(stage[name], stages[-1][name])
        for name in soak.DATASETS:
            suffix = '.symbols.tsv' if name in soak.SYMBOLIC else '.tsv'
            self.assertEqual(stages[-1][name], (SCRIPT.parents[1]/'data/unicode17'/ (name+suffix)).read_bytes())

    def test_modified_raw_source_and_bad_stage_refuse(self):
        for raw, stage in ((self.raw+b'\n',0), (self.raw,3), (self.raw,True)):
            with self.assertRaises(soak.Refused): soak.tables(raw, stage)

    def test_private_reader_refuses_link_fifo_permissions_and_oversize(self):
        original = self.root/'data'
        self.put(original, b'ok')
        self.assertEqual(b'ok', soak.read_private(original, 2))
        with self.assertRaises(soak.Refused): soak.read_private(original, 1)
        link = self.root/'link'; link.symlink_to(original)
        with self.assertRaises((soak.Refused, OSError)): soak.read_private(link, 2)
        fifo = self.root/'fifo'; os.mkfifo(fifo, 0o600)
        with self.assertRaises(soak.Refused): soak.read_private(fifo, 2)
        original.chmod(0o640)
        with self.assertRaises(soak.Refused): soak.read_private(original, 2)

    def test_duplicate_and_nonfinite_json_refuse(self):
        for raw in (b'{"a":1,"a":2}', b'{"a":NaN}', b'['*10000):
            with self.assertRaises(soak.Refused): soak.decode(raw)

    def test_child_output_and_time_are_bounded_on_both_pipes(self):
        call=soak.bounded_call(['/usr/bin/python3','-c','print("ok")'],self.root,1)
        self.assertEqual((0,b'ok\n',b''),call)
        for script in ('import os; os.write(1,b"x"*1000000)',
                       'import os; os.write(2,b"x"*1000000)',
                       'import time; time.sleep(30)'):
            start=time.monotonic()
            with self.assertRaises(soak.Refused):
                soak.bounded_call(['/usr/bin/python3','-c',script],self.root,.2)
            self.assertLess(time.monotonic()-start,3)

    def test_exclusive_receipts_and_compare_before_source_replace(self):
        path = self.root/'source.tsv'
        soak.publish(path, b'old')
        with self.assertRaises(FileExistsError): soak.publish(path, b'reset')
        with self.assertRaises(soak.Refused): soak.replace_source(path, b'wrong', b'new')
        self.assertEqual(b'old', path.read_bytes())
        soak.replace_source(path, b'old', b'new')
        self.assertEqual(b'new', path.read_bytes())

    def test_boot_suspend_and_rollback_fail_without_renewing_start(self):
        for boot, now in (('other',110),('boot',99),('boot',401)):
            with self.assertRaises(soak.Refused): soak.check_clock('boot',100,boot,now,300)
        soak.check_clock('boot',100,'boot',400,300)

    def status(self, jobs=4, state='running', outstanding=None, pending=None):
        return dict(event='learning_status', paused=False, jobs=jobs, run_state=state,
                    outstanding_state=outstanding, pending_intent=pending,
                    run=dict(Boot='boot', StartNanoseconds=1000000000,
                             LastNanoseconds=1000000000, TickCount=10, State=state,
                             LastAction='budget_complete' if state=='budget_complete' else 'idle'))

    def test_transitions_have_fixed_deadline_and_do_not_call_jobs_accepted(self):
        self.assertFalse(soak.ready(self.status(2, outstanding='probation'),0,50))
        self.assertTrue(soak.ready(self.status(),0,50))
        self.assertTrue(soak.ready(self.status(6),1,86450))
        for value, stage, elapsed in ((self.status(3),0,181), (self.status(5),0,50),
                                      (self.status(6),1,86399), (self.status(6),1,86600)):
            # Settled exact reservations after the deadline are valid; missing
            # only fails at the deadline. The fourth item is changed below.
            if elapsed == 86600: value['outstanding_state'] = 'probation'
            with self.assertRaises(soak.Refused): soak.ready(value,stage,elapsed)
        value=self.status(); value['paused']=True
        with self.assertRaises(soak.Refused): soak.ready(value,0,50)

    def test_owner_heartbeat_cannot_stall_regress_or_disagree_with_status(self):
        status=self.status()
        self.assertEqual((1.,1.,10),soak.check_run(status,'boot',2,None,None,None))
        for field,value in [('LastNanoseconds',3000000000),('TickCount',True),
                            ('State','failed'),('Boot','different')]:
            changed=self.status(); changed['run'][field]=value
            with self.assertRaises(soak.Refused): soak.check_run(changed,'boot',2,None,None,None)
        for now,start,last,ticks in ((122,None,None,None),(2,0,None,None),
                                    (2,1,1.5,10),(2,1,1,11)):
            with self.assertRaises(soak.Refused): soak.check_run(status,'boot',now,start,last,ticks)

    def receipt(self, name='unicode17_upper_latin1', count=16):
        return dict(event='learning_live_verification', dataset=name, checked_keys=256,
                    correct_answers=count, correct_abstentions=256-count,
                    missing_answers=0, wrong_answers=0, symbol_keys=0,
                    correct_symbol_answers=0, correct_symbol_abstentions=0,
                    missing_symbol_answers=0, wrong_symbol_answers=0, passed=True,
                    source_sha256='s', active_sha256='a'*64, revision=1,
                    boot='boot', start_ns=100, end_ns=110,
                    managed_sha256='m', native_sha256='n', policy_sha256='p')

    def test_all_receipt_counts_pins_identity_and_actual_time_are_required(self):
        expected = dict(managed_sha256='m',native_sha256='n',policy_sha256='p')
        good = self.receipt()
        soak.check_receipt(good, soak.NUMERIC[0],16,'s',expected,'boot',90,120)
        for key,value in [('passed',False),('wrong_answers',1),('correct_answers',True),
                          ('source_sha256','different'),('policy_sha256','q'),
                          ('boot','other'),('start_ns',89),('end_ns',121),
                          ('correct_abstentions',239),('missing_symbol_answers',1)]:
            bad=dict(good); bad[key]=value
            with self.assertRaises(soak.Refused):
                soak.check_receipt(bad,soak.NUMERIC[0],16,'s',expected,'boot',90,120)

    def test_symbol_answers_and_unknown_abstention_are_required(self):
        good=self.receipt('ascii_category',95)
        good.update(symbol_keys=95,correct_symbol_answers=95,correct_symbol_abstentions=1)
        expected=dict(managed_sha256='m',native_sha256='n',policy_sha256='p')
        soak.check_receipt(good,'ascii_category',95,'s',expected,'boot',90,120)
        good['correct_symbol_abstentions']=0
        with self.assertRaises(soak.Refused): soak.check_receipt(good,'ascii_category',95,'s',expected,'boot',90,120)

    def test_synthetic_demand_never_hides_wrong_verified_answer(self):
        observer=soak.Observer.__new__(soak.Observer)
        observer.sources=[soak.tables(self.raw,n) for n in range(3)]
        recorded=[]
        observer.record=lambda event,**fields: recorded.append((event,fields))
        observer.command=lambda verb,name,key: dict(event='learning_answer',dataset=name,
                                                  key=int(key),verified=True,value=0)
        with self.assertRaises(soak.Refused): observer.demand(1)
        self.assertEqual(0,recorded[-1][1]['receipt']['value'])

    def test_final_requires_real_span_all_stages_settled_budget_and_no_product_claim(self):
        status=self.status(8,'budget_complete')
        result=soak.finish(status,{0,1,2},100,100+259200)
        self.assertEqual('passed',result['unicode_soak'])
        self.assertIs(result['product_acceptance'],False)
        for stages, first, last, state in (({0,1},100,259300,status),
                   ({0,1,2},100,259299,status),({0,1,2},100,259300,self.status(8))):
            with self.assertRaises(soak.Refused): soak.finish(state,stages,first,last)


if __name__ == '__main__': unittest.main()
