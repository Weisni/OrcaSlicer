import tempfile,unittest,uuid,copy
from pathlib import Path
from custom_components.quack_material_demo.store import Store,Conflict
from custom_components.quack_material_demo.native_bridge import TABLES

class PilotTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.path=Path(self.tmp.name)/'pilot.db'
    def tearDown(self):self.tmp.cleanup()
    def store(self):return Store(self.path,seed_demo=False,settings={'mode':'pilot','base_url':'http://homeassistant.local:8123'})
    def bundle(self):
        t={k:[] for k in TABLES}
        for material,preset in [('PLA','legacy-id'),('ASA+','')]:
            ident=str(uuid.uuid4())
            t['spools'].append(dict(id=ident,name=material,manufacturer='Vendor',material_type=material,color_hex='#FFFFFF',filament_preset_id=preset,diameter_mm=1.75,density_g_cm3=1.24,nominal_capacity_mg=1000000,status='active'))
            t['stock_events'].append(dict(id=str(uuid.uuid4()),spool_id=ident,delta_mg=286088,operation_key=ident,event_type='initial',job_id=None))
        return dict(schema_version=8,tables=t)
    def test_empty_pilot_and_standalone_customer_order(self):
        s=self.store();self.assertEqual(s.snapshot()['spools'],[])
        c=s.dispatch('customer',dict(name='Example',contact_name='Person',email='example@invalid.test',request_key='c'))
        self.assertEqual(s.dispatch('customer',dict(name='Example',contact_name='Person',email='example@invalid.test',request_key='c'))['uuid'],c['uuid'])
        o=s.dispatch('order',dict(title='Bracket',customer_id=c['uuid'],order_number='2026-1',request_key='o'))
        self.assertEqual(s.management_snapshot()['orders'][0]['customer_id'],c['uuid'])
        self.assertEqual(s.management_snapshot()['customers'][0]['email'],'example@invalid.test')
    def test_import_preserves_unknown_profiles_materials_and_idempotency(self):
        s=self.store(); b=self.bundle(); before=copy.deepcopy(b)
        s.import_inventory(b,'hash1',{'legacy-id':'Actual PLA @P2S'})
        self.assertEqual(b,before)
        data=s.management_snapshot();self.assertEqual(len(data['spools']),2)
        self.assertEqual(data['spools'][0]['remaining_mg'],286088)
        self.assertEqual(data['spools'][0]['material_preset'],'Actual PLA @P2S')
        self.assertEqual(data['spools'][1]['material_type'],'ASA+')
        self.assertFalse(data['spools'][1]['material_preset'])
        self.assertIsNone(data['spools'][1]['bambu_material'])
        self.assertEqual(len(data['stock_events']),2)
        s.import_inventory(b,'hash1',{});self.assertEqual(len(s.snapshot()['spools']),2)
        with self.assertRaises(Conflict):s.import_inventory(b,'other',{})
        s.assign('A1',b['tables']['spools'][1]['id'],0)
        self.assertEqual(s.snapshot()['slots'][0]['spool_uuid'],b['tables']['spools'][1]['id'])
        self.assertEqual(s.snapshot()['spools'][1]['material_preset'],'')

    def test_profile_optional_roll_creation_edit_and_assignment(self):
        s=self.store()
        request=dict(product='AURAPOL PETG',manufacturer='AURAPOL',material_type='PETG',color='#004455',remaining_mg=960647)
        r=s.dispatch('create',request)['uuid']
        self.assertEqual(s.snapshot()['spools'][0]['material_preset'],'')
        s.assign('A1',r,0)
        s.dispatch('edit',dict(spool_uuid=r,material_preset='Special PETG',revision=s.snapshot()['revision']))
        s.dispatch('edit',dict(spool_uuid=r,material_preset='',revision=s.snapshot()['revision']))
        current=s.snapshot()['spools'][0]
        self.assertEqual(current['material_preset'],'')
        self.assertEqual(current['remaining_mg'],960647)
        self.assertEqual(current['manufacturer'],'AURAPOL')
    def test_observed_attempt_reconcile_same_job_without_double_count(self):
        s=self.store();b=self.bundle();s.import_inventory(b,'hash1',{'legacy-id':'PLA'});roll=b['tables']['spools'][0]['id'];s.assign('A1',roll,0)
        s.observe_printer('running','test');s.observe_printer('finish','test')
        j=s.snapshot()['jobs'][0]['uuid']
        request=dict(job_uuid=j,spool_uuid=roll,slot='A1',consumed_mg=3000,outcome='completed',quality='measured',revision=s.snapshot()['revision'],request_key='reconcile')
        s.dispatch('reconcile_observed',request);s.dispatch('reconcile_observed',request)
        data=s.management_snapshot();self.assertEqual(data['spools'][0]['remaining_mg'],283088);self.assertEqual(len(data['jobs']),1)
        self.assertEqual(data['jobs'][0]['settlement']['quality'],'measured')
        self.assertEqual(data['jobs'][0]['source'],'printer_observation')
    def test_stale_management_write_and_archive_links(self):
        s=self.store();c=s.dispatch('customer',dict(name='Example',revision=0,request_key='c'))
        with self.assertRaises(Conflict):s.dispatch('customer',dict(uuid=c['uuid'],name='Stale',revision=0,request_key='x'))
        s.dispatch('customer',dict(uuid=c['uuid'],archive=True,revision=s.snapshot()['revision'],request_key='a'))
        with self.assertRaises(Conflict):s.dispatch('order',dict(customer_id=c['uuid'],title='Bad',request_key='o'))
    def test_edit_roll_and_profile_conflict(self):
        s=self.store();b=self.bundle();s.import_inventory(b,'h',{'legacy-id':'PLA'});r=b['tables']['spools'][0]['id']
        s.dispatch('edit',dict(spool_uuid=r,color='#2255AA',manufacturer='Changed',density_g_cm3=1.26,material_price_per_kg_micros=22000000,revision=s.snapshot()['revision'],request_key='edit'))
        row=s.snapshot()['spools'][0];self.assertEqual(row['color'],'#2255AA');self.assertEqual(row['material_price_per_kg_micros'],22000000)
        with self.assertRaises(Conflict):s.dispatch('edit',dict(spool_uuid=r,manufacturer='stale',revision=0,request_key='stale'))

    def test_historical_allocation_metadata_and_timestamps_are_preserved(self):
        b=self.bundle();roll=b['tables']['spools'][0]['id'];job=str(uuid.uuid4());timestamp='2025-01-02T12:00:00Z'
        b['tables']['print_jobs']=[dict(id=job,job_name='History',state='completed',created_at=timestamp,updated_at=timestamp,completed_at=timestamp)]
        allocation=dict(id=str(uuid.uuid4()),job_id=job,spool_id=roll,estimated_weight_mg=1000,actual_weight_mg=1000,
            spool_name='Original roll name',manufacturer='Historical vendor',material_type='PLA',filament_preset_id='historical-profile',
            color_hex='#123456',material_price_per_kg_micros=12300000,estimated_material_cost_micros=12300,cost_currency='EUR')
        b['tables']['allocations']=[allocation]
        s=self.store();s.import_inventory(b,'h',{'legacy-id':'PLA'})
        out=s.snapshot()['native_bundle']['tables']
        self.assertEqual(out['allocations'][0],allocation)
        self.assertEqual(out['print_jobs'][0]['completed_at'],timestamp)
        self.assertEqual(out['print_jobs'][0]['updated_at'],timestamp)

    def test_measured_corrections_keep_each_event_and_original_time(self):
        s=self.store();b=self.bundle();s.import_inventory(b,'h',{'legacy-id':'PLA'});roll=b['tables']['spools'][0]['id']
        for n,amount in enumerate((250000,200000)):
            s.dispatch('weigh',dict(spool_uuid=roll,remaining_mg=amount,revision=s.snapshot()['revision'],request_key='weigh-'+str(n)))
        first=s.management_snapshot()['stock_events'];second=s.management_snapshot()['stock_events']
        corrections=[e for e in first if e.get('operation_key','').startswith('ha-lifecycle:')]
        self.assertEqual(len(corrections),2)
        self.assertEqual([e['delta_mg'] for e in corrections],[-36088,-50000])
        self.assertTrue(all('measured' in e['note'] for e in corrections))
        self.assertEqual(first,second)

    def test_observed_completion_survives_an_intermediate_quack_sync(self):
        s=self.store();b=self.bundle();s.import_inventory(b,'h',{'legacy-id':'PLA'});roll=b['tables']['spools'][0]['id']
        s.observe_printer('running','test');snapshot=s.snapshot()
        s.sync_native(dict(bundle=snapshot['native_bundle'],revision=snapshot['revision']))
        s.observe_printer('finish','test');j=s.snapshot()['jobs'][0]['uuid']
        s.reconcile_observed(dict(job_uuid=j,spool_uuid=roll,consumed_mg=1000,outcome='completed',quality='measured',revision=s.snapshot()['revision'],request_key='done'))
        native=s.snapshot()['native_bundle']['tables']['print_jobs'][0]
        self.assertTrue(native['completed_at'])

    def test_new_roll_rejects_negative_material_price(self):
        s=self.store()
        with self.assertRaises(ValueError):s.dispatch('create',dict(product='PLA',manufacturer='Example',material_type='PLA',material_preset='PLA',color='#FFFFFF',remaining_mg=1000,material_price_per_kg_micros=-1000000))

    def test_explicit_zero_observed_usage_roundtrips_without_fabricated_stock(self):
        s=self.store();b=self.bundle();s.import_inventory(b,'h',{'legacy-id':'PLA'});roll=b['tables']['spools'][0]['id']
        s.observe_printer('prepare','test');s.observe_printer('failed','test');j=s.snapshot()['jobs'][0]['uuid']
        s.reconcile_observed(dict(job_uuid=j,spool_uuid=roll,consumed_mg=0,outcome='failed',quality='measured',revision=s.snapshot()['revision'],request_key='zero'))
        snapshot=s.snapshot();s.sync_native(dict(bundle=snapshot['native_bundle'],revision=snapshot['revision']))
        self.assertEqual(s.snapshot()['spools'][0]['remaining_mg'],286088)
        self.assertEqual(s.snapshot()['jobs'][0]['state'],'failed')
