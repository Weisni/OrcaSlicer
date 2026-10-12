"""Generation reads avoid unchanged graphs and retain immutable profile history."""
import copy
import json
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from threading import Event
from unittest.mock import patch

from custom_components.quack_material_demo import read_api
from custom_components.quack_material_demo.profiles import apply_profile_change
from custom_components.quack_material_demo.recovery import export_recovery, restore_recovery
from custom_components.quack_material_demo.store import Store
from test_profiles import profile


class IncrementalSyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / 'inventory.sqlite3'
        self.store = Store(self.path)
        self.store.sync_native(dict(revision=0, bundle=dict(schema_version=8,
            tables={table: [] for table in read_api.TABLES})))

    def tearDown(self):
        self.temp.cleanup()

    def query(self, reply):
        return dict(generation=reply['generation'], revision=str(reply['revision']))

    def edit_customers(self, rows):
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            graph = json.loads(db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()['data'])
            graph['tables']['customers'] = rows
            db.execute("UPDATE bridge SET data=? WHERE id='native'", (json.dumps(graph),))
            self.store.event(db, 'customers', {})

    def test_unchanged_revision_never_constructs_snapshot_or_native_graph(self):
        first = read_api.sync(self.store, {})
        self.assertEqual(first['sync_version'], 1)
        self.assertTrue(first['reset'])
        self.assertFalse(first['unchanged'])
        with patch.object(self.store, 'snapshot', side_effect=AssertionError('Full graph constructed')):
            current = read_api.sync(self.store, self.query(first))
        self.assertEqual(current, dict(sync_version=1, generation=first['generation'],
            revision=first['revision'], unchanged=True, reset=False))
        self.assertLess(len(json.dumps(current).encode()), 256)

    def test_reset_includes_every_table_and_all_rows_without_deletions(self):
        current = read_api.sync(self.store, {})
        self.assertEqual(set(current['native_delta']['tables']), set(read_api.TABLES))
        self.assertEqual(current['native_delta']['schema_version'], 8)
        graph = self.store.snapshot()['native_bundle']['tables']
        for table in read_api.TABLES:
            self.assertEqual(current['native_delta']['tables'][table], dict(upserts=graph[table], deletes=[]))
        self.assertNotIn('native_bundle', current['snapshot'])
        self.assertNotIn('jobs', current['snapshot'])
        self.assertEqual(current['snapshot']['revision'], current['revision'])

    def test_one_row_update_transfers_only_changed_row(self):
        first = read_api.sync(self.store, {})
        ident = first['snapshot']['spools'][0]['uuid']
        self.store.set_profile(ident, 'Changed exact profile')
        changed = read_api.sync(self.store, self.query(first))
        self.assertFalse(changed['reset'])
        self.assertGreater(changed['revision'], first['revision'])
        self.assertEqual(len(changed['native_delta']['tables']['spools']['upserts']), 1)
        self.assertEqual(changed['native_delta']['tables']['spools']['upserts'][0]['id'], ident)
        for table in read_api.TABLES:
            self.assertEqual(changed['native_delta']['tables'][table]['deletes'], [])
            if table != 'spools':self.assertEqual(changed['native_delta']['tables'][table]['upserts'], [])

    def test_deletes_use_canonical_utf8_json_array_identity(self):
        ident = 'customer-"\\-Weiß'
        self.edit_customers([dict(id=ident, name='Removed')])
        first = read_api.sync(self.store, {})
        self.edit_customers([])
        changed = read_api.sync(self.store, self.query(first))
        self.assertEqual(changed['native_delta']['tables']['customers'],
            dict(upserts=[], deletes=['["customer-\\"\\\\-Weiß"]']))

    def test_duplicate_row_identity_rejects_changed_graph_without_poisoning_history(self):
        first = read_api.sync(self.store, {})
        self.edit_customers([dict(id='duplicate', name='A'), dict(id='duplicate', name='B')])
        with self.assertRaises(ValueError):read_api.sync(self.store, self.query(first))
        self.edit_customers([dict(id='valid', name='Valid')])
        changed = read_api.sync(self.store, self.query(first))
        self.assertFalse(changed['reset'])
        self.assertEqual(changed['native_delta']['tables']['customers']['upserts'], [dict(id='valid', name='Valid')])

    def test_restart_and_generation_mismatch_force_complete_reset(self):
        first = read_api.sync(self.store, {})
        restarted = Store(self.path)
        reset = read_api.sync(restarted, self.query(first))
        self.assertNotEqual(first['generation'], reset['generation'])
        self.assertTrue(reset['reset'])
        self.assertFalse(reset['unchanged'])
        self.assertEqual(len(reset['native_delta']['tables']['spools']['upserts']), 3)
        mismatch = read_api.sync(self.store, dict(generation='another-server', revision=first['revision']))
        self.assertTrue(mismatch['reset'])

    def test_revision_rollback_rotates_generation_and_resets(self):
        first = read_api.sync(self.store, {})
        self.store.set_profile(first['snapshot']['spools'][0]['uuid'], 'Later revision')
        later = read_api.sync(self.store, self.query(first))
        with self.store.connection() as db:
            db.execute('DELETE FROM events WHERE id>?', (first['revision'],))
        rolled_back = read_api.sync(self.store, self.query(first))
        self.assertTrue(rolled_back['reset'])
        self.assertFalse(rolled_back['unchanged'])
        self.assertNotEqual(rolled_back['generation'], later['generation'])

    def test_response_mutation_cannot_modify_retained_baseline(self):
        self.edit_customers([dict(id='customer', name='Actual')])
        first = read_api.sync(self.store, {})
        first['native_delta']['tables']['customers']['upserts'][0]['name'] = 'Caller mutation'
        self.edit_customers([dict(id='customer', name='Actual')])
        changed = read_api.sync(self.store, self.query(first))
        self.assertEqual(changed['native_delta']['tables']['customers'], dict(upserts=[], deletes=[]))

    def test_expired_history_resets_older_revision_but_current_needs_no_history(self):
        from custom_components.quack_material_demo import incremental_sync
        first = read_api.sync(self.store, {})
        with patch.object(incremental_sync, 'HISTORY_REVISIONS', 2):
            for index in range(3):
                self.store.set_profile(first['snapshot']['spools'][0]['uuid'], f'Profile {index}')
                last = read_api.sync(self.store, {})
            expired = read_api.sync(self.store, self.query(first))
            self.assertTrue(expired['reset'])
        # The opaque generation still validates the client's current graph when
        # server history is discarded; only older revisions need a full reset.
        with patch.object(incremental_sync, 'HISTORY_BYTES', 1):
            with patch.object(self.store, 'snapshot', side_effect=AssertionError('Eviction reconstructed current graph')):
                evicted = read_api.sync(self.store, self.query(last))
            self.assertFalse(evicted['reset'])
            self.assertTrue(evicted['unchanged'])
            older = read_api.sync(self.store, self.query(first))
            self.assertTrue(older['reset'])
            self.assertFalse(older['unchanged'])

    def test_graph_exceeding_retention_budget_still_has_snapshot_free_unchanged_reads(self):
        from custom_components.quack_material_demo import incremental_sync
        with patch.object(incremental_sync, 'HISTORY_BYTES', 1):
            first = read_api.sync(self.store, {})
            with patch.object(self.store, 'snapshot', side_effect=AssertionError('Unretained graph reconstructed')):
                unchanged = read_api.sync(self.store, self.query(first))
        self.assertEqual(unchanged, dict(sync_version=1, generation=first['generation'],
            revision=first['revision'], unchanged=True, reset=False))

    def test_graph_row_and_response_byte_limits_reject_before_history_publication(self):
        from custom_components.quack_material_demo import incremental_sync
        first = read_api.sync(self.store, {})
        self.edit_customers([dict(id='new', name='New')])
        with patch.object(incremental_sync, 'MAX_ROWS', 1), self.assertRaises(ValueError):
            read_api.sync(self.store, self.query(first))
        with patch.object(incremental_sync, 'MAX_REPLY_BYTES', 256), self.assertRaises(ValueError):
            read_api.sync(self.store, self.query(first))
        changed = read_api.sync(self.store, self.query(first))
        self.assertFalse(changed['reset'])

    def test_native_row_larger_than_client_limit_is_rejected_without_replacing_history(self):
        first = read_api.sync(self.store, {})
        self.edit_customers([dict(id='large', name='Large', notes='x' * (256 * 1024))])
        with self.assertRaises(ValueError):read_api.sync(self.store, self.query(first))
        self.edit_customers([dict(id='small', name='Small')])
        changed = read_api.sync(self.store, self.query(first))
        self.assertFalse(changed['reset'])
        self.assertEqual(changed['native_delta']['tables']['customers']['upserts'], [dict(id='small', name='Small')])

    def test_provider_metadata_larger_than_client_cache_limit_is_rejected(self):
        first = read_api.sync(self.store, {})
        ident = first['snapshot']['spools'][0]['uuid']
        with self.store.connection() as db:
            data = json.loads(db.execute('SELECT data FROM spools WHERE uuid=?', (ident,)).fetchone()['data'])
            data['notes'] = 'x' * (4 * 1024 * 1024)
            db.execute('UPDATE spools SET data=? WHERE uuid=?', (json.dumps(data), ident))
            self.store.event(db, 'metadata', {})
        with self.assertRaises(ValueError):read_api.sync(self.store, self.query(first))
        with self.store.connection() as db:
            del data['notes']
            db.execute('UPDATE spools SET data=? WHERE uuid=?', (json.dumps(data), ident))
            self.store.event(db, 'metadata', {})
        changed = read_api.sync(self.store, self.query(first))
        self.assertFalse(changed['reset'])

    def test_invalid_queries_are_rejected(self):
        for query in ({'revision': '-1'}, {'revision': True}, {'generation': 'x'},
                      {'generation': '', 'revision': '0'}, {'generation': 1, 'revision': '0'},
                      {'generation': 'x', 'revision': '0', 'unknown': 'x'}):
            with self.subTest(query=query), self.assertRaises(ValueError):read_api.sync(self.store, query)

    def test_read_transaction_prevents_mixed_revision_and_rows(self):
        first = read_api.sync(self.store, {})
        ident = first['snapshot']['spools'][0]['uuid']
        self.store.set_profile(ident, 'Before concurrent edit')
        entered, release, staged = Event(), Event(), Event()
        original_snapshot, original_event = self.store.snapshot, self.store.event
        def snapshot(*args, **kwargs):
            entered.set()
            if not release.wait(5):raise AssertionError('Reader was not released')
            return original_snapshot(*args, **kwargs)
        def event(db, kind, data):
            original_event(db, kind, data)
            if kind == 'profile':staged.set()
        with patch.object(self.store, 'snapshot', side_effect=snapshot), \
             patch.object(self.store, 'event', side_effect=event), ThreadPoolExecutor(2) as pool:
            reader = pool.submit(read_api.sync, self.store, self.query(first))
            try:
                self.assertTrue(entered.wait(5))
                writer = pool.submit(self.store.set_profile, ident, 'After concurrent edit')
                self.assertTrue(staged.wait(5))
            finally:
                release.set()
            captured = reader.result(5)
            writer.result(5)
        self.assertEqual(captured['snapshot']['spools'][0]['material_preset'], 'Before concurrent edit')
        self.assertEqual(captured['native_delta']['tables']['spools']['upserts'][0]['filament_preset_id'], 'Before concurrent edit')
        next_read = read_api.sync(self.store, self.query(captured))
        self.assertGreater(next_read['revision'], captured['revision'])
        self.assertEqual(next_read['snapshot']['spools'][0]['material_preset'], 'After concurrent edit')

    def publish(self, value, expected=None, context=None):
        ident = self.store.snapshot()['spools'][0]['uuid']
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            data = json.loads(db.execute('SELECT data FROM spools WHERE uuid=?', (ident,)).fetchone()['data'])
            data['material_preset'] = value['name']
            db.execute('UPDATE spools SET data=? WHERE uuid=?', (json.dumps(data), ident))
            change = dict(fields=dict(filament_preset_id=value['name']), material_profile=value,
                expected_profile_sha256=expected)
            if context:change['profile_context'] = context
            apply_profile_change(db, ident, change, dict(filament_preset_id=value['name'], material_type='PLA'))
            self.store.event(db, 'profile', {})
        return ident

    def test_immutable_content_survives_reassociation_and_restart(self):
        old, new = profile(), profile('1.03')
        self.publish(old)
        self.publish(new, old['sha256'])
        self.assertEqual(read_api.profile_content(self.store, {'sha256': old['sha256']}), old)
        self.assertEqual(read_api.profile_content(Store(self.path), {'sha256': old['sha256']}), old)
        summaries = read_api.sync(self.store, {})['snapshot']['profiles']
        self.assertEqual([summary['sha256'] for summary in summaries], [new['sha256']])
        self.assertNotIn('settings', summaries[0])

    def test_catalog_deduplicates_base_and_variant_summaries(self):
        value = profile()
        self.publish(value)
        self.publish(value, context=dict(printer_model='Bambu Lab P2S', nozzle_diameter='0.4', flow_type='standard'))
        result = read_api.sync(self.store, {})['snapshot']
        self.assertEqual(len(result['profiles']), 1)
        self.assertEqual(result['profiles'][0]['sha256'], value['sha256'])
        self.assertIn('Bambu Lab P2S|0.4|standard', result['spools'][0]['material_profile_variants'])

    def test_backfill_legacy_profiles_then_detach_keeps_digest_downloadable(self):
        value = profile()
        ident = self.store.snapshot()['spools'][0]['uuid']
        with self.store.connection() as db:
            db.execute('INSERT INTO material_profiles VALUES (?,?)', (ident, json.dumps(value)))
        migrated = Store(self.path)
        migrated.set_profile(ident, 'Different association')
        self.assertEqual(read_api.profile_content(migrated, {'sha256': value['sha256']}), value)

    def test_profile_content_rejects_tampering_and_invalid_queries(self):
        value = profile()
        self.publish(value)
        for query in ({}, {'sha256': '0'}, {'sha256': value['sha256'], 'spool_uuid': 'x'}):
            with self.subTest(query=query), self.assertRaises(ValueError):read_api.profile_content(self.store, query)
        bad = copy.deepcopy(value)
        bad['settings']['filament_flow_ratio'] = '999'
        with self.store.connection() as db:
            db.execute('UPDATE material_profile_contents SET payload=? WHERE sha256=?', (json.dumps(bad), value['sha256']))
        with self.assertRaises(ValueError):read_api.profile_content(self.store, {'sha256': value['sha256']})

    def test_profile_blob_and_association_rollback_together(self):
        value = profile()
        ident = self.store.snapshot()['spools'][0]['uuid']
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            apply_profile_change(db, ident, dict(fields=dict(filament_preset_id=value['name']),
                material_profile=value, expected_profile_sha256=None),
                dict(filament_preset_id=value['name'], material_type='PLA'))
            db.rollback()
        with self.assertRaises(ValueError):read_api.profile_content(self.store, {'sha256': value['sha256']})

    def test_recovery_preserves_unreferenced_historical_content(self):
        old, new = profile(), profile('1.03')
        self.publish(old)
        self.publish(new, old['sha256'])
        restored = restore_recovery(export_recovery(self.store), Path(self.temp.name) / 'restored.sqlite3')
        self.assertEqual(read_api.profile_content(restored, {'sha256': old['sha256']}), old)

    def test_synthetic_changed_and_unchanged_transfer_savings(self):
        rows = [dict(id=f'customer-{index}', name=f'Customer {index}', notes='x' * 1024) for index in range(1000)]
        self.edit_customers(rows)
        first = read_api.sync(self.store, {})
        rows[0]['name'] = 'Changed customer'
        self.edit_customers(rows)
        changed = read_api.sync(self.store, self.query(first))
        unchanged = read_api.sync(self.store, self.query(changed))
        sizes = [len(json.dumps(reply).encode()) for reply in (first, changed, unchanged)]
        self.assertLess(sizes[1], sizes[0] / 50)
        self.assertLess(sizes[2], sizes[0] / 1000)
        self.assertEqual(len(changed['native_delta']['tables']['customers']['upserts']), 1)
        legacy = read_api.materials(self.store, {'view': 'provider'})
        legacy_bytes, legacy_requests = len(json.dumps(legacy).encode()), 1
        for table in read_api.TABLES:
            cursor = 0
            while True:
                page = read_api.native_page(self.store, dict(table=table, revision=legacy['revision'], cursor=cursor))
                legacy_bytes += len(json.dumps(page).encode())
                legacy_requests += 1
                if page['next_cursor'] is None:break
                cursor = page['next_cursor']
        self.assertEqual(legacy_requests, 16)  # Byte cap fits fewer than 200 rows/page.
        self.assertLess(sizes[2], legacy_bytes / 1000)
        print(f'Local sync benchmark: 1000 customers; legacy={legacy_requests} requests/{legacy_bytes} bytes; '
              f'one request each: reset={sizes[0]} bytes, changed={sizes[1]} bytes, unchanged={sizes[2]} bytes')


if __name__ == '__main__':unittest.main()
