"""Nozzle profile variants preserve the physical roll and legacy profile."""
import hashlib
import json
import unittest
from pathlib import Path

import test_explicit_sync as fixtures
from test_profiles import profile
from custom_components.quack_material_demo.read_api import profile as download
from custom_components.quack_material_demo.recovery import export_recovery, restore_recovery
from custom_components.quack_material_demo.store import Conflict, Store


def high_flow_profile(flow_ratio='0.98'):
    """Synthetic effective 0.8 HF payload, not a calibrated printer preset."""
    value = profile(flow_ratio)
    value['name'] = 'Exact Weiß PLA 0.8 HF'
    value['settings'].update(compatible_printers='Bambu Lab P2S 0.8 nozzle',
                             filament_max_volumetric_speed='22')
    value.pop('sha256')
    value['sha256'] = hashlib.sha256(json.dumps(value, sort_keys=True,
        separators=(',', ':'), ensure_ascii=False).encode()).hexdigest()
    return value


class VariantTests(unittest.TestCase):
    setUp = fixtures.ExplicitSyncTests.setUp
    tearDown = fixtures.ExplicitSyncTests.tearDown
    request = fixtures.ExplicitSyncTests.request
    change = fixtures.ExplicitSyncTests.change
    apply = fixtures.ExplicitSyncTests.apply
    dump = fixtures.ExplicitSyncTests.dump

    def save(self, nozzle='0.4', flow='standard', key='variant', expected=None, value=None):
        value = value or (high_flow_profile() if (nozzle, flow) == ('0.8', 'high_flow') else profile())
        change = self.change(filament_preset_id=value['name'])
        change.update(material_profile=value, expected_profile_sha256=expected,
                      profile_context=dict(printer_model='Bambu Lab P2S', nozzle_diameter=nozzle, flow_type=flow))
        return self.request([change], key)

    def test_multiple_variants_keep_legacy_roll_and_history(self):
        before = self.store.snapshot()
        first = self.apply(self.save())
        hf = high_flow_profile('1.03')
        second = self.apply(self.save('0.8', 'high_flow', 'other', value=hf))
        variants = second['spools'][0]['material_profile_variants']
        self.assertEqual(set(variants), {'Bambu Lab P2S|0.4|standard', 'Bambu Lab P2S|0.8|high_flow'})
        self.assertEqual(second['native_bundle'], before['native_bundle'])
        self.assertEqual(second['slots'], before['slots'])
        self.assertEqual(second['spools'][0]['material_preset'], 'Special PLA')
        self.assertIsNone(second['spools'][0]['material_profile'])
        projected = self.store.snapshot(provider=True)['spools'][0]['material_profile_variants']
        self.assertNotIn('settings', projected['Bambu Lab P2S|0.4|standard'])
        self.assertEqual(variants['Bambu Lab P2S|0.8|high_flow'], hf)
        self.assertEqual(download(self.store, dict(spool_uuid=self.ids[0], sha256=hf['sha256'],
            context='Bambu Lab P2S|0.8|high_flow')), hf)
        self.assertEqual(download(self.store, dict(spool_uuid=self.ids[0], sha256=profile()['sha256'],
            context='Bambu Lab P2S|0.4|standard')), profile())
        with self.assertRaises(Conflict):
            download(self.store, dict(spool_uuid=self.ids[0], sha256=profile()['sha256']))

    def test_legacy_profile_survives_variant_and_retry(self):
        change = self.change(filament_preset_id=profile()['name'])
        change.update(material_profile=profile(), expected_profile_sha256=None)
        self.apply(self.request([change], 'legacy'))
        request = self.save(value=profile('1.01'))
        result = self.apply(request)
        replay = Store(self.path, seed_demo=False, settings={'mode':'pilot'}).dispatch('native_apply', request)
        self.assertEqual(result['spools'], replay['spools'])
        self.assertEqual(result['revision'], replay['revision'])
        self.assertEqual(result['spools'][0]['material_profile'], profile())
        restored = restore_recovery(export_recovery(self.store), Path(self.temp.name)/'restored.sqlite3')
        self.assertEqual(restored.snapshot()['spools'], result['spools'])

    def test_conflict_or_mismatch_rolls_back_complete_transaction(self):
        self.apply(self.save())
        before = self.dump()
        with self.assertRaises(Conflict):
            self.apply(self.save(key='conflict', expected='0'*64))
        self.assertEqual(before, self.dump())
        request = self.save('0.8', key='mismatch')
        request['changes'][0]['fields']['material_type']='PETG'
        request['changes'][0]['expected']['material_type']='PLA'
        with self.assertRaises(ValueError): self.apply(request)
        self.assertEqual(before, self.dump())

    def test_invalid_contexts_do_not_write(self):
        invalid = [('printer_model',''), ('printer_model','a|b'), ('printer_model','é'*65),
                   ('nozzle_diameter','0'), ('nozzle_diameter','0.40'), ('nozzle_diameter','04'),
                   ('nozzle_diameter',0.4), ('nozzle_diameter','1e-1'), ('flow_type','HF')]
        before = self.dump()
        for field, value in invalid:
            with self.subTest(field=field, value=value):
                request = self.save()
                request['changes'][0]['profile_context'][field]=value
                with self.assertRaises(ValueError): self.apply(request)
                self.assertEqual(before, self.dump())

    def test_old_database_migrates_without_relabeling_legacy(self):
        with self.store.connection() as db: db.execute('DROP TABLE material_profile_variants')
        before = self.store.snapshot()['native_bundle']
        reopened = Store(self.path, seed_demo=False, settings={'mode':'pilot'})
        self.assertEqual(reopened.snapshot()['native_bundle'], before)
        self.assertEqual(reopened.snapshot()['spools'][0]['material_profile_variants'], {})

    def test_envelope_only_variant_cas_is_independent_of_other_nozzle(self):
        request = self.save()
        request['changes'][0].update(fields={}, expected={})
        self.apply(request)
        self.apply(self.save('0.8', 'high_flow', 'other'))
        self.apply(self.save(key='updated', expected=profile()['sha256'], value=profile('1.02')))
        variants = self.store.snapshot()['spools'][0]['material_profile_variants']
        self.assertEqual(variants['Bambu Lab P2S|0.8|high_flow'], high_flow_profile())
        self.assertEqual(variants['Bambu Lab P2S|0.4|standard'], profile('1.02'))

    def test_asa_plus_roll_uses_asa_profile_without_changing_product(self):
        self.apply(self.request([self.change(material_type='ASA+')], 'physical-product'))
        value = profile()
        value.update(name='Generic ASA - NoWarp', material_type='ASA')
        value['settings']['filament_type']='ASA'
        value.pop('sha256')
        value['sha256']=hashlib.sha256(json.dumps(value, sort_keys=True,
            separators=(',', ':'), ensure_ascii=False).encode()).hexdigest()
        result = self.apply(self.save(value=value))
        self.assertEqual(result['spools'][0]['material_type'], 'ASA+')
        self.assertEqual(download(self.store, dict(spool_uuid=self.ids[0], sha256=value['sha256'],
            context='Bambu Lab P2S|0.4|standard')), value)
