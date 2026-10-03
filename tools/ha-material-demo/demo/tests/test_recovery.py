"""Recovery must retain ledger identity and refuse corruption or overwrite."""
import copy
import tempfile
import unittest
from pathlib import Path

from custom_components.quack_material_demo.store import Store, Conflict
from custom_components.quack_material_demo.recovery import export_recovery, restore_recovery


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.store = Store(self.root / 'source.sqlite3')
        self.roll = self.store.snapshot()['spools'][0]['uuid']
        self.store.assign('A1', self.roll, 0)
        self.job = self.store.start_job('Restore test', [{'slot':'A1','weight_mg':1986}], 'stable-request')

    def tearDown(self):
        self.temp.cleanup()

    def test_roundtrip_keeps_pending_reservation_and_exactly_once_completion(self):
        bundle = export_recovery(self.store)
        restored = restore_recovery(bundle, self.root / 'restored.sqlite3')
        self.assertEqual(restored.snapshot()['spools'][0]['reserved_mg'],1986)
        restored.finish_job(self.job['uuid'],'completed',{self.roll:1986},'estimated')
        restored.finish_job(self.job['uuid'],'completed',{self.roll:1986},'estimated')
        self.assertEqual(restored.snapshot()['spools'][0]['remaining_mg'],798014)
        self.assertEqual(restored.snapshot()['spools'][0]['reserved_mg'],0)
        self.assertEqual(self.store.snapshot()['spools'][0]['remaining_mg'],800000)

    def test_tampering_does_not_create_a_restored_database(self):
        bundle = export_recovery(self.store)
        bundle['tables']['spools']['rows'][0][2] = 999999
        destination = self.root / 'bad.sqlite3'
        with self.assertRaises(ValueError):restore_recovery(bundle,destination)
        self.assertFalse(destination.exists())

    def test_existing_destination_is_never_overwritten(self):
        before = (self.root / 'source.sqlite3').read_bytes()
        with self.assertRaises(FileExistsError):
            restore_recovery(export_recovery(self.store),self.root / 'source.sqlite3')
        self.assertEqual((self.root / 'source.sqlite3').read_bytes(),before)

    def test_incompatible_topology_is_rejected_before_destination_is_created(self):
        destination = self.root / 'subset.sqlite3'
        with self.assertRaises((Conflict, ValueError)):
            restore_recovery(export_recovery(self.store), destination, settings={'enabled_slots':['EXT']})
        self.assertFalse(destination.exists())

    def test_export_does_not_include_access_settings_or_files(self):
        self.store.settings.update(access_token='private-marker',allowed_sync_users=['private-user'],base_url='https://private.example')
        bundle=export_recovery(self.store)
        self.assertNotIn('settings',bundle)
        self.assertNotIn('private-marker',str(bundle))
        self.assertNotIn('private-user',str(bundle))


if __name__ == '__main__':unittest.main()
