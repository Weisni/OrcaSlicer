"""Whole-workflow backup keeps profiles and retry identities but excludes credentials."""
import importlib.util
import json
import tempfile
import unittest
import zipfile
from pathlib import Path
from custom_components.quack_material_demo.store import Store
from custom_components.quack_material_demo.recovery import export_recovery

spec=importlib.util.spec_from_file_location('workflow_recovery',Path(__file__).resolve().parents[2]/'tools/filament_workflow_recovery.py')
tool=importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class WorkflowRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        self.store=Store(self.root/'source.sqlite3',seed_demo=True)
        self.export=self.root/'ha.json';self.export.write_text(json.dumps(export_recovery(self.store)),encoding='utf-8')
        self.user=self.root/'user-data';(self.user/'user/default/filament').mkdir(parents=True)
        (self.user/'user/default/filament/Exact PLA.json').write_text('{"name":"Exact PLA","filament_start_gcode":["; keep exactly"]}',encoding='utf-8')
        self.journal=b'{"endpoint":"https://ha.example/api/quack_material_demo/materials","payload":{"request_key":"permanent-identity"}}'
        (self.user/'ha-explicit-pending.json').write_bytes(self.journal)
        self.project_journal=b'{"endpoint":"https://ha.example/api/quack_material_demo/materials","payload":{"request_key":"project-profile-identity","changes":[]},"cache_rolls":[]}'
        (self.user/'ha-project-material-pending.json').write_bytes(self.project_journal)
        (self.user/'QuackSlicer.conf').write_text('access_token=DO-NOT-EXPORT',encoding='utf-8')

    def tearDown(self):self.tmp.cleanup()

    def test_roundtrip_preserves_profiles_and_journal_without_account_settings(self):
        archive=self.root/'recovery.zip';tool.create_bundle(self.export,self.user,archive)
        with zipfile.ZipFile(archive) as z:
            self.assertNotIn('quack/QuackSlicer.conf',z.namelist())
            self.assertFalse(any(b'DO-NOT-EXPORT' in z.read(n) for n in z.namelist()))
        destination=self.root/'restored';result=tool.restore_bundle(archive,destination)
        self.assertEqual(self.journal,(destination/'quack/ha-explicit-pending.json').read_bytes())
        self.assertEqual(self.project_journal,(destination/'quack/ha-project-material-pending.json').read_bytes())
        self.assertTrue((destination/'quack/user/default/filament/Exact PLA.json').is_file())
        self.assertEqual(len(self.store.snapshot()['spools']),result['rolls'])

    def test_modified_member_rejected_without_creating_destination(self):
        archive=self.root/'recovery.zip';tool.create_bundle(self.export,self.user,archive)
        altered=self.root/'altered.zip'
        with zipfile.ZipFile(archive) as source, zipfile.ZipFile(altered,'w') as target:
            for name in source.namelist():target.writestr(name,b'{}' if name.endswith('pending.json') else source.read(name))
        with self.assertRaises(ValueError):tool.restore_bundle(altered,self.root/'restored')
        self.assertFalse((self.root/'restored').exists())

    def test_existing_destination_is_never_replaced(self):
        archive=self.root/'recovery.zip';tool.create_bundle(self.export,self.user,archive)
        destination=self.root/'restored';destination.mkdir();marker=destination/'keep';marker.write_text('keep')
        with self.assertRaises(FileExistsError):tool.restore_bundle(archive,destination)
        self.assertEqual('keep',marker.read_text())

    def test_unlisted_traversal_member_is_rejected(self):
        archive=self.root/'recovery.zip';tool.create_bundle(self.export,self.user,archive)
        with zipfile.ZipFile(archive,'a') as target:target.writestr('../escape.json','{}')
        with self.assertRaises(ValueError):tool.restore_bundle(archive,self.root/'restored')
        self.assertFalse((self.root/'escape.json').exists())


if __name__=='__main__':unittest.main()
