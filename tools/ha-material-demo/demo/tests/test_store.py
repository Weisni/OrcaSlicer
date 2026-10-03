import tempfile
import unittest
from pathlib import Path

from custom_components.quack_material_demo.store import Store, Conflict


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / 'demo.sqlite3'
        self.store = Store(self.path)
        self.snapshot = self.store.snapshot()
        self.roll = self.snapshot['spools'][0]['uuid']

    def tearDown(self):
        self.temp.cleanup()

    def test_assignment_preserves_actual_material_and_substitute(self):
        result = self.store.assign('A1', self.roll, 0)
        self.assertEqual(result['revision'], 1)
        spool = next(s for s in self.store.snapshot()['spools'] if s['uuid'] == self.roll)
        self.assertEqual(spool['manufacturer'], 'Demo manufacturer')
        self.assertEqual(spool['bambu_material'], 'Bambu PLA')
        self.assertEqual(spool['material_preset'], 'Generic PLA @BBL P2S')
        with self.assertRaises(Conflict):
            self.store.assign('A1', self.roll, 0)

    def test_roll_cannot_occupy_two_slots(self):
        self.store.assign('A1', self.roll, 0)
        with self.assertRaises(Conflict):
            self.store.assign('A2', self.roll, 0)

    def test_reservation_settlement_is_durable_and_idempotent(self):
        self.store.assign('A1', self.roll, 0)
        job = self.store.start_job('same-file.3mf', [{'slot': 'A1', 'weight_mg': 20000}], 'request-1')
        initial = self.store.snapshot()['spools'][0]['remaining_mg']
        self.assertEqual(self.store.snapshot()['spools'][0]['reserved_mg'], 20000)
        reopened = Store(self.path)
        reopened.finish_job(job['uuid'], 'completed', {self.roll: 18000}, 'measured')
        reopened.finish_job(job['uuid'], 'completed', {self.roll: 18000}, 'measured')
        self.assertEqual(reopened.snapshot()['spools'][0]['remaining_mg'], initial - 18000)
        self.assertEqual(reopened.snapshot()['spools'][0]['reserved_mg'], 0)
        with self.assertRaises(Conflict):
            reopened.finish_job(job['uuid'], 'completed', {self.roll: 19000}, 'measured')

    def test_same_filename_is_two_distinct_attempts(self):
        self.store.assign('A1', self.roll, 0)
        first = self.store.start_job('repeat.3mf', [{'slot': 'A1', 'weight_mg': 1000}], 'request-a')
        replay = self.store.start_job('repeat.3mf', [{'slot': 'A1', 'weight_mg': 1000}], 'request-a')
        self.assertEqual(first['uuid'], replay['uuid'])
        self.store.finish_job(first['uuid'], 'completed', None, 'estimated')
        second = self.store.start_job('repeat.3mf', [{'slot': 'A1', 'weight_mg': 1000}], 'request-b')
        self.assertNotEqual(first['uuid'], second['uuid'])

    def test_failure_keeps_consumption_pending_until_reconciled(self):
        self.store.assign('A1', self.roll, 0)
        job = self.store.start_job('failed.3mf', [{'slot': 'A1', 'weight_mg': 10000}], 'fail-1')
        self.store.finish_job(job['uuid'], 'failed', None, 'unknown')
        self.assertEqual(self.store.snapshot()['spools'][0]['reserved_mg'], 10000)
        self.store.finish_job(job['uuid'], 'failed', {self.roll: 3000}, 'measured')
        self.assertEqual(self.store.snapshot()['spools'][0]['reserved_mg'], 0)

    def test_external_job_without_quantities_is_logged_not_deducted(self):
        job = self.store.start_job('SD-card.3mf', [], 'external-1')
        self.store.finish_job(job['uuid'], 'completed', None, 'unknown')
        self.assertEqual(self.store.snapshot()['jobs'][0]['state'], 'needs_review')

    def test_insufficient_stock_and_invalid_quantities_do_not_create_jobs(self):
        self.store.assign('A1', self.roll, 0)
        for value in (-1, True, 1.5, 999999999):
            with self.assertRaises((ValueError, Conflict)):
                self.store.start_job('bad.3mf', [{'slot': 'A1', 'weight_mg': value}], 'bad-' + str(value))
        self.assertEqual(self.store.snapshot()['jobs'], [])

    def test_atomic_multicolor_settlement_does_not_partially_deduct(self):
        second = self.snapshot['spools'][1]['uuid']
        self.store.assign('A1', self.roll, 0)
        self.store.assign('A2', second, 0)
        job = self.store.start_job('multi', [{'slot': 'A1', 'weight_mg': 1000}, {'slot': 'A2', 'weight_mg': 1000}], 'multi-1')
        before = self.store.snapshot()['spools'][0]['remaining_mg']
        with self.assertRaises((ValueError, Conflict)):
            self.store.finish_job(job['uuid'], 'completed', {self.roll: 1000, second: -1}, 'measured')
        self.assertEqual(self.store.snapshot()['spools'][0]['remaining_mg'], before)

    def test_profile_update_changes_binding_revision_but_not_stock(self):
        self.store.assign('A1', self.roll, 0)
        self.store.set_profile(self.roll, 'My actual PLA @P2S')
        data = self.store.snapshot()
        self.assertEqual(data['spools'][0]['material_preset'], 'My actual PLA @P2S')
        self.assertEqual(data['spools'][0]['remaining_mg'], 800000)
        self.assertEqual(data['slots'][0]['revision'], 2)

    def test_profile_change_is_rejected_during_unsettled_job(self):
        self.store.assign('A1', self.roll, 0)
        self.store.start_job('active', [{'slot': 'A1', 'weight_mg': 1000}], 'active-1')
        with self.assertRaises(Conflict):
            self.store.set_profile(self.roll, 'Changed PLA')

    def test_observer_ignores_retained_finish_and_survives_restart(self):
        self.store.observe_printer('finish', 'old print')
        self.assertEqual(self.store.snapshot()['jobs'], [])
        self.store.observe_printer('running', 'actual print')
        job_uuid = self.store.snapshot()['jobs'][0]['uuid']
        reopened = Store(self.path)
        reopened.observe_printer('pause', 'actual print')
        reopened.observe_printer('running', 'actual print')
        reopened.observe_printer('finish', 'actual print')
        reopened.observe_printer('finish', 'actual print')
        jobs = reopened.snapshot()['jobs']
        self.assertEqual(len(jobs), 1)
        self.assertEqual(jobs[0]['uuid'], job_uuid)
        self.assertEqual(jobs[0]['state'], 'needs_review')
        self.assertEqual(self.store.snapshot()['spools'][0]['remaining_mg'], 800000)

    def test_observer_records_repeated_filename_as_separate_attempts(self):
        for _ in range(2):
            self.store.observe_printer('running', 'repeat')
            self.store.observe_printer('finish', 'repeat')
        self.assertEqual(len(self.store.snapshot()['jobs']), 2)

    def test_settlement_preserves_other_open_reservations(self):
        self.store.assign('A1', self.roll, 0)
        first = self.store.start_job('first', [{'slot': 'A1', 'weight_mg': 100000}], 'first')
        self.store.start_job('second', [{'slot': 'A1', 'weight_mg': 700000}], 'second')
        with self.assertRaises(Conflict):
            self.store.finish_job(first['uuid'], 'completed', {self.roll: 200000}, 'measured')


