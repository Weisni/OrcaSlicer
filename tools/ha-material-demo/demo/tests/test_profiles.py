"""Full material payload validation and atomic profile compare-and-swap."""
import copy
import hashlib
import json
import sqlite3
import unittest

from custom_components.quack_material_demo.profiles import (
    apply_profile_change, attach_profiles, profile_summary, read_profile, validate_profile)


def profile(flow='0.98'):
    value = dict(schema_version=1, name='Exact Weiß PLA', material_type='PLA',
        settings=dict(filament_type='PLA', filament_flow_ratio=flow,
                      filament_start_gcode='; keep material code\nM900 K0.02',
                      compatible_printers='Bambu Lab P2S 0.4 nozzle'),
        dependencies=dict(inherits='Generic PLA', filament_id='GFA00', vendor='BBL'))
    value['sha256'] = hashlib.sha256(json.dumps(value, sort_keys=True,
        separators=(',', ':'), ensure_ascii=False).encode()).hexdigest()
    return value


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(':memory:')
        self.db.row_factory = sqlite3.Row
        self.db.executescript('CREATE TABLE material_profiles(spool_uuid TEXT PRIMARY KEY,payload TEXT NOT NULL);'
                             'CREATE TABLE spools(uuid TEXT PRIMARY KEY,data TEXT NOT NULL);')
        self.native = dict(filament_preset_id='Exact Weiß PLA', material_type='PLA')
        self.db.execute('INSERT INTO spools VALUES (?,?)', ('roll-a', json.dumps(
            dict(material_preset=self.native['filament_preset_id'], material_type='PLA'))))

    def tearDown(self):
        self.db.close()

    def change(self, payload=None, expected=None):
        return dict(fields=dict(filament_preset_id=self.native['filament_preset_id']),
                    material_profile=payload or profile(), expected_profile_sha256=expected)

    def test_same_name_changed_settings_are_stored_with_new_digest(self):
        first = profile()
        apply_profile_change(self.db, 'roll-a', self.change(first), self.native)
        second = profile('1.03')
        apply_profile_change(self.db, 'roll-a', self.change(second, first['sha256']), self.native)
        self.assertEqual(read_profile(self.db, 'roll-a', second['sha256']), second)
        self.assertNotEqual(first['sha256'], second['sha256'])

    def test_tampered_settings_or_incomplete_envelopes_are_rejected(self):
        for field, value in [('sha256', '0' * 64), ('schema_version', True), ('settings', []),
                             ('dependencies', {}), ('extra', 'unsupported')]:
            with self.subTest(field=field):
                bad = profile()
                bad[field] = value
                with self.assertRaises(ValueError):
                    validate_profile(bad)

    def test_profile_digest_conflict_preserves_prior_settings(self):
        first = profile()
        apply_profile_change(self.db, 'roll-a', self.change(first), self.native)
        with self.assertRaises(ValueError):
            apply_profile_change(self.db, 'roll-a', self.change(profile('1.03'), '0' * 64), self.native)
        self.assertEqual(read_profile(self.db, 'roll-a', first['sha256']), first)

    def test_profile_upload_requires_matching_roll_material_and_selected_association(self):
        for native, change in [(dict(self.native, material_type='PETG'), self.change()),
                               (self.native, dict(self.change(), fields={})),
                               (self.native, dict(self.change(), material_profile=None))]:
            with self.subTest(native=native, change=change):
                with self.assertRaises(ValueError):
                    apply_profile_change(self.db, 'roll-a', change, native)
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM material_profiles').fetchone()[0], 0)

    def test_profile_read_is_digest_pinned_and_summary_omits_settings(self):
        value = profile()
        apply_profile_change(self.db, 'roll-a', self.change(value), self.native)
        with self.assertRaises(ValueError):
            read_profile(self.db, 'roll-a', '0' * 64)
        summary = profile_summary(value)
        self.assertNotIn('settings', summary)
        self.assertEqual(summary['dependencies'], value['dependencies'])
        spools = [dict(uuid='roll-a'), dict(uuid='roll-b')]
        attach_profiles(self.db, spools)
        self.assertEqual(spools[0]['material_profile'], value)
        self.assertIsNone(spools[1]['material_profile'])

    def test_names_only_reassignment_detaches_obsolete_profile(self):
        apply_profile_change(self.db, 'roll-a', self.change(), self.native)
        apply_profile_change(self.db, 'roll-a', dict(fields=dict(color_hex='#112233')), self.native)
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM material_profiles').fetchone()[0], 1)
        apply_profile_change(self.db, 'roll-a', dict(fields=dict(filament_preset_id='Other')),
                             dict(self.native, filament_preset_id='Other'))
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM material_profiles').fetchone()[0], 0)

    def test_cached_digest_cannot_load_a_profile_after_legacy_reassignment(self):
        value = profile()
        apply_profile_change(self.db, 'roll-a', self.change(value), self.native)
        self.db.execute('UPDATE spools SET data=? WHERE uuid=?',
            (json.dumps(dict(material_preset='Other profile', material_type='PLA')), 'roll-a'))
        with self.assertRaises(ValueError):
            read_profile(self.db, 'roll-a', value['sha256'])

    def test_oversized_or_nonstring_settings_are_rejected_before_storage(self):
        for key, value in [('filament_flow_ratio', 1.1), ('filament_start_gcode', 'x' * 65537),
                           ('bad/key', 'x'), ('filament_notes', '\0')]:
            bad = profile()
            bad['settings'][key] = value
            bad.pop('sha256')
            bad['sha256'] = hashlib.sha256(json.dumps(bad, sort_keys=True,
                separators=(',', ':'), ensure_ascii=False).encode()).hexdigest()
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_profile(bad)
