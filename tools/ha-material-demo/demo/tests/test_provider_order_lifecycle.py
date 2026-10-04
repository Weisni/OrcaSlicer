"""HA reservations retain native order identity and financial history atomically."""
import copy
import sqlite3
import unittest
import uuid
from unittest.mock import patch

import test_provider as fixtures
from custom_components.quack_material_demo.store import Conflict, Store


class ProviderOrderLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.ProviderTests()
        self.fixture.setUp()
        self.store = self.fixture.store
        self.customer = str(uuid.uuid4())
        self.order = str(uuid.uuid4())
        self.store.provider_delta(dict(request_key='customer-and-order', revision=self.store.snapshot()['revision'], changes=[
            dict(table='customers', before=None, after=dict(id=self.customer, name='Test customer')),
            dict(table='customer_orders', before=None, after=dict(id=self.order, customer_id=self.customer,
                title='Two-print order', order_number='ORDER-42', currency='EUR', status='active', archived=0,
                quoted_price_micros=90000000, invoice_amount_micros=77000000,
                design_time_seconds=1800, design_hourly_rate_micros=45000000,
                other_cost_micros=1500000, discount_basis_points=1250,
                bill_material=1, bill_electricity=1, bill_machine_wear=1,
                bill_maintenance=0, bill_repair_reserve=1, bill_design=1, bill_other=0))]))
        self.order_row = copy.deepcopy(self.tables()['customer_orders'][0])

    def tearDown(self):
        self.fixture.tearDown()

    def tables(self):
        return self.store.snapshot()['native_bundle']['tables']

    def reservation(self, key='reserve-order', order=True, amount=10000):
        job = str(uuid.uuid4())
        allocation = str(uuid.uuid4())
        return dict(request_key=key, revision=self.store.snapshot()['revision'], changes=[
            dict(table='print_jobs', before=None, after=dict(id=job, idempotency_key=key,
                job_name=key, project_path='project.3mf', printer_id='P2S-test', state='reserved',
                customer_order_id=self.order if order else None, cost_currency='EUR',
                estimated_runtime_seconds=1800, machine_power_watts=200,
                electricity_price_per_kwh_micros=400000, machine_wear_per_hour_micros=2000000,
                maintenance_per_hour_micros=600000, repair_reserve_per_hour_micros=300000)),
            dict(table='allocations', before=None, after=dict(id=allocation, job_id=job,
                spool_id=self.fixture.roll, filament_index=2, estimated_weight_mg=amount,
                material_price_per_kg_micros=20000000, cost_currency='EUR',
                estimated_material_cost_micros=amount*20, spool_name='Real PLA', manufacturer='Maker',
                material_type='PLA', filament_preset_id='PLA exact', color_hex='#FFFFFF'))])

    def close_order(self, status='completed', archived=0):
        before = self.tables()['customer_orders'][0]
        after = {**before, 'status':status, 'archived':archived}
        self.store.provider_delta(dict(request_key='close-'+status+'-'+str(archived),
            revision=self.store.snapshot()['revision'], changes=[dict(table='customer_orders',before=before,after=after)]))

    def complete(self, request, start, finish):
        job = request['changes'][0]['after']['id']
        slot = self.store.snapshot()['slots'][0]
        self.store.provider_job(dict(request_key='prepare-'+job, command='prepare_print', job_uuid=job,
            data=dict(printer_id='P2S-test', allocations=[dict(filament_index=2,
                spool_uuid=self.fixture.roll, slot='A1', revision=slot['revision'])])))
        self.store.provider_job(dict(request_key='accept-'+job, command='dispatch_result', job_uuid=job,
            data=dict(outcome='accepted')))
        name = request['changes'][0]['after']['job_name']
        with patch('custom_components.quack_material_demo.store.now', return_value=start):
            self.store.observe_printer('running', name)
        with patch('custom_components.quack_material_demo.store.now', return_value=finish), \
                patch('custom_components.quack_material_demo.provider.stamp', return_value=finish):
            self.store.observe_printer('finish', name)
        self.store.observe_printer('idle', name)
        return job

    def test_two_completed_prints_keep_order_cost_inputs_and_exact_allocation_identity(self):
        first = self.reservation()
        self.store.provider_delta(first)
        job1 = self.complete(first, '2026-10-03T10:00:00+00:00', '2026-10-03T10:30:00+00:00')
        second = self.reservation('reserve-second', amount=20000)
        self.store.provider_delta(second)
        job2 = self.complete(second, '2026-10-03T11:00:00+00:00', '2026-10-03T12:00:00+00:00')
        tables = self.tables()
        self.assertEqual(tables['customer_orders'], [self.order_row])
        jobs = {job['id']:job for job in tables['print_jobs']}
        for ident, runtime in ((job1,1800),(job2,3600)):
            job = jobs[ident]
            self.assertEqual(job['customer_order_id'], self.order)
            self.assertEqual(job['state'], 'completed')
            self.assertEqual(job['actual_runtime_seconds'], runtime)
            for field, value in [('machine_power_watts',200),('electricity_price_per_kwh_micros',400000),
                                 ('machine_wear_per_hour_micros',2000000),('maintenance_per_hour_micros',600000),
                                 ('repair_reserve_per_hour_micros',300000)]:
                self.assertEqual(job[field], value)
        self.assertEqual(sum(j['electricity_cost_micros'] for j in jobs.values()),120000)
        self.assertEqual(sum(j['machine_wear_cost_micros'] for j in jobs.values()),3000000)
        self.assertEqual(sum(j['maintenance_cost_micros'] for j in jobs.values()),900000)
        self.assertEqual(sum(j['repair_reserve_cost_micros'] for j in jobs.values()),450000)
        allocations = {row['id']:row for row in tables['allocations']}
        for request, amount, cost in ((first,10000,200000),(second,20000,400000)):
            source = request['changes'][1]['after']
            row = allocations[source['id']]
            for field in ('job_id','spool_id','filament_index','filament_preset_id','material_price_per_kg_micros'):
                self.assertEqual(row[field], source[field])
            self.assertEqual(row['actual_weight_mg'], amount)
            self.assertEqual(row['actual_material_cost_micros'], cost)
        snapshot = self.store.snapshot()
        self.assertEqual(snapshot['spools'][0]['remaining_mg'],470000)
        self.assertEqual(snapshot['spools'][0]['reserved_mg'],0)
        self.assertTrue(all(j['customer_order_uuid']==self.order for j in snapshot['jobs']))
        before = self.fixture.dump()
        self.store.provider_delta(first)
        self.store.provider_delta(second)
        self.store.observe_printer('finish','reserve-second')
        self.store.observe_printer('idle','reserve-second')
        self.assertEqual(self.fixture.dump(), before)
        self.assertEqual(len([e for e in tables['stock_events'] if e['event_type']=='consumption']),2)

    def test_reordered_native_rows_keep_settled_quantities_on_exact_filament_allocation(self):
        request=self.fixture.request('second-roll')
        second=str(uuid.uuid4())
        spool=copy.deepcopy(request['bundle']['tables']['spools'][0]);spool.update(id=second,name='Second PLA')
        request['bundle']['tables']['spools'].append(spool)
        request['bundle']['tables']['stock_events'].append(dict(id=str(uuid.uuid4()),spool_id=second,
            job_id=None,allocation_id=None,event_type='initial',delta_mg=500000,balance_after_mg=500000,
            operation_key='second-stock',note='',created_at='2026-01-01'))
        self.store.provider_apply(request);self.store.assign('A2',second,0)
        request=self.reservation();self.store.provider_delta(request)
        job=request['changes'][0]['after']['id']
        allocation=copy.deepcopy(request['changes'][1]['after'])
        allocation.update(id=str(uuid.uuid4()),spool_id=second,filament_index=7,estimated_weight_mg=20000,
            estimated_material_cost_micros=400000)
        self.store.provider_delta(dict(request_key='second-allocation',revision=self.store.snapshot()['revision'],
            changes=[dict(table='allocations',before=None,after=allocation)]))
        reordered=self.fixture.request('reorder-only');reordered['bundle']['tables']['allocations'].reverse()
        self.store.provider_apply(reordered)
        slots={s['id']:s for s in self.store.snapshot()['slots']}
        self.store.provider_job(dict(request_key='two-roll-prepare',command='prepare_print',job_uuid=job,
            data=dict(printer_id='P2S-test',allocations=[
                dict(filament_index=2,spool_uuid=self.fixture.roll,slot='A1',revision=slots['A1']['revision']),
                dict(filament_index=7,spool_uuid=second,slot='A2',revision=slots['A2']['revision'])])))
        self.store.observe_printer('running','reserve-order');self.store.observe_printer('finish','reserve-order')
        rows={a['filament_index']:a for a in self.tables()['allocations']}
        self.assertEqual(rows[2]['actual_weight_mg'],10000)
        self.assertEqual(rows[7]['actual_weight_mg'],20000)
        self.assertEqual(rows[2]['actual_material_cost_micros'],200000)
        self.assertEqual(rows[7]['actual_material_cost_micros'],400000)
        self.assertEqual(rows[2]['id'],request['changes'][1]['after']['id'])
        self.assertEqual(rows[7]['id'],allocation['id'])
        before=self.store.snapshot()['native_bundle']
        self.store.observe_printer('finish','reserve-order')
        self.assertEqual(self.store.snapshot()['native_bundle'],before)

    def test_personal_print_remains_explicitly_unassigned(self):
        request = self.reservation(order=False)
        self.store.provider_delta(request)
        self.complete(request,'2026-10-03T10:00:00+00:00','2026-10-03T10:30:00+00:00')
        self.assertIsNone(self.tables()['print_jobs'][0]['customer_order_id'])
        self.assertIsNone(self.store.snapshot()['jobs'][0].get('customer_order_uuid'))
        self.assertEqual(self.tables()['customer_orders'],[self.order_row])

    def test_concurrent_closed_order_rejects_new_reservation_without_receipt_or_stock_changes(self):
        for status, archived in [('completed',0),('cancelled',0),('active',1)]:
            with self.subTest(status=status,archived=archived):
                request = self.reservation(key='stale-'+status)
                self.close_order(status,archived)
                before = self.fixture.dump()
                with self.assertRaises(Conflict):
                    self.store.provider_delta(request)
                self.assertEqual(self.fixture.dump(),before)

    def test_concurrent_order_currency_change_rejects_new_reservation(self):
        request = self.reservation()
        order = self.tables()['customer_orders'][0]
        self.store.provider_delta(dict(request_key='change-currency',revision=self.store.snapshot()['revision'],
            changes=[dict(table='customer_orders',before=order,after={**order,'currency':'USD'})]))
        before = self.fixture.dump()
        with self.assertRaises(Conflict):
            self.store.provider_delta(request)
        self.assertEqual(self.fixture.dump(),before)

    def test_existing_closed_order_link_does_not_block_unrelated_edits_or_request_replay(self):
        request = self.reservation()
        accepted = self.store.provider_delta(request)
        self.complete(request,'2026-10-03T10:00:00+00:00','2026-10-03T10:30:00+00:00')
        self.close_order('completed',1)
        before = self.fixture.dump()
        replay = self.store.provider_delta(request)
        self.assertEqual(replay['accepted_revision'],accepted['accepted_revision'])
        self.assertEqual(self.fixture.dump(),before)
        customer = self.tables()['customers'][0]
        self.store.provider_delta(dict(request_key='customer-note',revision=self.store.snapshot()['revision'],
            changes=[dict(table='customers',before=customer,after={**customer,'notes':'History retained'})]))
        self.assertEqual(self.tables()['print_jobs'][0]['customer_order_id'],self.order)

    def test_reassignment_to_closed_order_is_rejected_but_existing_link_can_remain(self):
        request = self.reservation(order=False)
        self.store.provider_delta(request)
        self.close_order('completed')
        job = self.tables()['print_jobs'][0]
        before = self.fixture.dump()
        with self.assertRaises(Conflict):
            self.store.provider_delta(dict(request_key='move-to-closed',revision=self.store.snapshot()['revision'],
                changes=[dict(table='print_jobs',before=job,after={**job,'customer_order_id':self.order})]))
        self.assertEqual(self.fixture.dump(),before)

    def test_remotely_archived_source_order_blocks_stale_native_reassignment(self):
        request=self.reservation();self.store.provider_delta(request)
        self.complete(request,'2026-10-03T10:00:00+00:00','2026-10-03T10:30:00+00:00')
        job=self.tables()['print_jobs'][0]
        revision=self.store.snapshot()['revision']
        self.close_order('completed',1)
        before=self.fixture.dump()
        with self.assertRaises(Conflict):
            self.store.provider_delta(dict(request_key='stale-unassign',revision=revision,
                changes=[dict(table='print_jobs',before=job,after={**job,'customer_order_id':None})]))
        self.assertEqual(self.fixture.dump(),before)

    def test_lost_ack_replay_after_reopen_keeps_one_order_reservation(self):
        request = self.reservation()
        self.store.provider_delta(request)  # Simulate the caller losing the successful response.
        self.store = Store(self.store.path,seed_demo=False,settings=self.store.settings)
        before = self.fixture.dump()
        self.assertTrue(self.store.provider_delta(request)['accepted'])
        self.assertEqual(self.fixture.dump(),before)
        self.assertEqual(len(self.tables()['print_jobs']),1)
        self.assertEqual(self.store.snapshot()['spools'][0]['reserved_mg'],10000)
        self.assertEqual(self.tables()['print_jobs'][0]['customer_order_id'],self.order)

    def test_receipt_failure_rolls_back_job_order_link_and_reservation(self):
        request = self.reservation()
        with self.store.connection() as db:
            db.execute("CREATE TRIGGER reject_receipt BEFORE INSERT ON receipts WHEN NEW.request_key='reserve-order' BEGIN SELECT RAISE(ABORT, 'disk failure'); END")
        before = self.fixture.dump()
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.provider_delta(request)
        self.assertEqual(self.fixture.dump(),before)
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM joblinks').fetchone()[0],0)


if __name__ == '__main__':
    unittest.main()