if __name__ == '__main__':
    unittest.main()

class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name) / 'life.sqlite3')
    def tearDown(self): self.temp.cleanup()
    def new_roll(self):
        return self.store.dispatch('create', dict(product='Elegoo Rapid PETG Blue', manufacturer='ELEGOO',
            material_type='PETG', color='#2255AA', remaining_mg=1000000, nominal_mg=1000000,
            diameter_mm=1.75, density_g_cm3=1.26, material_preset='Elegoo Rapid PETG @BBL P2S - HA Demo', request_key='new-roll'))
    def test_auto_identity_create_retry_and_profile(self):
        a=self.new_roll(); b=self.new_roll()
        self.assertEqual(a['uuid'], b['uuid'])
        import uuid
        self.assertEqual(uuid.UUID(a['uuid']).version,4)
        roll=next(s for s in self.store.snapshot()['spools'] if s['uuid']==a['uuid'])
        self.assertEqual(roll['manufacturer'],'ELEGOO')
        self.assertEqual(roll['bambu_material'],'Bambu PETG')
        self.assertEqual(roll['remaining_mg'],1000000)
    def test_empty_archive_detaches_and_rejects_scan_restore_preserves_uuid(self):
        roll=self.new_roll()['uuid']; self.store.assign('A3',roll,0)
        job=self.store.start_job('consume-all',[dict(slot='A3',weight_mg=1000000)],'all')
        self.store.finish_job(job['uuid'],'completed',None,'estimated')
        with self.assertRaises(Conflict): self.store.assign('A4',roll,0)
        self.store.dispatch('archive',dict(spool_uuid=roll))
        s=self.store.snapshot(); self.assertIsNone(next(x for x in s['slots'] if x['id']=='A3')['spool_uuid'])
        with self.assertRaises(Conflict): self.store.assign('A4',roll,0)
        self.store.dispatch('restore',dict(spool_uuid=roll))
        self.store.dispatch('weigh',dict(spool_uuid=roll,remaining_mg=150000,request_key='scale'))
        self.store.assign('A4',roll,0)
        self.assertEqual(next(s for s in self.store.snapshot()['spools'] if s['uuid']==roll)['remaining_mg'],150000)
    def test_native_sync_conflict_preserves_ha_consumption_and_order_projection(self):
        snap=self.store.snapshot()
        bundle={'schema_version':8,'tables':{'spools':[],'spool_identifiers':[], 'stock_events':[],
            'customers':[dict(id='customer',name='Demo customer')],
            'customer_orders':[dict(id='order',customer_id='customer',title='Bracket',currency='EUR',status='active')],
            'print_jobs':[],'allocations':[],'job_identifiers':[], 'inventory_settings':[], 'print_job_manual_overrides':[]}}
        out=self.store.dispatch('native_sync',dict(bundle=bundle,revision=snap['revision']))
        self.assertEqual(out['orders'][0]['title'],'Bracket')
        roll=snap['spools'][0]['uuid']; self.store.assign('A1',roll,0)
        job=self.store.start_job('cube',[dict(slot='A1',weight_mg=20000)],'cube')
        self.store.finish_job(job['uuid'],'completed',None,'estimated')
        with self.assertRaises(Conflict): self.store.dispatch('native_sync',dict(bundle=out['native_bundle'],revision=out['revision']))
        final=self.store.snapshot()
        native=final['native_bundle']['tables']
        self.assertEqual(sum(e['delta_mg'] for e in native['stock_events'] if e['spool_id']==roll),780000)
        self.assertEqual(next(j for j in native['print_jobs'] if j['id']==job['uuid'])['state'],'completed')
        self.assertEqual(next(a for a in native['allocations'] if a['job_id']==job['uuid'])['actual_weight_mg'],20000)

