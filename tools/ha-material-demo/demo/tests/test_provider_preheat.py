"""Provider preheat claims share the real dispatch/material SQLite commit."""
import copy
import json
import sqlite3
import tempfile
import unittest
import uuid
from pathlib import Path

from custom_components.quack_materials.native_bridge import TABLES
from custom_components.quack_materials.store import Store, Conflict
from custom_components.quack_materials.preheat import Preheat
from custom_components.quack_materials.read_api import materials, provider_view
from custom_components.quack_materials.recovery import export_recovery, restore_recovery


class ProviderPreheatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.now = 100000.0
        self.settings = {'mode': 'pilot', 'provider_printer_id': 'P2S-test'}
        self.store = Store(self.root/'source.db', seed_demo=False, settings=self.settings)
        self.roll, self.job = str(uuid.uuid4()), str(uuid.uuid4())
        tables = {name: [] for name in TABLES}
        tables['spools'] = [dict(id=self.roll, name='PLA', manufacturer='Maker', created_at='2026-01-01', updated_at='2026-01-01', material_type='PLA', filament_preset_id='PLA exact',
            color_hex='#FFFFFF', diameter_mm=1.75, density_g_cm3=1.24, nominal_capacity_mg=1000000,
            status='active', warning_mode='none', warning_value=0)]
        tables['stock_events'] = [dict(id=str(uuid.uuid4()), spool_id=self.roll, event_type='initial',
            delta_mg=500000, balance_after_mg=500000, operation_key='seed')]
        self.store.sync_native(dict(revision=0, bundle=dict(schema_version=8, tables=tables)))
        self.store.assign('A1', self.roll, 0)
        snapshot = self.store.snapshot()
        bundle = copy.deepcopy(snapshot['native_bundle'])
        bundle['tables']['print_jobs'].append(dict(id=self.job, idempotency_key='reserve', job_name='Plate',
            printer_id='P2S-test', state='reserved'))
        bundle['tables']['allocations'].append(dict(id=str(uuid.uuid4()), job_id=self.job, spool_id=self.roll,
            filament_index=0, estimated_weight_mg=10000, actual_weight_mg=None,
            filament_preset_id='PLA exact', color_hex='#FFFFFF'))
        self.store.provider_apply(dict(request_key='reserve', revision=snapshot['revision'],
            before=snapshot['native_bundle'], bundle=bundle))
        self.core = Preheat(self.store, clock=lambda: self.now)
        self.store.preheat = self.core

    def observation(self):
        return dict(printer_id='P2S-test', status='idle', status_at=self.now,
                    door_closed=True, door_closed_at=self.now)

    def admission(self):
        response = self.core.request(self.job, dict(request_key='preheat', instance_id='quack-a',
            printer_id='P2S-test', parameters=dict(enabled=False, chamber_target=0,
            print_bed_target=0, preheat_bed_target=0)), self.observation())
        self.assertTrue(response['ready'])
        return dict(instance_id='quack-a', version=response['version'])

    def request(self, metadata=True):
        slot = self.store.snapshot()['slots'][0]
        data = dict(printer_id='P2S-test', print_name='Plate', allocations=[dict(filament_index=0,
            spool_uuid=self.roll, slot='A1', revision=slot['revision'])])
        if metadata: data['preheat'] = self.admission()
        return dict(request_key='prepare', command='prepare_print', job_uuid=self.job, data=data)

    def preheat_phase(self):
        with self.store.connection() as db:
            return json.loads(db.execute('SELECT data FROM job_preheat WHERE job_uuid=?', (self.job,)).fetchone()[0])['phase']

    def test_enabled_runtime_rejects_legacy_bypass_without_creating_dispatch(self):
        with self.assertRaises(Conflict): self.store.provider_job(self.request(False))
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM provider_dispatch').fetchone()[0], 0)

    def test_disabled_preheat_admission_claim_and_receipt_replay_are_atomic(self):
        request = self.request()
        result = self.store.provider_job(request)
        self.assertEqual(result['provider_job']['status'], 'prepared')
        self.assertEqual(self.preheat_phase(), 'handoff')
        self.assertEqual(self.store.provider_job(request)['provider_job'], result['provider_job'])
        request['request_key'] = 'second-prepare'
        with self.assertRaises(Conflict): self.store.provider_job(request)
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM provider_dispatch').fetchone()[0], 1)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM events WHERE kind='job_preheat_claim'").fetchone()[0], 1)

    def test_wrong_instance_and_stale_version_cannot_claim(self):
        request = self.request()
        for metadata in ({'instance_id': 'quack-b', 'version': 1}, {'instance_id': 'quack-a', 'version': 0}):
            request['data']['preheat'] = metadata
            with self.assertRaises(Conflict): self.store.provider_job(request)
            self.assertEqual(self.preheat_phase(), 'ready')

    def test_material_conflict_leaves_the_preheat_claim_available(self):
        request = self.request()
        request['data']['allocations'][0]['revision'] += 1
        with self.assertRaises(Conflict): self.store.provider_job(request)
        self.assertEqual(self.preheat_phase(), 'ready')

    def test_failed_dispatch_insert_rolls_back_the_preheat_claim(self):
        request = self.request()
        with self.store.connection() as db:
            db.execute("CREATE TRIGGER fail_dispatch BEFORE INSERT ON provider_dispatch BEGIN SELECT RAISE(ABORT,'injected insert failure'); END")
        with self.assertRaises(sqlite3.IntegrityError): self.store.provider_job(request)
        self.assertEqual(self.preheat_phase(), 'ready')
        with self.store.connection() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM events WHERE kind='job_preheat_claim'").fetchone()[0], 0)
            db.execute('DROP TRIGGER fail_dispatch')
        self.assertEqual(self.store.provider_job(request)['provider_job']['status'], 'prepared')

    def test_runtime_disable_rejects_metadata_but_preserves_legacy_behavior(self):
        request = self.request()
        self.store.preheat = None
        with self.assertRaises(Conflict): self.store.provider_job(request)
        request['data'].pop('preheat')
        self.assertEqual(self.store.provider_job(request)['provider_job']['status'], 'prepared')

    def test_capability_is_published_only_with_the_runtime_and_internal_flag_is_hidden(self):
        for query in ({}, {'view': 'provider'}):
            result = materials(self.store, query)
            self.assertTrue(result['capabilities']['job_preheat_v1'])
            self.assertNotIn('job_preheat_enabled', result)
            self.assertTrue(provider_view(result)['capabilities']['job_preheat_v1'])
        self.store.preheat = None
        for query in ({}, {'view': 'provider'}):
            self.assertNotIn('job_preheat_v1', materials(self.store, query)['capabilities'])

    def manual_observation(self):
        result = self.observation()
        result.update(chamber_temp=40, bed_temp=20, bed_target=0, door_open_count=0,
                      air_mode='internal', idle_protection=True, maintenance_mode=False)
        for key in ('chamber_temp', 'bed_temp', 'bed_target', 'air_mode', 'idle_protection', 'maintenance_mode'):
            result[key + '_at'] = self.now
        return result

    def manual_admission(self):
        self.store.preheat = None
        self.store.preheat_guard = self.core
        self.store.settings['manual_preheat_enabled'] = True
        self.manual = str(uuid.uuid4())
        return self.core.request_manual(self.manual, dict(request_key='manual-start', instance_id='ha-manual-a',
            printer_id='P2S-test', parameters=dict(enabled=True, chamber_target=55,
            print_bed_target=110, preheat_bed_target=110)), self.manual_observation())

    def test_active_manual_preheat_blocks_legacy_dispatch_without_claim_or_intent(self):
        self.manual_admission()
        with self.assertRaises(Conflict): self.store.provider_job(self.request(False))
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM provider_dispatch').fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM events WHERE kind='job_preheat_claim'").fetchone()[0], 0)

    def test_reservation_during_manual_heating_then_takeover_and_dispatch_remain_atomic(self):
        manual = self.manual_admission()
        self.store.preheat = self.core
        # This is the actual provider reservation path, while manual preheat owns
        # the printer. Reservations allocate stock; they do not dispatch a print.
        snapshot = self.store.snapshot()
        bundle = copy.deepcopy(snapshot['native_bundle'])
        self.job = str(uuid.uuid4())
        bundle['tables']['print_jobs'].append(dict(id=self.job, idempotency_key='reserve-during-manual',
            job_name='Warm plate', printer_id='P2S-test', state='reserved'))
        bundle['tables']['allocations'].append(dict(id=str(uuid.uuid4()), job_id=self.job, spool_id=self.roll,
            filament_index=0, estimated_weight_mg=10000, actual_weight_mg=None,
            filament_preset_id='PLA exact', color_hex='#FFFFFF'))
        self.store.provider_apply(dict(request_key='reserve-during-manual', revision=snapshot['revision'],
            before=snapshot['native_bundle'], bundle=bundle))
        self.assertEqual(self.store.snapshot()['spools'][0]['reserved_mg'], 20000)
        target = 0
        observation = self.manual_observation()
        for intent in self.core.tick(observation):
            self.assertTrue(self.core.authorize(intent, observation))
            self.assertEqual((intent['kind'], intent['value']), ('bed_target', 110))
            target = intent['value']
            observation.update(bed_target=target, bed_temp=target)
            self.core.acknowledge(intent, True, observation)
        data = dict(request_key='print-takes-manual', instance_id='quack-a', printer_id='P2S-test',
            parameters=dict(enabled=True, chamber_target=55, preheat_bed_target=110, print_bed_target=100))
        result = self.core.request(self.job, data, observation)
        self.assertFalse(result['ready'])
        self.assertEqual(result['expires_at'], manual['expires_at'])
        prepare = self.request(False)
        prepare['data']['preheat'] = dict(instance_id='quack-a', version=result['version'])
        with self.assertRaises(Conflict): self.store.provider_job(prepare)
        self.assertEqual(self.preheat_phase(), 'heating')
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM provider_dispatch').fetchone()[0], 0)
        # The normal job runtime completes warming and its print-bed transition.
        for _ in range(40):
            self.now += 15
            observation = self.manual_observation()
            observation.update(bed_target=target, bed_temp=target)
            for intent in self.core.tick(observation):
                self.assertTrue(self.core.authorize(intent, observation))
                self.assertGreater(intent['value'], 0)  # No cooldown during transfer.
                target = intent['value']
                observation.update(bed_target=target, bed_temp=target)
                self.core.acknowledge(intent, True, observation)
        self.assertEqual(target, 100)
        self.assertTrue(self.core.status(self.job, observation)['ready'])
        prepared = self.store.provider_job(prepare)
        self.assertEqual(prepared['provider_job']['status'], 'prepared')
        self.assertEqual(self.store.provider_job(prepare)['provider_job'], prepared['provider_job'])
        self.assertEqual(self.preheat_phase(), 'handoff')
        self.assertEqual(self.core.manual_status(observation)['phase'], 'transferred')
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM provider_dispatch').fetchone()[0], 1)
            self.assertEqual(db.execute("SELECT count(*) FROM events WHERE kind='job_preheat_manual_transfer'").fetchone()[0], 1)
            self.assertEqual(db.execute("SELECT count(*) FROM events WHERE kind='job_preheat_claim'").fetchone()[0], 1)
        self.assertEqual(self.store.snapshot()['spools'][0]['reserved_mg'], 20000)

    def test_cancelled_manual_preheat_allows_legacy_dispatch(self):
        response = self.manual_admission()
        self.core.cancel(self.manual, dict(request_key='manual-cancel', instance_id='ha-manual-a',
            version=response['version']), self.manual_observation())
        self.assertEqual(self.store.provider_job(self.request(False))['provider_job']['status'], 'prepared')

    def test_expired_manual_preheat_allows_legacy_dispatch_in_the_same_transaction(self):
        self.manual_admission()
        self.now += 2701
        self.assertEqual(self.store.provider_job(self.request(False))['provider_job']['status'], 'prepared')
        with self.store.connection() as db:
            row = db.execute('SELECT active,data FROM job_preheat WHERE job_uuid=?', (self.manual,)).fetchone()
            self.assertEqual(row['active'], 0)
            self.assertEqual(json.loads(row['data'])['phase'], 'expired')

    def test_stale_manual_telemetry_keeps_legacy_dispatch_blocked(self):
        self.manual_admission()
        self.now += 31
        with self.assertRaises(Conflict): self.store.provider_job(self.request(False))

    def test_manual_only_guard_blocks_a_persisted_job_mode_owner(self):
        self.admission()
        self.store.preheat = None
        self.store.preheat_guard = self.core
        self.store.settings['manual_preheat_enabled'] = True
        with self.assertRaises(Conflict): self.store.provider_job(self.request(False))
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM provider_dispatch').fetchone()[0], 0)

    def test_manual_only_runtime_without_active_occupant_keeps_legacy_prepare(self):
        self.store.preheat = None
        self.store.preheat_guard = self.core
        self.store.settings['manual_preheat_enabled'] = True
        self.assertEqual(self.store.provider_job(self.request(False))['provider_job']['status'], 'prepared')

    def test_manual_only_runtime_advertises_manual_without_job_protocol(self):
        self.store.preheat = None
        self.store.preheat_guard = self.core
        self.store.settings['manual_preheat_enabled'] = True
        for query in ({}, {'view': 'provider'}):
            result = materials(self.store, query)
            self.assertTrue(result['capabilities']['manual_preheat_v1'])
            self.assertNotIn('job_preheat_v1', result['capabilities'])
            self.assertNotIn('manual_preheat_enabled', result)
            self.assertNotIn('job_preheat_enabled', result)
            self.assertTrue(provider_view(result)['capabilities']['manual_preheat_v1'])
        self.store.settings['manual_preheat_enabled'] = False
        self.assertNotIn('manual_preheat_v1', materials(self.store, {})['capabilities'])
        self.store.settings['manual_preheat_enabled'] = True
        self.store.preheat_guard = None
        self.assertNotIn('manual_preheat_v1', materials(self.store, {})['capabilities'])

    def test_recovery_keeps_preheat_records_and_single_active_printer_index(self):
        self.admission()
        exported = export_recovery(self.store)
        restored = restore_recovery(exported, self.root/'restored.db', settings=self.settings)
        for table in ('job_preheat', 'job_preheat_receipts'):
            self.assertEqual(export_recovery(restored)['tables'][table], exported['tables'][table])
        with restored.connection() as db:
            self.assertIsNotNone(db.execute("SELECT 1 FROM sqlite_master WHERE type='index' AND name='job_preheat_printer'").fetchone())
        self.assertEqual(restored.snapshot()['spools'][0]['reserved_mg'], 10000)

    def test_legacy_recovery_does_not_create_preheat_tables_or_advertise_capability(self):
        # An older database has no optional coordinator tables.
        with self.store.connection() as db:
            db.execute('DROP TABLE IF EXISTS manual_preheat_attempts')
            db.execute('DROP TABLE job_preheat_receipts')
            db.execute('DROP TABLE job_preheat')
        self.store.preheat = None
        restored = restore_recovery(export_recovery(self.store), self.root/'legacy.db', settings=self.settings)
        with restored.connection() as db:
            self.assertIsNone(db.execute("SELECT 1 FROM sqlite_master WHERE name='job_preheat'").fetchone())
        self.assertNotIn('job_preheat_v1', materials(restored, {})['capabilities'])


if __name__ == '__main__': unittest.main()
