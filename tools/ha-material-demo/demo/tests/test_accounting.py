"""Cost parity, retrospective accounting and immutable invoice snapshots."""
import copy
import sqlite3
import unittest
import uuid
from pathlib import Path

import test_provider_order_lifecycle as fixtures
from custom_components.quack_material_demo.store import Conflict


class AccountingTests(unittest.TestCase):
    def setUp(self):
        self.fixture=fixtures.ProviderOrderLifecycleTests();self.fixture.setUp()
        self.store=self.fixture.store
        self.order=self.fixture.order
        self.customer=self.fixture.customer
        self.first=self.fixture.reservation()
        self.first['changes'][0]['after']['estimated_runtime_seconds']=7200
        self.store.provider_delta(self.first)
        self.job1=self.fixture.complete(self.first,'2026-10-03T10:00:00+00:00','2026-10-03T10:30:00+00:00')
        second=self.fixture.reservation('second',amount=20000)
        self.store.provider_delta(second)
        self.job2=self.fixture.complete(second,'2026-10-03T11:00:00+00:00','2026-10-03T12:00:00+00:00')

    def tearDown(self):self.fixture.tearDown()

    def read(self,**query):
        from custom_components.quack_material_demo.accounting import accounting
        return accounting(self.store,query)

    def request(self,target=None,key='move'):
        return dict(request_key=key,revision=self.store.snapshot()['revision'],job_uuid=self.job1,
            expected_order_uuid=self.order,customer_order_uuid=target)

    def invoice(self,key='invoice',number='INV-42'):
        return dict(request_key=key,revision=self.store.snapshot()['revision'],order_uuid=self.order,
            details=dict(seller_name='Workshop',seller_address='Example Street 1',seller_contact='mail@example.test',
                tax_identifier='TEST-ID',customer_name='Test customer',customer_address='Example Road 2',
                invoice_number=number,invoice_date='2026-10-03',service_date='2026-10-03',due_date='2026-10-17',
                small_business=False,vat_basis_points=1900))

    def test_order_summary_and_invoice_lines_match_native_cost_rules(self):
        result=self.read(order_uuid=self.order)
        summary=result['summary']
        expected=dict(material_cost_micros=600000,electricity_cost_micros=120000,machine_wear_cost_micros=3000000,
            maintenance_cost_micros=900000,repair_reserve_cost_micros=450000,design_cost_micros=22500000,
            other_cost_micros=1500000,total_cost_micros=29070000,billable_subtotal_micros=26670000,
            discount_micros=3333750,calculated_invoice_micros=23336250,
            quoted_price_micros=90000000,invoice_amount_micros=77000000)
        for field,value in expected.items():self.assertEqual(summary[field],value,field)
        self.assertEqual({j['uuid'] for j in result['jobs']},{self.job1,self.job2})
        lines=result['invoice_lines']
        material=[line for line in lines if line['category']=='material']
        self.assertEqual(len(material),1)
        self.assertEqual(material[0]['detail'],'30.0 g')
        self.assertEqual(material[0]['internal_amount_micros'],600000)
        for line in lines:
            if line['category'] in ('maintenance','other'):
                self.assertFalse(line['included']);self.assertEqual(line['invoice_amount_micros'],0)
        self.assertEqual(sum(line['invoice_amount_micros'] for line in lines),23336250)
        self.assertEqual(self.read(customer_uuid=self.customer)['summary'],summary)

    def test_current_compatible_spool_price_updates_preview_but_not_saved_invoice_or_ledger(self):
        invoice=self.store.dispatch('create_invoice',self.invoice())
        before_allocations=copy.deepcopy(self.fixture.tables()['allocations'])
        spool=self.fixture.tables()['spools'][0]
        self.store.provider_delta(dict(request_key='price-correction',revision=self.store.snapshot()['revision'],
            changes=[dict(table='spools',before=spool,after={**spool,'material_price_per_kg_micros':30000000})]))
        before=self.fixture.fixture.dump()
        self.assertEqual(self.read(order_uuid=self.order)['summary']['material_cost_micros'],900000)
        self.assertEqual(self.read(invoice_uuid=invoice['id']),invoice)
        self.assertEqual(self.fixture.tables()['allocations'],before_allocations)
        self.assertEqual(self.fixture.fixture.dump(),before)

    def test_different_live_currency_keeps_booked_price_and_positive_usage_minimum_cent(self):
        spool=self.fixture.tables()['spools'][0]
        self.store.provider_delta(dict(request_key='foreign-price',revision=self.store.snapshot()['revision'],
            changes=[dict(table='spools',before=spool,after={**spool,'material_price_per_kg_micros':999000000,'price_currency':'USD'})]))
        self.assertEqual(self.read(order_uuid=self.order)['summary']['material_cost_micros'],600000)
        spool=self.fixture.tables()['spools'][0]
        self.store.provider_delta(dict(request_key='free-price',revision=self.store.snapshot()['revision'],
            changes=[dict(table='spools',before=spool,after={**spool,'material_price_per_kg_micros':0,'price_currency':'EUR'})]))
        self.assertEqual(self.read(order_uuid=self.order)['summary']['material_cost_micros'],20000)

    def test_invoice_snapshot_and_replay_identity_survive_recovery(self):
        from custom_components.quack_material_demo.recovery import export_recovery,restore_recovery
        from custom_components.quack_material_demo.accounting import accounting
        request=self.invoice();invoice=self.store.dispatch('create_invoice',request)
        recovered=restore_recovery(export_recovery(self.store),Path(self.store.path).with_name('restored.sqlite3'),settings=self.store.settings)
        self.assertEqual(accounting(recovered,{'invoice_uuid':invoice['id']}),invoice)
        self.assertEqual(recovered.dispatch('create_invoice',request),invoice)
        self.assertEqual(recovered.snapshot()['spools'],self.store.snapshot()['spools'])

    def test_personal_print_is_excluded_from_customer_cost_overview(self):
        request=self.fixture.reservation('personal',order=False)
        self.store.provider_delta(request)
        self.fixture.complete(request,'2026-10-03T13:00:00+00:00','2026-10-03T13:30:00+00:00')
        overview=self.read()
        self.assertEqual(len(overview['jobs']),3)
        self.assertEqual(overview['customers'][0]['summary']['total_cost_micros'],29070000)

    def test_completed_reassignment_preserves_stock_allocation_rates_and_replays(self):
        before=self.fixture.tables()
        stock=copy.deepcopy(self.store.snapshot()['spools'])
        request=self.request()
        result=self.store.dispatch('job_order',request)
        self.assertIsNone(result['customer_order_uuid'])
        after=self.fixture.tables()
        self.assertEqual(after['allocations'],before['allocations'])
        self.assertEqual(after['stock_events'],before['stock_events'])
        self.assertEqual(self.store.snapshot()['spools'],stock)
        first=next(j for j in after['print_jobs'] if j['id']==self.job1)
        original=next(j for j in before['print_jobs'] if j['id']==self.job1)
        for field in ('estimated_runtime_seconds','actual_runtime_seconds','started_at','completed_at','electricity_price_per_kwh_micros',
                      'machine_wear_cost_micros','maintenance_cost_micros','repair_reserve_cost_micros'):
            self.assertEqual(first[field],original[field])
        self.assertIsNone(first['customer_order_id'])
        self.assertEqual(self.read(order_uuid=self.order)['summary']['total_cost_micros'],27380000)
        self.assertEqual(after['print_job_manual_overrides'][0]['job_id'],self.job1)
        dumped=self.fixture.fixture.dump()
        self.assertEqual(self.store.dispatch('job_order',request),result)
        self.assertEqual(self.fixture.fixture.dump(),dumped)

    def test_stale_or_closed_reassignment_rolls_back_all_changes(self):
        request=self.request()
        self.fixture.close_order('completed',1)
        before=self.fixture.fixture.dump()
        with self.assertRaises(Conflict):self.store.dispatch('job_order',request)
        self.assertEqual(self.fixture.fixture.dump(),before)
        request['revision']=self.store.snapshot()['revision']
        with self.assertRaises(Conflict):self.store.dispatch('job_order',request)
        self.assertEqual(self.fixture.fixture.dump(),before)

    def test_closed_or_wrong_currency_target_rejects_historical_reassignment(self):
        for status,currency in [('completed','EUR'),('active','USD')]:
            with self.subTest(status=status,currency=currency):
                target=str(uuid.uuid4())
                self.store.provider_delta(dict(request_key='target-'+target,revision=self.store.snapshot()['revision'],
                    changes=[dict(table='customer_orders',before=None,after=dict(id=target,customer_id=self.customer,
                        title='Other order',status=status,currency=currency))]))
                before=self.fixture.fixture.dump()
                with self.assertRaises(Conflict):self.store.dispatch('job_order',self.request(target,key=target))
                self.assertEqual(self.fixture.fixture.dump(),before)

    def test_completed_reassignment_to_active_order_moves_exact_costs(self):
        target=str(uuid.uuid4())
        self.store.provider_delta(dict(request_key='active-target',revision=self.store.snapshot()['revision'],
            changes=[dict(table='customer_orders',before=None,after=dict(id=target,customer_id=self.customer,
                title='Other order',status='active',currency='EUR'))]))
        before=copy.deepcopy(self.fixture.tables())
        result=self.store.dispatch('job_order',self.request(target))
        self.assertEqual(result['customer_order_uuid'],target)
        self.assertEqual(self.read(order_uuid=target)['summary']['total_cost_micros'],1690000)
        self.assertEqual(self.read(order_uuid=self.order)['summary']['total_cost_micros'],27380000)
        after=self.fixture.tables()
        self.assertEqual(after['stock_events'],before['stock_events'])
        self.assertEqual(after['allocations'],before['allocations'])

    def test_invoice_snapshot_is_immutable_after_reassignment_and_survives_replay(self):
        request=self.invoice()
        result=self.store.dispatch('create_invoice',request)
        self.assertEqual(result['totals'],dict(internal_cost_micros=29070000,net_micros=23336250,
            tax_micros=4433888,gross_micros=27770138))
        self.store.dispatch('job_order',self.request())
        self.assertEqual(self.read(invoice_uuid=result['id']),result)
        self.assertEqual(self.store.dispatch('create_invoice',request),result)
        selected=self.read(order_uuid=self.order)
        self.assertEqual(selected['invoices'],[result])
        self.assertEqual(selected['invoice_defaults']['seller_name'],'Workshop')
        self.assertEqual(selected['summary']['invoice_amount_micros'],77000000)
        self.assertEqual(selected['summary']['total_cost_micros'],27380000)

    def test_stale_invoice_preview_and_wrong_expected_order_fail_without_writes(self):
        invoice=self.invoice()
        self.store.dispatch('job_order',self.request())
        before=self.fixture.fixture.dump()
        with self.assertRaises(Conflict):self.store.dispatch('create_invoice',invoice)
        self.assertEqual(self.fixture.fixture.dump(),before)
        with self.assertRaises(Conflict):self.store.dispatch('job_order',self.request(key='stale-reviewed-link'))
        self.assertEqual(self.fixture.fixture.dump(),before)

    def test_duplicate_number_and_missing_explicit_tax_details_do_not_write(self):
        self.store.dispatch('create_invoice',self.invoice())
        before=self.fixture.fixture.dump()
        with self.assertRaises(Conflict):self.store.dispatch('create_invoice',self.invoice('duplicate'))
        self.assertEqual(self.fixture.fixture.dump(),before)
        request=self.invoice('invalid','INV-43');del request['details']['small_business']
        with self.assertRaises(ValueError):self.store.dispatch('create_invoice',request)
        self.assertEqual(self.fixture.fixture.dump(),before)

    def test_small_business_has_no_tax_and_invoice_receipt_failure_rolls_back(self):
        request=self.invoice();request['details']['small_business']=True
        with self.store.connection() as db:
            db.execute("CREATE TRIGGER reject_invoice BEFORE INSERT ON receipts WHEN NEW.request_key='invoice' BEGIN SELECT RAISE(ABORT,'disk failure'); END")
        before=self.fixture.fixture.dump()
        with self.assertRaises(sqlite3.IntegrityError):self.store.dispatch('create_invoice',request)
        self.assertEqual(self.fixture.fixture.dump(),before)
        with self.store.connection() as db:db.execute('DROP TRIGGER reject_invoice')
        result=self.store.dispatch('create_invoice',request)
        self.assertEqual(result['totals']['tax_micros'],0)
        self.assertEqual(result['totals']['gross_micros'],23336250)

    def test_accounting_http_routes_return_snapshot_and_reject_duplicate_query_selectors(self):
        import json
        import threading
        import urllib.request
        from urllib.error import HTTPError
        from pathlib import Path
        from server import make_server
        server=make_server(Path(self.store.path),port=0,settings=self.store.settings)
        worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
        base=f'http://127.0.0.1:{server.server_port}/api/quack_material_demo/'
        try:
            selected=json.load(urllib.request.urlopen(base+'accounting?order_uuid='+self.order))
            self.assertEqual(selected['summary']['total_cost_micros'],29070000)
            request=urllib.request.Request(base+'action/create_invoice',json.dumps(self.invoice()).encode(),
                {'Content-Type':'application/json'})
            invoice=json.load(urllib.request.urlopen(request))
            self.assertEqual(json.load(urllib.request.urlopen(base+'accounting?invoice_uuid='+invoice['id'])),invoice)
            with self.assertRaises(HTTPError) as caught:
                urllib.request.urlopen(base+'accounting?order_uuid='+self.order+'&order_uuid='+self.order)
            self.assertEqual(caught.exception.code,400);caught.exception.close()
        finally:
            server.shutdown();server.server_close();worker.join()

    def test_accounting_mutations_remain_admin_only(self):
        from types import SimpleNamespace
        from custom_components.quack_material_demo.access import may_write
        admin=SimpleNamespace(is_admin=True,id='admin');sync=SimpleNamespace(is_admin=False,id='sync')
        for action in ('job_order','create_invoice'):
            self.assertTrue(may_write(admin,['sync'],action))
            self.assertFalse(may_write(sync,['sync'],action))

    def test_zero_confirmed_material_usage_remains_zero_with_native_cent_minimum(self):
        request=self.fixture.reservation('zero-failed',order=False)
        self.store.provider_delta(request);job=request['changes'][0]['after']['id']
        slot=self.store.snapshot()['slots'][0]
        self.store.provider_job(dict(request_key='zero-prepare',command='prepare_print',job_uuid=job,
            data=dict(printer_id='P2S-test',allocations=[dict(filament_index=2,spool_uuid=self.fixture.fixture.roll,
                slot='A1',revision=slot['revision'])])))
        self.store.observe_printer('running','zero-failed')
        self.store.observe_printer('failed','zero-failed')
        self.store.observe_printer('idle','zero-failed')
        self.store.reconcile_provider(dict(request_key='zero-reconcile',revision=self.store.snapshot()['revision'],
            confirmed=True,job_uuid=job,outcome='failed',quality='measured',consumption={self.fixture.fixture.roll:0}))
        summary=self.read(job_uuid=job)['summary']
        self.assertEqual(summary['material_cost_micros'],0)
        self.assertEqual(summary['actual_material_cost_micros'],0)
        self.assertEqual(summary['estimated_material_cost_micros'],200000)

    def test_mixed_currency_diagnostics_do_not_hide_other_valid_cost_rows(self):
        with self.store.connection() as db:
            import json
            row=db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
            bundle=json.loads(row['data']);bundle['tables']['customer_orders'][0]['currency']='USD'
            db.execute("UPDATE bridge SET data=? WHERE id='native'",(json.dumps(bundle),))
        overview=self.read()
        self.assertIsNone(overview['orders'][0]['summary'])
        self.assertIn('currenc',overview['orders'][0]['error'].lower())
        self.assertIsNotNone(overview['jobs'][0]['summary'])
        with self.assertRaises(Conflict):self.read(order_uuid=self.order)


if __name__=='__main__':unittest.main()
