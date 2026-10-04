"""Bounded native pages are coherent or fail without importing a mixed revision."""
import json
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from threading import Event
from unittest.mock import patch
from pathlib import Path

from custom_components.quack_material_demo.store import Store, Conflict
from custom_components.quack_material_demo import read_api


class ReadApiTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.store=Store(Path(self.temp.name)/'inventory.sqlite3')
        self.store.sync_native(dict(revision=0,bundle=dict(schema_version=8,
            tables={table:[] for table in read_api.TABLES})))

    def tearDown(self):
        self.temp.cleanup()

    def test_lite_materials_excludes_history_without_losing_bindings(self):
        snapshot=self.store.snapshot()
        result=read_api.materials(self.store,{'view':'provider'})
        self.assertEqual(result['spools'],snapshot['spools'])
        self.assertEqual(result['slots'],snapshot['slots'])
        for key in ('jobs','native_bundle','orders'):self.assertNotIn(key,result)
        self.assertTrue(result['capabilities']['provider_delta'])

    def test_lite_materials_does_not_project_history_and_keeps_reservations(self):
        snapshot = self.store.snapshot()
        spool = snapshot['spools'][0]
        self.store.assign('A1', spool['uuid'], 0)
        self.store.start_job('Reservation', [{'slot':'A1','spool_uuid':spool['uuid'],'weight_mg':5000}], 'lite-job')
        full = self.store.snapshot()
        with patch.object(self.store, 'project_native', side_effect=AssertionError('History was projected')):
            result = read_api.materials(self.store, {'view':'provider'})
        self.assertEqual(result['spools'], full['spools'])
        self.assertEqual(result['spools'][0]['reserved_mg'], 5000)

    def test_pages_reconstruct_rows_and_reject_revision_changes(self):
        first=read_api.native_page(self.store,{'table':'spools','limit':'1'})
        self.assertEqual(len(first['rows']),1)
        self.assertEqual(first['next_cursor'],1)
        second=read_api.native_page(self.store,{'table':'spools','limit':'2','cursor':'1','revision':str(first['revision'])})
        self.assertEqual(len(second['rows']),2)
        self.assertIsNone(second['next_cursor'])
        self.assertEqual({r['id'] for r in first['rows']+second['rows']},
                         {s['uuid'] for s in self.store.snapshot()['spools']})
        self.store.set_profile(first['rows'][0]['id'],'New exact profile')
        with self.assertRaises(Conflict):
            read_api.native_page(self.store,{'table':'spools','cursor':'1','revision':str(first['revision'])})

    def test_page_is_bounded_by_bytes_as_well_as_rows(self):
        with self.store.connection() as db:
            bundle=json.loads(db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()['data'])
            bundle['tables']['customers']=[dict(id=str(index),name='Customer',notes='x'*16000) for index in range(100)]
            db.execute("UPDATE bridge SET data=? WHERE id='native'",(json.dumps(bundle),))
        result=read_api.native_page(self.store,{'table':'customers','limit':'200'})
        self.assertLess(len(json.dumps(result).encode()),200000)
        self.assertGreater(len(result['rows']),0)
        self.assertLess(len(result['rows']),100)
        self.assertEqual(result['next_cursor'],len(result['rows']))

    def test_projected_rows_have_stable_creation_identity_at_one_revision(self):
        first=read_api.native_page(self.store,{'table':'spools'})
        second=read_api.native_page(self.store,{'table':'spools','revision':str(first['revision'])})
        self.assertEqual([row['created_at'] for row in first['rows']],
                         [row['created_at'] for row in second['rows']])

    def test_pages_project_once_per_revision_and_copy_their_rows(self):
        with patch.object(self.store, 'project_native', wraps=self.store.project_native) as project:
            first = read_api.native_page(self.store, {'table':'spools','limit':1})
            ident = first['rows'][0]['id']
            first['rows'][0]['filament_preset_id'] = 'Caller-owned edit'
            read_api.native_page(self.store, {'table':'stock_events','revision':first['revision']})
            again = read_api.native_page(self.store, {'table':'spools','revision':first['revision']})
            self.assertEqual(project.call_count, 1)
            self.assertNotEqual(again['rows'][0]['filament_preset_id'], 'Caller-owned edit')
            self.store.set_profile(ident, 'New profile')
            with self.assertRaises(Conflict):
                read_api.native_page(self.store, {'table':'spools','revision':first['revision']})
            self.assertEqual(project.call_count, 1)  # Stale reads fail before projection.
            updated = read_api.native_page(self.store, {'table':'spools'})
            self.assertEqual(project.call_count, 2)
            self.assertEqual(updated['rows'][0]['filament_preset_id'], 'New profile')

    def test_oversized_cache_is_evicted_and_bypassed(self):
        first = read_api.native_page(self.store, {'table':'spools'})
        self.store.set_profile(first['rows'][0]['id'], 'New profile')
        with patch.object(read_api, 'NATIVE_CACHE_BYTES', 1), \
             patch.object(self.store, 'project_native', wraps=self.store.project_native) as project:
            read_api.native_page(self.store, {'table':'spools'})
            read_api.native_page(self.store, {'table':'stock_events'})
            self.assertEqual(project.call_count, 2)
            self.assertIsNone(self.store._native_page_cache)

    def test_mutation_snapshot_reads_its_transaction_without_the_page_cache(self):
        initial = read_api.native_page(self.store, {'table':'spools'})
        ident = initial['rows'][0]['id']
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT data FROM spools WHERE uuid=?', (ident,)).fetchone()
            data = json.loads(row['data']);data['material_preset'] = 'Uncommitted profile'
            db.execute('UPDATE spools SET data=? WHERE uuid=?', (json.dumps(data), ident))
            snapshot = self.store.snapshot(db)
            self.assertEqual(snapshot['native_bundle']['tables']['spools'][0]['filament_preset_id'], 'Uncommitted profile')
            db.rollback()
        again = read_api.native_page(self.store, {'table':'spools'})
        self.assertEqual(again, initial)

    def test_concurrent_readers_share_one_projection(self):
        entered, release = Event(), Event()
        original = self.store.project_native
        def project(*args):
            entered.set()
            if not release.wait(5):raise AssertionError('Reader was not released')
            return original(*args)
        with patch.object(self.store, 'project_native', side_effect=project) as spy, ThreadPoolExecutor(2) as pool:
            first = pool.submit(read_api.native_page, self.store, {'table':'spools'})
            try:
                self.assertTrue(entered.wait(5))
                second = pool.submit(read_api.native_page, self.store, {'table':'stock_events'})
            finally:
                release.set()
            self.assertEqual(first.result(5)['revision'], second.result(5)['revision'])
            self.assertEqual(spy.call_count, 1)

    def test_write_during_projection_cannot_mix_page_revisions(self):
        ident = self.store.snapshot()['spools'][0]['uuid']
        entered, release, staged = Event(), Event(), Event()
        original_project, original_event = self.store.project_native, self.store.event
        def project(*args):
            result = original_project(*args)
            entered.set()
            if not release.wait(5):raise AssertionError('Reader was not released')
            return result
        def event(db, kind, data):
            original_event(db, kind, data)
            if kind == 'profile':staged.set()
        with patch.object(self.store, 'project_native', side_effect=project), \
             patch.object(self.store, 'event', side_effect=event), ThreadPoolExecutor(2) as pool:
            page = pool.submit(read_api.native_page, self.store, {'table':'spools'})
            try:
                self.assertTrue(entered.wait(5))
                writer = pool.submit(self.store.set_profile, ident, 'Concurrent profile')
                self.assertTrue(staged.wait(5))
            finally:
                release.set()
            old = page.result(5)
            writer.result(5)
        self.assertNotEqual(old['rows'][0]['filament_preset_id'], 'Concurrent profile')
        with self.assertRaises(Conflict):
            read_api.native_page(self.store, {'table':'stock_events','revision':old['revision']})
        new = read_api.native_page(self.store, {'table':'spools'})
        self.assertGreater(new['revision'], old['revision'])
        self.assertEqual(new['rows'][0]['filament_preset_id'], 'Concurrent profile')

    def test_invalid_page_inputs_do_not_mutate(self):
        for query in ({'table':'secrets'},{'table':'spools','limit':'201'},
                      {'table':'spools','cursor':'1'},{'table':'spools','cursor':'-1'},
                      {'table':'spools','revision':'-1'},{'table':'spools','limit':'0'},
                      {'table':'spools','unknown':'1'}):
            with self.subTest(query=query),self.assertRaises(ValueError):read_api.native_page(self.store,query)


if __name__=='__main__':unittest.main()
