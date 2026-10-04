"""Receipt compaction keeps permanent operation identity and is transactional."""
import copy
import json
import sqlite3
import tempfile
import unittest
from pathlib import Path

from custom_components.quack_material_demo import receipts
from custom_components.quack_material_demo.native_bridge import TABLES
from custom_components.quack_material_demo.store import Store, Conflict


class ReceiptMigrationTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.store=Store(Path(self.temp.name)/'receipts.sqlite3')
        self.native=dict(revision=0,bundle=dict(schema_version=8,tables={table:[] for table in TABLES}))
        self.store.sync_native(self.native)
        baseline=self.store.snapshot();roll=baseline['native_bundle']['tables']['spools'][0]
        self.request=dict(request_key='legacy-edit',revision=baseline['revision'],confirmed=True,
            changes=[dict(spool_uuid=roll['id'],fields={'name':'Originally committed'},expected={'name':roll['name']})])
        self.accepted=self.store.apply_native(self.request)
        with self.store.connection() as db:
            db.execute("UPDATE receipts SET payload=? WHERE request_key LIKE 'native:%'",(json.dumps(self.native,sort_keys=True),))
            db.execute('UPDATE receipts SET payload=?,result=? WHERE request_key=?',
                (json.dumps(dict(action='native_apply',data=self.request),sort_keys=True),json.dumps(self.accepted),'legacy-edit'))

    def tearDown(self):self.temp.cleanup()

    def dump(self):
        with self.store.connection() as db:return list(db.iterdump())

    def test_legacy_snapshot_compaction_preserves_replay_and_original_acceptance(self):
        result=receipts.compact_legacy_receipts(self.store)
        self.assertEqual(result['compacted'],2)
        self.assertGreater(result['bytes_saved'],1000)
        with self.store.connection() as db:
            row=db.execute("SELECT payload,result FROM receipts WHERE request_key='legacy-edit'").fetchone()
            self.assertLess(len(row['payload'])+len(row['result']),256)
        self.store.apply_native(dict(request_key='later-edit',revision=self.store.snapshot()['revision'],confirmed=True,
            changes=[dict(spool_uuid=self.request['changes'][0]['spool_uuid'],fields={'name':'Later metadata'},
                expected={'name':'Originally committed'})]))
        reopened=Store(self.store.path,seed_demo=False)
        before=self.dump()
        ack=reopened.apply_native(dict(self.request,response='ack'))
        self.assertEqual(ack['accepted_revision'],self.accepted['revision'])
        self.assertEqual(reopened.apply_native(self.request)['spools'][0]['product'],'Later metadata')
        self.assertEqual(reopened.sync_native(self.native)['spools'][0]['product'],'Later metadata')
        self.assertEqual(self.dump(),before)
        forged=copy.deepcopy(self.request);forged['changes'][0]['fields']['name']='Different'
        with self.assertRaises(Conflict):reopened.apply_native(forged)

    def test_migration_batch_limit_and_import_metadata_preserve_unknown_receipts(self):
        with self.store.connection() as db:
            db.execute('INSERT INTO receipts VALUES (?,?,?)',('production-import','{"source": "Production database"}','{}'))
            db.execute('INSERT INTO receipts VALUES (?,?,?)',('other-action','{"action": "unknown", "data": {}}','{}'))
        self.assertEqual(receipts.compact_legacy_receipts(self.store,limit=1)['compacted'],1)
        self.assertEqual(receipts.compact_legacy_receipts(self.store,limit=1)['compacted'],1)
        self.assertEqual(receipts.compact_legacy_receipts(self.store,limit=1)['compacted'],0)
        with self.store.connection() as db:
            self.assertEqual(db.execute("SELECT payload FROM receipts WHERE request_key='production-import'").fetchone()[0],'{"source": "Production database"}')
            self.assertEqual(db.execute("SELECT payload FROM receipts WHERE request_key='other-action'").fetchone()[0],'{"action": "unknown", "data": {}}')

    def test_migration_failure_rolls_back_every_updated_receipt(self):
        with self.store.connection() as db:
            db.execute("CREATE TRIGGER reject_compaction BEFORE UPDATE ON receipts WHEN NEW.request_key='legacy-edit' BEGIN SELECT RAISE(ABORT, 'disk failure'); END")
        before=self.dump()
        with self.assertRaises(sqlite3.IntegrityError):receipts.compact_legacy_receipts(self.store)
        self.assertEqual(self.dump(),before)


if __name__=='__main__':unittest.main()