class SyncRecoveryTests(unittest.TestCase):
    def test_lost_native_ack_replays_once_after_later_ha_change(self):
        import copy
        with tempfile.TemporaryDirectory() as tmp:
            store=Store(Path(tmp)/'ack.db'); snap=store.snapshot()
            bundle={'schema_version':8,'tables':{k:[] for k in ('inventory_settings','spools','spool_identifiers','customers','customer_orders','print_jobs','allocations','job_identifiers','stock_events','print_job_manual_overrides')}}
            request=dict(bundle=bundle,revision=snap['revision'])
            accepted=store.dispatch('native_sync',request)
            store.assign('A1',snap['spools'][0]['uuid'],0)
            replay=store.dispatch('native_sync',request)
            self.assertEqual(replay['revision'],accepted['revision']+1)
            self.assertEqual(replay['slots'][0]['spool_uuid'],snap['spools'][0]['uuid'])
            bad=copy.deepcopy(request);bad['bundle']['tables']['customers']=[dict(id='bad',name='other')]
            with self.assertRaises(Conflict):store.dispatch('native_sync',bad)

    def test_separate_offline_jobs_keep_consumption_attribution(self):
        with tempfile.TemporaryDirectory() as tmp:
            store=Store(Path(tmp)/'history.db'); snap=store.snapshot()
            bundle={'schema_version':8,'tables':{k:[] for k in ('inventory_settings','spools','spool_identifiers','customers','customer_orders','print_jobs','allocations','job_identifiers','stock_events','print_job_manual_overrides')}}
            store.dispatch('native_sync',dict(bundle=bundle,revision=snap['revision']))
            roll=snap['spools'][0]['uuid'];store.assign('A1',roll,0)
            jobs=[]
            for i,amount in enumerate((20000,10000)):
                job=store.start_job('batch',[dict(slot='A1',weight_mg=amount)],str(i));jobs.append(job['uuid'])
                store.finish_job(job['uuid'],'completed',None,'estimated')
            for _ in range(2):
                events=store.snapshot()['native_bundle']['tables']['stock_events']
                usage=[e for e in events if e.get('job_id') in jobs]
                self.assertEqual(len(usage),2)
                self.assertEqual({e['job_id']:e['delta_mg'] for e in usage},dict(zip(jobs,(-20000,-10000))))
                self.assertEqual(sum(e['delta_mg'] for e in events if e['spool_id']==roll),770000)


    def test_native_roundtrip_preserves_measured_failed_settlement(self):
        with tempfile.TemporaryDirectory() as tmp:
            store=Store(Path(tmp)/'provenance.db'); snap=store.snapshot()
            from custom_components.quack_material_demo.native_bridge import TABLES
            store.dispatch('native_sync',dict(bundle={'schema_version':8,'tables':{k:[] for k in TABLES}},revision=snap['revision']))
            roll=snap['spools'][0]['uuid']; store.assign('A1',roll,0)
            job=store.start_job('partial',[dict(slot='A1',weight_mg=5000)],'partial')
            store.finish_job(job['uuid'],'failed',{roll:3000},'measured')
            current=store.snapshot()
            store.dispatch('native_sync',dict(bundle=current['native_bundle'],revision=current['revision']))
            result=next(j for j in store.snapshot()['jobs'] if j['uuid']==job['uuid'])
            self.assertEqual(result['state'],'failed')
            self.assertEqual(result['settlement']['quality'],'measured')
            self.assertEqual(result['source'],'simulation')
