"""Provider requests exercise real transactions without printer commands."""
import copy
import json
import tempfile
import unittest
import uuid
from pathlib import Path

from custom_components.quack_material_demo.native_bridge import TABLES
from custom_components.quack_material_demo.store import Store, Conflict


class ProviderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name)/'provider.sqlite3', seed_demo=False,
            settings={'mode':'pilot', 'provider_printer_id':'P2S-test'})
        self.roll = str(uuid.uuid4())
        tables = {t:[] for t in TABLES}
        tables['spools'] = [dict(id=self.roll,name='Real PLA',manufacturer='Maker',material_type='PLA',
            filament_preset_id='PLA exact',color_hex='#FFFFFF',diameter_mm=1.75,density_g_cm3=1.24,
            nominal_capacity_mg=1000000,status='active',warning_mode='none',warning_value=0,
            material_price_per_kg_micros=20000000,price_currency='EUR',created_at='2026-01-01',updated_at='2026-01-01')]
        tables['stock_events'] = [dict(id=str(uuid.uuid4()),spool_id=self.roll,job_id=None,allocation_id=None,
            event_type='initial',delta_mg=500000,balance_after_mg=500000,operation_key='seed',note='',created_at='2026-01-01')]
        self.store.sync_native({'revision':0,'bundle':{'schema_version':8,'tables':tables}})
        self.store.assign('A1',self.roll,0)

    def tearDown(self):
        self.temp.cleanup()

    def request(self, key='provider-1'):
        snapshot=self.store.snapshot()
        return dict(request_key=key,revision=snapshot['revision'],before=snapshot['native_bundle'],bundle=copy.deepcopy(snapshot['native_bundle']))

    def dump(self):
        with self.store.connection() as db:return list(db.iterdump())

    def reserve(self):
        request=self.request('reserve')
        self.job=str(uuid.uuid4()); self.allocation=str(uuid.uuid4())
        request['bundle']['tables']['print_jobs'].append(dict(id=self.job,idempotency_key='reserve-key',job_name='Test plate',
            printer_id='P2S-test',state='reserved',created_at='2026-01-01',updated_at='2026-01-01'))
        request['bundle']['tables']['allocations'].append(dict(id=self.allocation,job_id=self.job,spool_id=self.roll,
            filament_index=0,estimated_weight_mg=10000,actual_weight_mg=None,filament_preset_id='PLA exact',color_hex='#FFFFFF'))
        return self.store.dispatch('provider_apply',request)

    def job_request(self, command, data, key=None):
        return dict(request_key=key or command,command=command,job_uuid=self.job,data=data)

    def prepare(self):
        slot=self.store.snapshot()['slots'][0]
        return self.store.dispatch('provider_job',self.job_request('prepare_print',dict(printer_id='P2S-test',
            allocations=[dict(filament_index=0,spool_uuid=self.roll,slot='A1',revision=slot['revision'])])))

    def test_mutation_is_atomic_and_replay_returns_current_authority(self):
        request=self.request();request['bundle']['tables']['spools'][0]['name']='Renamed'
        result=self.store.dispatch('provider_apply',request)
        self.assertEqual(result['spools'][0]['product'],'Renamed')
        self.store.dispatch('weigh',dict(spool_uuid=self.roll,remaining_mg=480000))
        before=self.dump()
        replay=self.store.dispatch('provider_apply',request)
        self.assertEqual(replay['spools'][0]['remaining_mg'],480000)
        self.assertEqual(before,self.dump())
        request['bundle']['tables']['spools'][0]['name']='Changed replay'
        with self.assertRaises(Conflict):self.store.dispatch('provider_apply',request)

    def test_provider_receipts_are_bounded_and_legacy_requests_still_replay(self):
        request=self.request();request['bundle']['tables']['spools'][0]['name']='Compact commit'
        accepted=self.store.provider_apply(request)
        with self.store.connection() as db:
            row=db.execute('SELECT payload,result FROM receipts WHERE request_key=?',(request['request_key'],)).fetchone()
            self.assertLess(len(row['payload'])+len(row['result']),256)
        self.assertTrue(self.store.provider_request_recorded('provider_apply',request))
        with self.store.connection() as db:
            db.execute('UPDATE receipts SET payload=? WHERE request_key=?',
                (json.dumps(dict(action='provider_apply',data=request),sort_keys=True),request['request_key']))
        before=self.dump()
        self.assertEqual(self.store.provider_apply(request)['revision'],accepted['revision'])
        self.assertEqual(self.dump(),before)

    def test_job_lite_response_replays_legacy_identity_without_large_history(self):
        self.reserve();result=self.prepare()
        slot=self.store.snapshot()['slots'][0]
        request=self.job_request('prepare_print',dict(printer_id='P2S-test',
            allocations=[dict(filament_index=0,spool_uuid=self.roll,slot='A1',revision=slot['revision'])]))
        with self.store.connection() as db:
            db.execute('UPDATE receipts SET payload=? WHERE request_key=?',
                (json.dumps(dict(action='provider_job',data=request),sort_keys=True),request['request_key']))
        request['response']='provider'
        before=self.dump()
        lite=self.store.provider_job(request)
        self.assertEqual(lite['provider_job'],result['provider_job'])
        for key in ('native_bundle','jobs','orders'):self.assertNotIn(key,lite)
        self.assertTrue(self.store.provider_request_recorded('provider_job',request))
        self.assertEqual(self.dump(),before)

    def delta(self, request, key='delta-1'):
        before=request['before']['tables']['spools'][0]
        after=request['bundle']['tables']['spools'][0]
        return dict(request_key=key,revision=request['revision'],changes=[dict(table='spools',before=before,after=after)])

    def test_delta_merges_other_rows_and_replays_without_reapplying(self):
        request=self.request();request['bundle']['tables']['spools'][0]['name']='Delta winner'
        delta=self.delta(request)
        self.store.dispatch('customer',dict(name='Unrelated concurrent customer'))
        result=self.store.provider_delta(delta)
        self.assertTrue(result['accepted'])
        snapshot=self.store.snapshot()
        self.assertEqual(snapshot['spools'][0]['product'],'Delta winner')
        self.assertEqual(snapshot['native_bundle']['tables']['customers'][0]['name'],'Unrelated concurrent customer')
        self.store.dispatch('edit',dict(spool_uuid=self.roll,product='Later winner'))
        before=self.dump()
        reopened=Store(self.store.path,seed_demo=False,settings=self.store.settings)
        replay=reopened.provider_delta(delta)
        self.assertEqual(replay['accepted_revision'],result['accepted_revision'])
        self.assertGreater(replay['revision'],result['revision'])
        self.assertEqual(reopened.snapshot()['spools'][0]['product'],'Later winner')
        self.assertEqual(self.dump(),before)
        delta['changes'][0]['after']['name']='Reused key'
        with self.assertRaises(Conflict):self.store.provider_delta(delta)

    def test_delta_rejects_same_row_conflict_and_rolls_back_entire_batch(self):
        request=self.request();request['bundle']['tables']['spools'][0]['name']='Stale'
        delta=self.delta(request)
        delta['changes'].insert(0,dict(table='customers',before=None,after=dict(id=str(uuid.uuid4()),name='Must roll back')))
        self.store.dispatch('edit',dict(spool_uuid=self.roll,color='#123456'))
        before=self.dump()
        with self.assertRaises(Conflict):self.store.provider_delta(delta)
        self.assertEqual(self.dump(),before)

    def test_delta_does_not_allow_history_deletion_or_invalid_stock(self):
        for deletion in (False,True):
            snapshot=self.store.snapshot();event=snapshot['native_bundle']['tables']['stock_events'][0]
            changed=copy.deepcopy(event);changed['balance_after_mg']=1
            request=dict(request_key='bad-'+str(deletion),revision=snapshot['revision'],
                changes=[dict(table='stock_events',before=event,after=None if deletion else changed)])
            before=self.dump()
            with self.assertRaises((Conflict,ValueError)):self.store.provider_delta(request)
            self.assertEqual(self.dump(),before)

    def test_delta_accepts_small_edit_with_more_than_2000_history_rows(self):
        with self.store.connection() as db:
            bundle=json.loads(db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()['data'])
            for index in range(2001):
                bundle['tables']['stock_events'].append(dict(id=str(uuid.uuid4()),spool_id=self.roll,
                    job_id=None,allocation_id=None,event_type='adjustment',delta_mg=0,balance_after_mg=500000,
                    operation_key='history:'+str(index),note='',created_at='2026-01-01'))
            db.execute("UPDATE bridge SET data=? WHERE id='native'",(json.dumps(bundle),))
        request=self.request();request['bundle']['tables']['spools'][0]['name']='Large inventory edit'
        result=self.store.provider_delta(self.delta(request))
        self.assertTrue(result['accepted'])
        self.assertEqual(len(self.store.snapshot()['native_bundle']['tables']['stock_events']),2002)
        with self.assertRaises(ValueError):self.store.provider_apply(self.request('legacy-limit'))

    def test_delta_stock_booking_is_once_only_and_receipt_failure_rolls_back(self):
        snapshot=self.store.snapshot()
        event=dict(id=str(uuid.uuid4()),spool_id=self.roll,job_id=None,allocation_id=None,
            event_type='adjustment',delta_mg=-50000,balance_after_mg=450000,
            operation_key='delta-stock',note='Explicit correction',created_at='2026-01-02')
        request=dict(request_key='delta-stock',revision=snapshot['revision'],
            changes=[dict(table='stock_events',before=None,after=event)])
        result=self.store.provider_delta(request)
        self.store.provider_delta(request)
        self.assertEqual(self.store.snapshot()['spools'][0]['remaining_mg'],450000)
        self.assertEqual(len(self.store.snapshot()['native_bundle']['tables']['stock_events']),2)
        with self.store.connection() as db:
            db.execute("CREATE TRIGGER reject_delta BEFORE INSERT ON receipts WHEN NEW.request_key='disk-failure' BEGIN SELECT RAISE(ABORT, 'disk failure'); END")
        request=self.request();request['bundle']['tables']['spools'][0]['name']='Never committed'
        before=self.dump()
        import sqlite3
        with self.assertRaises(sqlite3.IntegrityError):self.store.provider_delta(self.delta(request,'disk-failure'))
        self.assertEqual(self.dump(),before)
        self.assertEqual(result['accepted_revision'],result['revision'])

    def test_delta_reservation_is_validated_against_current_stock(self):
        job=str(uuid.uuid4());allocation=str(uuid.uuid4())
        changes=[dict(table='print_jobs',before=None,after=dict(id=job,idempotency_key='delta-reserve',
            job_name='Delta plate',printer_id='P2S-test',state='reserved')),
            dict(table='allocations',before=None,after=dict(id=allocation,job_id=job,spool_id=self.roll,
                filament_index=0,estimated_weight_mg=10000,actual_weight_mg=None,
                filament_preset_id='PLA exact',color_hex='#FFFFFF'))]
        request=dict(request_key='delta-reserve',revision=self.store.snapshot()['revision'],changes=changes)
        self.store.dispatch('weigh',dict(spool_uuid=self.roll,remaining_mg=9000))
        before=self.dump()
        with self.assertRaises(Conflict):self.store.provider_delta(request)
        self.assertEqual(self.dump(),before)
        self.store.dispatch('weigh',dict(spool_uuid=self.roll,remaining_mg=11000))
        self.assertTrue(self.store.provider_delta(request)['accepted'])
        self.assertEqual(self.store.snapshot()['spools'][0]['available_mg'],1000)

    def test_delta_identity_duplicate_and_future_revision_are_rejected(self):
        request=self.request();request['bundle']['tables']['spools'][0]['name']='Renamed'
        delta=self.delta(request)
        cases=[]
        duplicate=copy.deepcopy(delta);duplicate['changes']*=2;cases.append(duplicate)
        moved=copy.deepcopy(delta);moved['changes'][0]['after']['id']=str(uuid.uuid4());cases.append(moved)
        future=copy.deepcopy(delta);future['revision']+=100;cases.append(future)
        before=self.dump()
        for item in cases:
            with self.assertRaises((Conflict,ValueError)):self.store.provider_delta(item)
            self.assertEqual(self.dump(),before)

    def terminal_review(self, terminal='failed'):
        self.reserve();self.prepare()
        self.store.observe_printer('running','Test plate')
        self.store.observe_printer(terminal,'Test plate')

    def review_request(self, amount=2500, key='review-1'):
        return dict(request_key=key,revision=self.store.snapshot()['revision'],confirmed=True,
            job_uuid=self.job,outcome='failed',quality='measured',consumption={self.roll:amount})

    def test_known_failed_provider_review_books_explicit_amount_once_and_releases_reservation(self):
        self.terminal_review()
        request=self.review_request()
        before=self.store.snapshot()
        self.assertEqual(before['spools'][0]['reserved_mg'],10000)
        accepted=self.store.reconcile_provider(request)
        after=self.store.snapshot()
        self.assertEqual(after['spools'][0]['remaining_mg'],497500)
        self.assertEqual(after['spools'][0]['reserved_mg'],0)
        self.assertEqual(after['spools'][0]['weight_quality'],'measured')
        self.assertEqual(after['jobs'][0]['allocations'],before['jobs'][0]['allocations'])
        self.assertEqual(after['jobs'][0]['state'],'failed')
        dump=self.dump()
        reopened=Store(self.store.path,seed_demo=False,settings=self.store.settings)
        self.assertEqual(reopened.reconcile_provider(request),accepted)
        self.assertEqual(self.dump(),dump)
        changed=copy.deepcopy(request);changed['consumption'][self.roll]=0
        with self.assertRaises(Conflict):self.store.reconcile_provider(changed)

    def test_provider_review_rejects_uncertain_dispatch_and_idle_only_end(self):
        self.reserve();self.prepare()
        self.store.provider_job(self.job_request('dispatch_result',{'outcome':'uncertain'}))
        before=self.dump()
        with self.assertRaises(Conflict):self.store.reconcile_provider(self.review_request())
        self.assertEqual(self.dump(),before)
        self.store.observe_printer('running','Test plate')
        self.store.observe_printer('idle','Test plate')
        before=self.dump()
        with self.assertRaises(Conflict):self.store.reconcile_provider(self.review_request())
        self.assertEqual(self.dump(),before)

    def test_provider_review_requires_every_known_roll_and_explicit_provenance(self):
        self.terminal_review()
        for edit in ({'consumption':{}},{'consumption':{self.roll:-1}},
                     {'consumption':{self.roll:0,str(uuid.uuid4()):0}},
                     {'quality':'unknown'},{'confirmed':False}):
            request=self.review_request();request.update(edit)
            before=self.dump()
            with self.assertRaises((ValueError,Conflict)):self.store.reconcile_provider(request)
            self.assertEqual(self.dump(),before)
        self.store.reconcile_provider(self.review_request(0))
        self.assertEqual(self.store.snapshot()['spools'][0]['remaining_mg'],500000)
        self.assertEqual(self.store.snapshot()['spools'][0]['reserved_mg'],0)

    def test_provider_review_receipt_failure_and_other_reservations_preserve_stock(self):
        self.terminal_review()
        self.store.start_job('Other allocation',[dict(slot='A1',weight_mg=490000)],'other-job')
        before=self.dump()
        with self.assertRaises(Conflict):self.store.reconcile_provider(self.review_request(20000))
        self.assertEqual(self.dump(),before)
        import sqlite3
        with self.store.connection() as db:
            db.execute("CREATE TRIGGER reject_review BEFORE INSERT ON receipts WHEN NEW.request_key='review-1' BEGIN SELECT RAISE(ABORT, 'disk failure'); END")
        before=self.dump()
        with self.assertRaises(sqlite3.IntegrityError):self.store.reconcile_provider(self.review_request(1000))
        self.assertEqual(self.dump(),before)

    def test_stale_or_forged_baseline_does_not_write(self):
        request=self.request();request['before']['tables']['spools'][0]['name']='Forged'
        before=self.dump()
        with self.assertRaises(Conflict):self.store.dispatch('provider_apply',request)
        self.assertEqual(before,self.dump())
        request=self.request();self.store.dispatch('weigh',dict(spool_uuid=self.roll,remaining_mg=480000))
        with self.assertRaises(Conflict):self.store.dispatch('provider_apply',request)

    def test_history_and_roll_omissions_cannot_replace_authority(self):
        for table in ('spools','stock_events'):
            with self.subTest(table=table):
                request=self.request();request['bundle']['tables'][table]=[];before=self.dump()
                with self.assertRaises((ValueError,Conflict)):self.store.dispatch('provider_apply',request)
                self.assertEqual(before,self.dump())
        request=self.request();request['bundle']['tables']['stock_events'][0]['delta_mg']=499000
        with self.assertRaises(Conflict):self.store.dispatch('provider_apply',request)

    def test_stock_ledger_requires_conservation_and_protects_provenance(self):
        self.store.dispatch('weigh',dict(spool_uuid=self.roll,remaining_mg=480000))
        request=self.request();request['bundle']['tables']['spools'][0]['color_hex']='#112233'
        result=self.store.dispatch('provider_apply',request)
        self.assertEqual(result['spools'][0]['weight_quality'],'measured')
        request=self.request('bad-ledger');event=copy.deepcopy(request['bundle']['tables']['stock_events'][0])
        event.update(id=str(uuid.uuid4()),operation_key='bad',delta_mg=-1000,balance_after_mg=470000)
        request['bundle']['tables']['stock_events'].append(event)
        with self.assertRaises((ValueError,Conflict)):self.store.dispatch('provider_apply',request)

    def test_reserved_job_prepares_only_exact_printer_and_current_binding(self):
        self.reserve();result=self.prepare()
        self.assertEqual(result['provider_job']['status'],'prepared')
        request=self.job_request('prepare_print',dict(printer_id='other',allocations=[]),'wrong')
        with self.assertRaises(Conflict):self.store.dispatch('provider_job',request)
        self.assertEqual(self.store.snapshot()['spools'][0]['reserved_mg'],10000)

    def test_accepted_dispatch_is_adopted_by_observer_without_second_job(self):
        self.reserve();self.prepare()
        self.store.dispatch('provider_job',self.job_request('dispatch_result',{'outcome':'accepted'}))
        self.store.observe_printer('running','Test plate')
        snapshot=self.store.snapshot()
        self.assertEqual(len(snapshot['jobs']),1)
        self.assertEqual(snapshot['jobs'][0]['uuid'],self.job)
        self.store.observe_printer('finish','Test plate')
        snapshot=self.store.snapshot()
        self.assertEqual(snapshot['jobs'][0]['state'],'completed')
        self.assertEqual(snapshot['spools'][0]['remaining_mg'],490000)
        self.assertEqual(snapshot['jobs'][0]['settlement']['quality'],'estimated')
        self.store.observe_printer('finish','Test plate')
        self.assertEqual(self.store.snapshot()['spools'][0]['remaining_mg'],490000)

    def test_uncertain_dispatch_keeps_reservation_and_rejected_releases_it(self):
        self.reserve();self.prepare()
        self.store.dispatch('provider_job',self.job_request('dispatch_result',{'outcome':'uncertain'}))
        self.assertEqual(self.store.snapshot()['spools'][0]['reserved_mg'],10000)
        with self.assertRaises(Conflict):
            self.store.dispatch('provider_job',self.job_request('dispatch_result',{'outcome':'rejected'},'later-reject'))

    def test_receipt_failure_rolls_back_entire_provider_mutation(self):
        with self.store.connection() as db:
            db.execute("CREATE TRIGGER reject_provider BEFORE INSERT ON receipts WHEN NEW.request_key='provider-1' BEGIN SELECT RAISE(ABORT, 'disk failure'); END")
        request=self.request();request['bundle']['tables']['spools'][0]['name']='Never committed'
        before=self.dump()
        with self.assertRaises(Exception):self.store.dispatch('provider_apply',request)
        self.assertEqual(before,self.dump())

    def test_observer_start_before_transport_ack_adopts_prepared_job(self):
        self.reserve();prepared=self.prepare()
        self.store.observe_printer('running','Test plate')
        self.assertEqual(len(self.store.snapshot()['jobs']),1)
        self.store.dispatch('provider_job',self.job_request('dispatch_result',{'outcome':'accepted'}))
        self.assertEqual(self.store.snapshot()['jobs'][0]['state'],'printing')
        slot=self.store.snapshot()['slots'][0]
        replay=self.store.dispatch('provider_job',self.job_request('prepare_print',dict(printer_id='P2S-test',
            allocations=[dict(filament_index=0,spool_uuid=self.roll,slot='A1',revision=slot['revision'])])))
        self.assertEqual(replay['provider_job']['status'],'observing')

    def test_failed_provider_print_retains_unknown_consumption_and_reservation(self):
        self.reserve();self.prepare()
        self.store.dispatch('provider_job',self.job_request('dispatch_result',{'outcome':'accepted'}))
        self.store.observe_printer('running','Test plate');self.store.observe_printer('failed','Test plate')
        snapshot=self.store.snapshot()
        self.assertEqual(snapshot['jobs'][0]['state'],'needs_review')
        self.assertIsNone(snapshot['jobs'][0]['settlement'])
        self.assertEqual(snapshot['spools'][0]['remaining_mg'],500000)
        self.assertEqual(snapshot['spools'][0]['reserved_mg'],10000)

    def test_repeated_roll_allocations_settle_once_and_project_distinct_quantities(self):
        self.reserve()
        request=self.request('second-index');a=copy.deepcopy(request['bundle']['tables']['allocations'][0])
        a.update(id=str(uuid.uuid4()),filament_index=1,estimated_weight_mg=20000)
        request['bundle']['tables']['allocations'].append(a)
        self.store.dispatch('provider_apply',request)
        slot=self.store.snapshot()['slots'][0]
        self.store.dispatch('provider_job',self.job_request('prepare_print',dict(printer_id='P2S-test',allocations=[
            dict(filament_index=i,spool_uuid=self.roll,slot='A1',revision=slot['revision']) for i in (0,1)])))
        self.store.observe_printer('running','Test plate');self.store.observe_printer('finish','Test plate')
        snapshot=self.store.snapshot()
        self.assertEqual(snapshot['spools'][0]['remaining_mg'],470000)
        allocations=snapshot['native_bundle']['tables']['allocations']
        self.assertEqual([a['actual_weight_mg'] for a in allocations],[10000,20000])

    def test_zero_estimate_cannot_shift_allocation_projection(self):
        self.reserve()
        request=self.request('zero-first-index')
        first=request['bundle']['tables']['allocations'][0]
        second=copy.deepcopy(first)
        first['estimated_weight_mg']=0
        second.update(id=str(uuid.uuid4()),filament_index=1,estimated_weight_mg=10000)
        request['bundle']['tables']['allocations'].append(second)
        before=self.dump()
        with self.assertRaisesRegex(ValueError,'Positive reservation required'):
            self.store.provider_apply(request)
        self.assertEqual(before,self.dump())

    def test_native_completed_quantity_and_consumption_must_match(self):
        self.reserve();request=self.request('settle')
        request['bundle']['tables']['print_jobs'][0]['state']='completed'
        request['bundle']['tables']['allocations'][0]['actual_weight_mg']=10000
        with self.assertRaises(Conflict):self.store.dispatch('provider_apply',request)
        request['bundle']['tables']['stock_events'].append(dict(id=str(uuid.uuid4()),spool_id=self.roll,
            job_id=self.job,allocation_id=self.allocation,event_type='consumption',delta_mg=-10000,
            balance_after_mg=490000,operation_key='consume:'+self.job,note='',created_at='2026-01-01'))
        result=self.store.dispatch('provider_apply',request)
        self.assertEqual(result['spools'][0]['remaining_mg'],490000)
        changed=self.request('tamper-settled');changed['bundle']['tables']['allocations'][0]['actual_weight_mg']=20000
        with self.assertRaises(Conflict):self.store.dispatch('provider_apply',changed)

    def test_settled_jobs_reject_added_allocations_and_forged_history_links(self):
        self.reserve();self.prepare();self.store.observe_printer('running','Test plate');self.store.observe_printer('finish','Test plate')
        request=self.request('inject-settled-allocation');a=copy.deepcopy(request['bundle']['tables']['allocations'][0])
        a.update(id=str(uuid.uuid4()),filament_index=1)
        request['bundle']['tables']['allocations'].append(a)
        with self.assertRaises(Conflict):self.store.dispatch('provider_apply',request)

    def test_duplicate_job_request_keys_and_unknown_customer_links_are_rejected(self):
        self.reserve();request=self.request('duplicate-job')
        j=copy.deepcopy(request['bundle']['tables']['print_jobs'][0]);j['id']=str(uuid.uuid4())
        request['bundle']['tables']['print_jobs'].append(j)
        with self.assertRaises((ValueError,Conflict)):self.store.dispatch('provider_apply',request)
        request=self.request('bad-order');request['bundle']['tables']['customer_orders'].append(dict(id=str(uuid.uuid4()),customer_id='missing',title='No customer',currency='EUR',status='active'))
        with self.assertRaises((ValueError,Conflict)):self.store.dispatch('provider_apply',request)

    def test_prepare_rejects_reserved_job_for_other_printer(self):
        self.reserve();request=self.request('wrong-device')
        request['bundle']['tables']['print_jobs'][0]['printer_id']='another-printer'
        self.store.dispatch('provider_apply',request)
        with self.assertRaises(Conflict):self.prepare()

    def test_explicit_discard_closes_pending_dispatch_without_fabricated_consumption(self):
        self.reserve();self.prepare()
        self.store.dispatch('provider_job',self.job_request('dispatch_result',{'outcome':'uncertain'}))
        request=self.request('discard');request['bundle']['tables']['print_jobs'][0]['state']='discarded'
        result=self.store.dispatch('provider_apply',request)
        self.assertEqual(result['spools'][0]['reserved_mg'],0)
        self.assertEqual(result['spools'][0]['remaining_mg'],500000)
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT status FROM provider_dispatch').fetchone()['status'],'terminal')

    def test_measured_failed_settlement_survives_native_cost_edit(self):
        self.reserve()
        self.store.finish_job(self.job,'failed',{self.roll:1234},'measured')
        request=self.request('cost');request['bundle']['tables']['print_jobs'][0]['machine_wear_cost_micros']=50000
        result=self.store.dispatch('provider_apply',request)
        self.assertEqual(result['jobs'][0]['state'],'failed')
        self.assertEqual(result['jobs'][0]['settlement']['quality'],'measured')
        self.assertEqual(result['jobs'][0]['settlement']['consumption'],{self.roll:1234})
        self.assertEqual(result['spools'][0]['remaining_mg'],498766)

    def test_unrelated_updates_preserve_legacy_stock_above_nominal_capacity(self):
        with self.store.connection() as db:
            row=db.execute('SELECT data FROM spools WHERE uuid=?',(self.roll,)).fetchone()
            metadata=json.loads(row['data']);metadata['nominal_mg']=250000
            db.execute('UPDATE spools SET data=? WHERE uuid=?',(json.dumps(metadata),self.roll))
        request=self.request();request['bundle']['tables']['spools'][0]['name']='Renamed legacy roll'
        result=self.store.dispatch('provider_apply',request)
        self.assertEqual(result['spools'][0]['remaining_mg'],500000)
        self.assertEqual(result['spools'][0]['nominal_mg'],250000)

    def test_name_sensor_lag_adopts_pending_job_without_duplicate_attempt(self):
        self.reserve();self.prepare()
        self.store.observe_printer('prepare','Previous print name')
        self.assertEqual(len(self.store.snapshot()['jobs']),1)
        self.store.observe_printer('prepare','Test plate')  # Name-only sensor update.
        self.store.observe_printer('running','Test plate')
        self.store.observe_printer('finish','Test plate')
        snapshot=self.store.snapshot()
        self.assertEqual(len(snapshot['jobs']),1)
        self.assertEqual(snapshot['jobs'][0]['state'],'completed')
        self.assertEqual(snapshot['spools'][0]['remaining_mg'],490000)
        self.assertEqual(snapshot['spools'][0]['reserved_mg'],0)

    def test_unmatched_pending_attempt_is_preserved_without_false_consumption(self):
        self.reserve();self.prepare()
        self.store.observe_printer('prepare','Other print')
        self.assertEqual(len(self.store.snapshot()['jobs']),1)
        self.store.observe_printer('finish','Other print')
        snapshot=self.store.snapshot()
        self.assertEqual(len(snapshot['jobs']),2)
        self.assertEqual(snapshot['spools'][0]['remaining_mg'],500000)
        self.assertEqual(snapshot['spools'][0]['reserved_mg'],10000)
        observed=next(j for j in snapshot['jobs'] if j['uuid']!=self.job)
        self.assertEqual(observed['state'],'needs_review')
        self.assertIsNone(observed['settlement'])
        self.store.observe_printer('finish','Other print')
        self.assertEqual(len(self.store.snapshot()['jobs']),2)

    def test_physical_tag_replacement_and_unreferenced_order_deletion(self):
        request=self.request('create-editable')
        customer=str(uuid.uuid4());order=str(uuid.uuid4())
        request['bundle']['tables']['customers'].append(dict(id=customer,name='Customer'))
        request['bundle']['tables']['customer_orders'].append(dict(id=order,customer_id=customer,title='Unreferenced',status='active',currency='EUR'))
        request['bundle']['tables']['spool_identifiers'].append(dict(kind='nfc_uid',value='00112233',spool_id=self.roll))
        self.store.dispatch('provider_apply',request)
        request=self.request('replace-physical')
        request['bundle']['tables']['spool_identifiers']=[r for r in request['bundle']['tables']['spool_identifiers'] if r['kind']!='nfc_uid']
        request['bundle']['tables']['spool_identifiers'].append(dict(kind='nfc_uid',value='11223344',spool_id=self.roll))
        request['bundle']['tables']['customer_orders']=[]
        result=self.store.dispatch('provider_apply',request)
        self.assertEqual(result['native_bundle']['tables']['customer_orders'],[])
        self.assertEqual([r['value'] for r in result['native_bundle']['tables']['spool_identifiers'] if r['kind']=='nfc_uid'],['11223344'])

    def test_null_native_fields_and_nontext_identifiers_cannot_poison_authority(self):
        self.reserve()
        for table,field in (('print_jobs','project_path'),('allocations','spool_name'),('stock_events','note')):
            with self.subTest(table=table):
                request=self.request('null-'+table);request['bundle']['tables'][table][0][field]=None
                before=self.dump()
                with self.assertRaises((ValueError,Conflict)):self.store.dispatch('provider_apply',request)
                self.assertEqual(before,self.dump())
        request=self.request('null-customer')
        request['bundle']['tables']['customers'].append(dict(id=str(uuid.uuid4()),name='Customer',contact_name=None))
        with self.assertRaises(ValueError):self.store.dispatch('provider_apply',request)
        request=self.request('bad-id')
        request['bundle']['tables']['spool_identifiers'].append(dict(kind='nfc_uid',value=123,spool_id=self.roll))
        with self.assertRaises(ValueError):self.store.dispatch('provider_apply',request)

    def test_started_allocation_cannot_move_to_another_job(self):
        self.reserve();self.prepare();self.store.observe_printer('running','Test plate')
        request=self.request('move-allocation');other=str(uuid.uuid4())
        request['bundle']['tables']['print_jobs'].append(dict(id=other,idempotency_key='other',job_name='Other',state='reserved'))
        request['bundle']['tables']['allocations'][0]['job_id']=other
        before=self.dump()
        with self.assertRaises(Conflict):self.store.dispatch('provider_apply',request)
        self.assertEqual(before,self.dump())

    def test_native_required_columns_and_positive_default_power(self):
        for field in ('warning_mode','warning_value'):
            request=self.request('missing-'+field);del request['bundle']['tables']['spools'][0][field]
            with self.assertRaises(ValueError):self.store.dispatch('provider_apply',request)
        request=self.request('bad-power')
        request['bundle']['tables']['inventory_settings']=[dict(id=1,currency='EUR',electricity_price_per_kwh_micros=400000,default_machine_power_watts=0)]
        with self.assertRaises(ValueError):self.store.dispatch('provider_apply',request)
        customer=str(uuid.uuid4())
        for field in ('currency','status'):
            request=self.request('missing-order-'+field)
            request['bundle']['tables']['customers'].append(dict(id=customer,name='Customer'))
            order=dict(id=str(uuid.uuid4()),customer_id=customer,title='Order',currency='EUR',status='active')
            del order[field];request['bundle']['tables']['customer_orders'].append(order)
            with self.assertRaises(ValueError):self.store.dispatch('provider_apply',request)
