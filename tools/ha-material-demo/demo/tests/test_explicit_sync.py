"""Selective inventory commands run against real isolated SQLite stores."""
import copy
import json
import sqlite3
import tempfile
import unittest
import uuid
from pathlib import Path

from custom_components.quack_material_demo.native_bridge import TABLES
from custom_components.quack_material_demo.store import Conflict, Store


class ExplicitSyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / 'inventory.sqlite3'
        self.store = Store(self.path, seed_demo=False, settings={'mode': 'pilot'})
        tables = {name: [] for name in TABLES}
        self.ids = [str(uuid.uuid4()) for _ in range(31)]
        for index, ident in enumerate(self.ids):
            tables['spools'].append(dict(id=ident, name=f'Roll {index}', manufacturer='Vendor',
                material_type='PLA', filament_preset_id='Special PLA', color_hex='#112233',
                nominal_capacity_mg=1000000, diameter_mm=1.75, density_g_cm3=1.24,
                warning_mode='none', warning_value=0, material_price_per_kg_micros=20000000,
                price_currency='EUR', status='active', created_at='2025-01-01', updated_at='2025-01-01'))
            tables['stock_events'].append(dict(id=str(uuid.uuid4()), spool_id=ident,
                event_type='initial', delta_mg=500000, balance_after_mg=500000,
                operation_key='initial:' + ident, note='Imported stock', created_at='2025-01-01'))
        tables['customers'] = [dict(id=str(uuid.uuid4()), name='Historical customer')]
        tables['customer_orders'] = [dict(id=str(uuid.uuid4()), title='Historical order')]
        self.store.sync_native(dict(revision=0, bundle=dict(schema_version=8, tables=tables)))

    def tearDown(self):
        self.temp.cleanup()

    def request(self, changes, key='explicit-1'):
        return dict(revision=self.store.snapshot()['revision'], confirmed=True,
                    request_key=key, changes=changes)

    def change(self, index=0, **fields):
        native = self.store.snapshot()['native_bundle']['tables']['spools'][index]
        return dict(spool_uuid=self.ids[index], fields=fields,
                    expected={key: native[key] for key in fields})

    def apply(self, request):
        return self.store.dispatch('native_apply', request)

    def dump(self):
        with self.store.connection() as db:
            return list(db.iterdump())

    def test_two_of_31_updates_leave_29_and_history_unchanged(self):
        before = self.store.snapshot()
        result = self.apply(self.request([self.change(name='Reviewed name'), self.change(1, color_hex='#FFEEDD')]))
        self.assertEqual(len(result['spools']), 31)
        self.assertEqual(result['spools'][0]['product'], 'Reviewed name')
        self.assertEqual(result['spools'][1]['color'], '#FFEEDD')
        self.assertEqual(result['spools'][2:], before['spools'][2:])
        old = before['native_bundle']['tables']; new = result['native_bundle']['tables']
        self.assertEqual(new['spools'][2:], old['spools'][2:])
        for table in TABLES:
            if table != 'spools':
                self.assertEqual(new[table], old[table], table)

    def test_profile_only_preserves_measured_stock_and_history(self):
        self.store.dispatch('weigh', dict(spool_uuid=self.ids[0], remaining_mg=470000))
        before = self.store.snapshot()
        result = self.apply(self.request([self.change(filament_preset_id='New exact PLA')]))
        self.assertEqual(result['spools'][0]['remaining_mg'], 470000)
        self.assertEqual(result['spools'][0]['weight_quality'], 'measured')
        self.assertEqual(result['native_bundle']['tables']['stock_events'], before['native_bundle']['tables']['stock_events'])
        self.assertEqual(result['spools'][0]['material_preset'], 'New exact PLA')

    def test_conflicting_second_row_rolls_back_batch_and_receipt(self):
        second = self.change(1, name='Second')
        second['expected']['name'] = 'Outdated'
        request = self.request([self.change(name='First'), second])
        before = self.dump()
        with self.assertRaises(Conflict):
            self.apply(request)
        self.assertEqual(self.dump(), before)

    def test_replay_after_later_change_returns_receipt_without_reapplying(self):
        request = self.request([self.change(name='First result')])
        accepted = self.apply(request)
        self.apply(self.request([self.change(name='Later result')], key='later'))
        before = self.dump()
        reopened = Store(self.path, seed_demo=False, settings={'mode': 'pilot'})
        replay = reopened.dispatch('native_apply', request)
        self.assertEqual(replay['spools'][0]['product'], 'Later result')
        self.assertGreater(replay['revision'], accepted['revision'])
        self.assertEqual(self.dump(), before)
        changed = copy.deepcopy(request); changed['changes'][0]['fields']['name'] = 'Different'
        with self.assertRaises(Conflict):
            self.apply(changed)

    def test_creation_preserves_uuid_and_explicit_archive_restore(self):
        ident = str(uuid.uuid4())
        fields = dict(name='New roll', manufacturer='Maker', material_type='PETG',
            filament_preset_id='', color_hex='#FFFFFF', nominal_capacity_mg=1000000,
            diameter_mm=1.75, density_g_cm3=1.27, status='active')
        result = self.apply(self.request([dict(spool_uuid=ident, create=True, fields=fields,
            expected={}, remaining_mg=650000, quality='measured')]))
        self.assertEqual(result['spools'][-1]['uuid'], ident)
        self.assertEqual(result['spools'][-1]['weight_quality'], 'measured')
        self.store.assign('A1', ident, 0)
        archived = self.apply(self.request([dict(spool_uuid=ident, fields={'status': 'archived'},
            expected={'status': 'active'})], key='archive'))
        self.assertEqual(archived['slots'][0]['spool_uuid'], None)
        self.assertEqual(archived['spools'][-1]['remaining_mg'], 650000)
        restored = self.apply(self.request([dict(spool_uuid=ident, fields={'status': 'active'},
            expected={'status': 'archived'})], key='restore'))
        self.assertEqual(restored['spools'][-1]['uuid'], ident)
        self.assertEqual(restored['spools'][-1]['status'], 'active')
        self.assertEqual(restored['native_bundle']['tables']['stock_events'], archived['native_bundle']['tables']['stock_events'])

    def test_stock_correction_has_provenance_and_no_double_booking(self):
        change = self.change(); change.update(remaining_mg=450000, expected_remaining_mg=500000, quality='measured')
        request = self.request([change])
        result = self.apply(request)
        events = result['native_bundle']['tables']['stock_events']
        self.assertEqual(len(events), 32)
        self.assertEqual(events[-1]['delta_mg'], -50000)
        self.assertIn('measured', events[-1]['note'])
        self.assertEqual(result['spools'][0]['weight_quality'], 'measured')
        self.assertEqual(self.apply(request)['revision'], result['revision'])

    def test_profile_only_preserves_legacy_overcapacity_without_accepting_new_invalid_stock(self):
        with self.store.connection() as db:
            row = db.execute('SELECT data FROM spools WHERE uuid=?', (self.ids[0],)).fetchone()
            data = json.loads(row['data']); data['nominal_mg'] = 250000
            db.execute('UPDATE spools SET data=? WHERE uuid=?', (json.dumps(data), self.ids[0]))
        result = self.apply(self.request([self.change(filament_preset_id='Legacy exact profile')]))
        self.assertEqual(result['spools'][0]['remaining_mg'], 500000)
        self.assertEqual(result['spools'][0]['nominal_mg'], 250000)
        change = self.change(); change.update(remaining_mg=490000, expected_remaining_mg=500000, quality='measured')
        before = self.dump()
        with self.assertRaises(ValueError):
            self.apply(self.request([change], key='invalid-stock'))
        self.assertEqual(self.dump(), before)

    def test_field_preconditions_merge_unrelated_rows_and_same_row_fields(self):
        request = self.request([self.change(filament_preset_id='Chosen profile')])
        request['concurrency'] = 'fields'
        self.store.dispatch('edit', dict(spool_uuid=self.ids[1], product='Other roll changed'))
        self.store.dispatch('edit', dict(spool_uuid=self.ids[0], color='#AABBCC'))
        result = self.apply(request)
        self.assertEqual(result['spools'][0]['material_preset'], 'Chosen profile')
        self.assertEqual(result['spools'][0]['color'], '#AABBCC')
        self.assertEqual(result['spools'][1]['product'], 'Other roll changed')

    def test_field_preconditions_reject_same_field_and_stock_conflicts_atomically(self):
        for stock in (False, True):
            change = self.change(name='Rejected')
            if stock:
                change.update(remaining_mg=450000, expected_remaining_mg=500000, quality='measured')
            request = self.request([self.change(1, name='Must roll back'), change], key='conflict-'+str(stock))
            request['concurrency'] = 'fields'
            if stock:
                self.store.dispatch('weigh', dict(spool_uuid=self.ids[0], remaining_mg=490000))
            else:
                self.store.dispatch('edit', dict(spool_uuid=self.ids[0], product='Concurrent winner'))
            before = self.dump()
            with self.assertRaises(Conflict): self.apply(request)
            self.assertEqual(self.dump(), before)

    def test_compact_receipt_replay_survives_restart_and_rejects_changed_request(self):
        request = self.request([self.change(name='Committed')])
        self.apply(request)
        with self.store.connection() as db:
            receipt = db.execute('SELECT payload,result FROM receipts WHERE request_key=?', (request['request_key'],)).fetchone()
        self.assertLess(len(receipt['payload']) + len(receipt['result']), 256)
        self.assertNotIn('Committed', receipt['payload'])
        reopened = Store(self.path, seed_demo=False, settings={'mode':'pilot'})
        self.assertEqual(reopened.dispatch('native_apply', request)['spools'][0]['product'], 'Committed')
        changed = copy.deepcopy(request); changed['changes'][0]['fields']['name'] = 'Forgery'
        with self.assertRaises(Conflict): reopened.dispatch('native_apply', changed)

    def test_legacy_raw_receipt_is_replayed_without_mutation(self):
        request = self.request([self.change(name='Legacy commit')])
        accepted = self.apply(request)
        with self.store.connection() as db:
            db.execute('UPDATE receipts SET payload=?,result=? WHERE request_key=?',
                (json.dumps(dict(action='native_apply', data=request), sort_keys=True), json.dumps(accepted), request['request_key']))
        self.apply(self.request([self.change(name='Newer')], key='newer'))
        before = self.dump()
        self.assertEqual(self.apply(request), accepted)
        self.assertEqual(self.dump(), before)

    def test_transport_ack_can_be_added_to_legacy_replay_without_new_mutation(self):
        request=self.request([self.change(name='Legacy acknowledged')])
        accepted=self.apply(request)
        with self.store.connection() as db:
            db.execute('UPDATE receipts SET payload=?,result=? WHERE request_key=?',
                (json.dumps(dict(action='native_apply',data=request),sort_keys=True),json.dumps(accepted),request['request_key']))
        self.apply(self.request([self.change(name='Later')],key='later'))
        request['response']='ack'
        before=self.dump()
        ack=self.apply(request)
        self.assertEqual(set(ack),{'accepted','request_key','accepted_revision','revision'})
        self.assertTrue(ack['accepted'])
        self.assertEqual(ack['accepted_revision'],accepted['revision'])
        self.assertGreater(ack['revision'],ack['accepted_revision'])
        self.assertEqual(self.dump(),before)

    def test_compact_ack_first_commit_and_snapshot_replay_share_identity(self):
        request=self.request([self.change(name='Ack first')]);request['response']='ack'
        ack=self.apply(request)
        self.assertTrue(ack['accepted'])
        request.pop('response')
        before=self.dump()
        self.assertEqual(self.apply(request)['spools'][0]['product'],'Ack first')
        self.assertEqual(self.dump(),before)

    def test_profile_content_and_association_commit_atomically(self):
        from test_profiles import profile
        from custom_components.quack_material_demo.profiles import read_profile
        payload=profile()
        change=self.change(filament_preset_id=payload['name'])
        change.update(material_profile=payload,expected_profile_sha256=None)
        self.apply(self.request([change]))
        with self.store.connection() as db:
            self.assertEqual(read_profile(db,self.ids[0],payload['sha256']),payload)
        changed=profile('1.03')
        change=self.change(filament_preset_id=changed['name'],color_hex='#ABCDEF')
        change.update(material_profile=changed,expected_profile_sha256='0'*64)
        before=self.dump()
        with self.assertRaises(Conflict):self.apply(self.request([change],key='wrong-profile-baseline'))
        self.assertEqual(self.dump(),before)

    def test_later_roll_conflict_rolls_back_profile_payload_and_association(self):
        from test_profiles import profile
        payload=profile()
        first=self.change(filament_preset_id=payload['name'])
        first.update(material_profile=payload,expected_profile_sha256=None)
        second=self.change(1,name='Second');second['expected']['name']='Outdated'
        before=self.dump()
        with self.assertRaises(Conflict):self.apply(self.request([first,second]))
        self.assertEqual(self.dump(),before)

    def test_receipt_storage_failure_rolls_back_inventory_and_revision(self):
        with self.store.connection() as db:
            db.execute("CREATE TRIGGER reject_receipt BEFORE INSERT ON receipts WHEN NEW.request_key='explicit-1' BEGIN SELECT RAISE(ABORT, 'Injected disk failure'); END")
        before = self.dump()
        with self.assertRaises(sqlite3.IntegrityError):
            self.apply(self.request([self.change(name='Must roll back')]))
        self.assertEqual(self.dump(), before)

    def test_empty_store_accepts_first_creation_without_migration_upload(self):
        empty = Store(Path(self.temp.name) / 'empty.sqlite3', seed_demo=False)
        ident = str(uuid.uuid4())
        result = empty.dispatch('native_apply', dict(revision=0, request_key='first', confirmed=True,
            changes=[dict(spool_uuid=ident, create=True, expected={}, fields=dict(name='First',
                manufacturer='Maker', material_type='PLA', color_hex='#AABBCC'),
                remaining_mg=50000, quality='estimated')]))
        self.assertEqual(result['spools'][0]['uuid'], ident)
        self.assertEqual(result['native_bundle']['tables']['spools'][0]['id'], ident)
        self.assertEqual(len(result['native_bundle']['tables']['stock_events']), 1)

    def test_archived_roll_stock_requires_separate_explicit_restore(self):
        self.apply(self.request([self.change(status='archived')]))
        change = self.change(); change.update(remaining_mg=600000, expected_remaining_mg=500000, quality='measured')
        before = self.dump()
        with self.assertRaises(Conflict):
            self.apply(self.request([change], key='stock'))
        self.assertEqual(self.dump(), before)

    def test_warning_fields_persist_without_stock_events(self):
        before = self.store.snapshot()['native_bundle']['tables']['stock_events']
        result = self.apply(self.request([self.change(warning_mode='percent', warning_value=2000)]))
        roll = result['native_bundle']['tables']['spools'][0]
        self.assertEqual(roll['warning_mode'], 'percent')
        self.assertEqual(roll['warning_value'], 2000)
        self.assertEqual(result['native_bundle']['tables']['stock_events'], before)

    def test_native_positive_dimensions_and_optional_manufacturer_are_preserved(self):
        result = self.apply(self.request([self.change(diameter_mm=0.05, density_g_cm3=11.5, manufacturer='')]))
        native = result['native_bundle']['tables']['spools'][0]
        self.assertEqual(native['diameter_mm'], 0.05)
        self.assertEqual(native['density_g_cm3'], 11.5)
        self.assertEqual(native['manufacturer'], '')

    def test_native_warning_units_and_nominal_capacity_mapping(self):
        result = self.apply(self.request([self.change(warning_mode='grams', warning_value=25000,
            nominal_capacity_mg=750000)]))
        native = result['native_bundle']['tables']['spools'][0]
        self.assertEqual(native['warning_value'], 25000)
        self.assertEqual(native['nominal_capacity_mg'], 750000)
        self.assertEqual(result['spools'][0]['nominal_mg'], 750000)
        self.assertEqual(result['spools'][0]['warning_value'], 25000)

    def test_percent_warning_and_integer_validation_roll_back(self):
        for fields in [dict(warning_mode='percent', warning_value=10001), dict(warning_value=1.5),
                       dict(warning_value=True)]:
            before = self.dump()
            with self.assertRaises(ValueError):
                self.apply(self.request([self.change(**fields)]))
            self.assertEqual(self.dump(), before)

    def test_coupled_status_stock_changes_are_atomic_and_preserve_normal_price(self):
        change = self.change(status='empty', material_price_per_kg_micros=20000000)
        change.update(remaining_mg=0, expected_remaining_mg=500000, quality='measured')
        self.store.assign('A1', self.ids[0], 0)
        request = self.request([change])
        result = self.apply(request)
        self.assertEqual(result['spools'][0]['status'], 'empty')
        self.assertEqual(result['spools'][0]['remaining_mg'], 0)
        self.assertEqual(result['slots'][0]['spool_uuid'], None)
        refill = self.change(status='active')
        refill.update(remaining_mg=300000, expected_remaining_mg=0, quality='estimated')
        result = self.apply(self.request([refill], key='refill'))
        self.assertEqual(result['spools'][0]['status'], 'active')
        self.assertEqual(result['spools'][0]['remaining_mg'], 300000)
        self.assertEqual(result['spools'][0]['material_price_per_kg_micros'], 20000000)
        contradictory = self.change(status='empty')
        contradictory.update(remaining_mg=250000, expected_remaining_mg=300000, quality='measured')
        before = self.dump()
        with self.assertRaises(ValueError):
            self.apply(self.request([self.change(1, name='Must roll back'), contradictory], key='contradiction'))
        self.assertEqual(self.dump(), before)

    def test_concurrent_global_revision_rejects_whole_coupled_batch(self):
        stock = self.change(status='empty')
        stock.update(remaining_mg=0, expected_remaining_mg=500000, quality='measured')
        request = self.request([self.change(1, name='Must stay unchanged'), stock])
        self.store.dispatch('edit', dict(spool_uuid=self.ids[2], product='Concurrent change'))
        before = self.dump()
        with self.assertRaises(Conflict):
            self.apply(request)
        self.assertEqual(self.dump(), before)

    def test_stale_revision_and_stock_baseline_are_conflicts(self):
        request = self.request([self.change(name='Wrong')]); request['revision'] -= 1
        with self.assertRaises(Conflict): self.apply(request)
        change = self.change(); change.update(remaining_mg=1, expected_remaining_mg=1, quality='estimated')
        with self.assertRaises(Conflict): self.apply(self.request([change]))

    def test_unsettled_stock_and_archive_rejected_profile_preserves_job(self):
        self.store.assign('A1', self.ids[0], 0)
        self.store.start_job('Active job', [{'slot': 'A1', 'weight_mg': 1000}], 'job')
        before = self.store.snapshot()
        for change in [self.change(status='archived'), dict(self.change(), remaining_mg=499000,
                           expected_remaining_mg=500000, quality='measured')]:
            with self.assertRaises(Conflict): self.apply(self.request([change]))
        result = self.apply(self.request([self.change(filament_preset_id='New profile')]))
        self.assertEqual(result['jobs'], before['jobs'])
        self.assertEqual(result['native_bundle']['tables']['allocations'], before['native_bundle']['tables']['allocations'])

    def test_invalid_requests_have_no_partial_effects(self):
        good = self.request([self.change(name='Valid')])
        invalid = []
        for key, value in [('confirmed', False), ('confirmed', 1), ('changes', []),
                           ('changes', [self.change()] * 101), ('request_key', ''), ('revision', True)]:
            item = copy.deepcopy(good); item[key] = value; invalid.append(item)
        item = copy.deepcopy(good); del item['confirmed']; invalid.append(item)
        for change in [dict(spool_uuid=self.ids[0], fields={'id': 'replace'}, expected={'id': self.ids[0]}),
                       dict(spool_uuid='invalid', fields={'name': 'X'}, expected={'name': 'Roll 0'}),
                       dict(spool_uuid=self.ids[0], fields={'name': 'X'}, expected={}),
                       dict(self.change(), remaining_mg=-1, expected_remaining_mg=500000, quality='measured'),
                       dict(self.change(), remaining_mg=400000, expected_remaining_mg=500000),
                       self.change(color_hex='white'), self.change(status='empty'),
                       self.change(density_g_cm3=True), self.change(nominal_capacity_mg=1000),
                       self.change(warning_mode='invalid')]:
            invalid.append(self.request([change]))
        invalid.append(self.request([self.change(name='A'), self.change(name='B')]))
        for request in invalid:
            with self.subTest(request=request):
                before = self.dump()
                with self.assertRaises(ValueError): self.apply(request)
                self.assertEqual(self.dump(), before)


if __name__ == '__main__':
    unittest.main()
