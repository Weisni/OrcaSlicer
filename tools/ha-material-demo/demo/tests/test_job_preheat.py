"""Job-bound preheat exercises real SQLite transactions with fake telemetry."""
import concurrent.futures
import copy
import json
import tempfile
import unittest
import uuid
from pathlib import Path

from custom_components.quack_materials.store import Store, Conflict
from custom_components.quack_materials.native_bridge import TABLES
from custom_components.quack_materials.preheat import Preheat


class PreheatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.now = 100000.0
        self.device_bed_target = 0
        self.device_bed_temp = 110
        self.store = Store(Path(self.temp.name) / 'preheat.db', seed_demo=False,
                           settings={'mode': 'pilot', 'provider_printer_id': 'P2S-test'})
        self.core = Preheat(self.store, clock=lambda: self.now)
        self.job = self.reserve()

    def reserve(self, printer='P2S-test', source='quack_provider', state='reserved'):
        ident = str(uuid.uuid4())
        with self.store.connection() as db:
            db.execute('INSERT INTO jobs VALUES (?,?,?,?,?,NULL,?,?)',
                       (ident, 'provider:' + ident, 'Plate', state, '[]', 'now', 'now'))
            db.execute('INSERT INTO origins VALUES (?,?)', (ident, source))
            row = db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
            bundle = json.loads(row['data']) if row else {'schema_version': 8, 'tables': {name: [] for name in TABLES}}
            bundle['tables']['print_jobs'].append(dict(id=ident, printer_id=printer, state=state, job_name='Plate', idempotency_key=ident))
            db.execute("INSERT OR REPLACE INTO bridge VALUES ('native',?)", (json.dumps(bundle),))
        return ident

    def observation(self, **changes):
        result = dict(printer_id='P2S-test', status='idle', chamber_temp=55.0,
                      bed_temp=self.device_bed_temp, bed_target=self.device_bed_target, door_closed=True, door_open_count=0,
                      air_mode='internal', idle_protection=True, maintenance_mode=False)
        for name in ('status', 'chamber_temp', 'bed_temp', 'bed_target', 'door_closed',
                     'air_mode', 'idle_protection', 'maintenance_mode'):
            result[name + '_at'] = self.now
        result.update(changes)
        return result

    def request(self, enabled=True, **changes):
        result = dict(request_key='start-' + self.job, instance_id='quack-1', printer_id='P2S-test',
                      parameters=dict(enabled=enabled, chamber_target=60,
                                      print_bed_target=100, preheat_bed_target=110))
        result.update(changes)
        return result

    def command(self, response, key='command', **changes):
        return dict(request_key=key, instance_id='quack-1', version=response['version'], **changes)

    def warm(self, seconds=600, temp=55.0, door_count=0):
        for step in range(0, seconds + 1, 15):
            if step:
                self.now += 15
            observation = self.observation(chamber_temp=temp, door_open_count=door_count)
            for intent in self.core.tick(observation):
                self.assertTrue(self.core.authorize(intent, observation))
                if intent['kind'] == 'bed_target':
                    self.device_bed_target = intent['value']
                    self.device_bed_temp = intent['value']
                self.core.acknowledge(intent, True, self.observation(chamber_temp=temp, door_open_count=door_count))
        return self.core.status(self.job, self.observation(chamber_temp=temp, door_open_count=door_count))

    def claim(self, response):
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            return self.core.claim_in_transaction(db, self.job,
                dict(instance_id='quack-1', version=response['version']))

    def test_admission_requires_reserved_provider_attempt_and_physical_identity(self):
        for job in (str(uuid.uuid4()), self.reserve(source='printer_observation'),
                    self.reserve(state='printing'), self.reserve(printer='foreign')):
            with self.assertRaises(Conflict):
                self.core.request(job, self.request(), self.observation())
        for changes in ({'status': 'running'}, {'status': 'pause'}, {'status_at': self.now - 31},
                        {'idle_protection': False}, {'maintenance_mode': True}, {'printer_id': 'foreign'}):
            with self.assertRaises(Conflict):
                self.core.request(self.job, self.request(), self.observation(**changes))

    def test_idempotency_parameter_identity_and_single_active_slot(self):
        first = self.core.request(self.job, self.request(), self.observation())
        self.assertEqual(first, self.core.request(self.job, self.request(), self.observation()))
        altered = self.request()
        altered['parameters']['chamber_target'] = 56
        with self.assertRaises(Conflict): self.core.request(self.job, altered, self.observation())
        with self.assertRaises(Conflict):
            self.core.request(self.job, self.request(instance_id='other'), self.observation())
        with self.assertRaises(Conflict):
            self.core.request(self.reserve(), self.request(), self.observation())
        self.assertEqual(first['expires_at'], self.now + 2700)

    def test_no_preheat_reads_never_heat_and_claim_is_atomic_once(self):
        before = self.store.snapshot()['spools']
        response = self.core.request(self.job, self.request(False), self.observation())
        self.assertTrue(response['ready'])
        self.assertEqual(self.core.tick(self.observation()), [])
        self.assertTrue(self.core.status(self.job, self.observation())['ready'])
        claimed = self.claim(response)
        self.assertEqual(claimed['phase'], 'handoff')
        with self.assertRaises(Conflict): self.claim(response)
        self.assertEqual(before, self.store.snapshot()['spools'])
        self.assertEqual(self.core.tick(self.observation()), [])

    def test_stability_target_range_slope_bed_and_door_recovery(self):
        self.core.request(self.job, self.request(), self.observation(chamber_temp=53))
        response = self.warm(600, temp=53)
        self.assertTrue(response['ready'])  # The accepted model permits a lower stable plateau.
        self.assertFalse(self.core.status(self.job, self.observation(bed_temp=90))['ready'])
        self.assertFalse(self.core.status(self.job, self.observation(door_open_count=1))['ready'])
        self.assertFalse(self.warm(585, door_count=1)['ready'])
        self.assertTrue(self.warm(15, door_count=1)['ready'])

    def test_restart_and_stale_gap_reset_window_and_no_status_reactivation(self):
        response = self.core.request(self.job, self.request(), self.observation())
        self.assertTrue(self.warm()['ready'])
        self.core = Preheat(self.store, clock=lambda: self.now)
        self.assertFalse(self.core.status(self.job, self.observation())['ready'])
        self.assertTrue(self.warm()['ready'])
        self.now += 31
        stale = self.observation(chamber_temp_at=self.now - 31)
        self.assertFalse(self.core.status(self.job, stale)['ready'])
        with self.assertRaises(Conflict): self.claim(response)
        self.assertEqual(self.core.tick(stale), [])
        self.assertFalse(self.core.status(self.job, self.observation())['ready'])

    def test_manual_takeover_cancellation_and_old_intent_revocation(self):
        response = self.core.request(self.job, self.request(), self.observation())
        intent = self.core.tick(self.observation())[0]
        self.assertTrue(self.core.authorize(intent, self.observation()))
        self.core.acknowledge(intent, True, self.observation())
        changed = self.core.status(self.job, self.observation(bed_target=90))
        self.assertEqual(changed['phase'], 'manual_takeover')
        self.assertEqual(self.core.tick(self.observation()), [])
        with self.assertRaises(Conflict): self.claim(response)
        self.assertFalse(self.core.authorize(intent, self.observation()))
        # A distinct attempt may now occupy the printer, then cancellation revokes its queued command.
        self.job = self.reserve()
        response = self.core.request(self.job, self.request(), self.observation())
        queued = self.core.tick(self.observation())[0]
        cancelled = self.core.cancel(self.job, self.command(response), self.observation())
        self.assertEqual(cancelled['phase'], 'cancelled')
        self.assertFalse(self.core.authorize(queued, self.observation()))
        self.core.acknowledge(queued, True, self.observation())
        self.assertEqual(self.core.tick(self.observation()), [])
        self.assertEqual(self.core.status(self.job, self.observation())['phase'], 'cancelled')

    def test_busy_protection_and_terminal_inventory_state_block_every_command(self):
        self.core.request(self.job, self.request(), self.observation())
        for changes in ({'status': 'paused'}, {'status': 'running'}, {'idle_protection': None},
                        {'maintenance_mode': True}, {'door_closed': None}, {'air_mode': None}):
            self.assertEqual(self.core.tick(self.observation(**changes)), [])
            self.assertFalse(self.core.status(self.job, self.observation(**changes))['ready'])
        with self.store.connection() as db: db.execute("UPDATE jobs SET state='completed' WHERE uuid=?", (self.job,))
        self.assertEqual(self.core.tick(self.observation()), [])
        self.assertEqual(self.core.status(self.job, self.observation())['phase'], 'cancelled')

    def test_deadline_extension_skip_retry_versions_and_idempotency(self):
        response = self.core.request(self.job, self.request(), self.observation())
        extended = self.core.extend(self.job, self.command(response, 'extend', seconds=600), self.observation())
        self.assertEqual(extended['expires_at'], response['expires_at'] + 600)
        self.assertEqual(extended, self.core.extend(self.job, self.command(response, 'extend', seconds=600), self.observation()))
        with self.assertRaises(Conflict): self.core.extend(self.job, self.command(extended, 'again', seconds=1), self.observation())
        with self.assertRaises(Conflict): self.core.skip(self.job, self.command(response, 'stale'), self.observation())
        skipped = self.core.skip(self.job, self.command(extended, 'skip'), self.observation())
        self.assertTrue(skipped['ready'])
        self.assertEqual(self.core.tick(self.observation()), [])
        self.now = extended['expires_at'] + 1
        self.assertEqual(self.core.status(self.job, self.observation())['phase'], 'expired')
        with self.assertRaises(Conflict): self.core.retry(self.job, self.command(skipped, 'retry'), self.observation())
        self.assertEqual(self.core.tick(self.observation()), [])

    def test_claim_checks_current_reserved_state_instance_version_and_observation(self):
        response = self.core.request(self.job, self.request(False), self.observation())
        for metadata in (dict(instance_id='foreign', version=response['version']),
                         dict(instance_id='quack-1', version=99)):
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE')
                with self.assertRaises(Conflict): self.core.claim_in_transaction(db, self.job, metadata)
        self.now += 31
        with self.assertRaises(Conflict): self.claim(response)
        self.core.status(self.job, self.observation())
        with self.store.connection() as db: db.execute("UPDATE jobs SET state='needs_review' WHERE uuid=?", (self.job,))
        with self.assertRaises(Conflict): self.claim(response)

    def test_sqlite_competing_admissions_and_claims_have_one_winner(self):
        other = self.reserve()
        def admission(job):
            try: return self.core.request(job, self.request(), self.observation())
            except Conflict: return None
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(admission, [self.job, other]))
        self.assertEqual(sum(value is not None for value in results), 1)
        # Use an independently created connection for each transaction.
        self.job = [self.job, other][next(i for i, value in enumerate(results) if value)]
        response = self.core.skip(self.job, self.command(next(value for value in results if value), 'skip'), self.observation())
        def claim(_):
            try: return self.claim(response)
            except Conflict: return None
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(claim, range(2)))
        self.assertEqual(sum(value is not None for value in results), 1)

    def test_disabled_preheat_uses_only_idle_door_identity_telemetry(self):
        observation = {name: value for name, value in self.observation().items()
                       if name in ('printer_id', 'status', 'status_at', 'door_closed', 'door_closed_at')}
        response = self.core.request(self.job, self.request(False), observation)
        self.assertTrue(response['ready'])
        self.assertEqual(self.core.tick(observation), [])
        self.assertEqual(self.claim(response)['phase'], 'handoff')

    def test_air_selection_waits_for_observed_internal_mode_before_heating(self):
        observation = self.observation(air_mode='external')
        self.core.request(self.job, self.request(), observation)
        intent = self.core.tick(observation)[0]
        self.assertEqual((intent['kind'], intent['value']), ('air_mode', 'internal'))
        self.assertTrue(self.core.authorize(intent, observation))
        self.core.acknowledge(intent, True, observation)
        self.assertFalse(self.core.status(self.job, observation)['ready'])
        observation = self.observation(air_mode='internal')
        following = self.core.tick(observation)
        self.assertEqual(len(following), 1)
        self.assertEqual(following[0]['kind'], 'bed_target')

    def test_ready_waits_for_final_bed_command_and_actual_cooling(self):
        self.core.request(self.job, self.request(), self.observation())
        captured = []
        for step in range(41):
            if step: self.now += 15
            observation = self.observation()
            for intent in self.core.tick(observation):
                if intent['kind'] == 'bed_target' and intent['value'] == 100:
                    captured.append(intent)
                else:
                    self.device_bed_target = intent['value']
                    self.core.acknowledge(intent, True, self.observation())
        self.assertEqual(len(captured), 1)
        intent = captured[0]
        self.assertEqual(self.core.status(self.job, self.observation())['phase'], 'conditioning_bed')
        self.assertTrue(self.core.authorize(intent, self.observation()))
        self.device_bed_target = 100
        self.core.acknowledge(intent, True, self.observation())
        self.assertFalse(self.core.status(self.job, self.observation())['ready'])
        self.device_bed_temp = 100
        self.assertTrue(self.core.status(self.job, self.observation())['ready'])

    def test_window_rejects_slope_and_range_and_never_reuses_long_gap(self):
        self.core.request(self.job, self.request(), self.observation())
        for step in range(41):
            if step: self.now += 15
            temp = 55 + step * 0.015  # 0.06 C/min despite range below 1 C.
            observation = self.observation(chamber_temp=temp)
            for intent in self.core.tick(observation):
                self.device_bed_target = intent['value']
                self.core.acknowledge(intent, True, self.observation(chamber_temp=temp))
        self.assertFalse(self.core.status(self.job, self.observation(chamber_temp=temp))['ready'])
        for step in range(41):
            self.now += 15
            observation = self.observation(chamber_temp=55 + (step % 2) * 2)
            for intent in self.core.tick(observation):
                self.device_bed_target = intent['value']
                self.core.acknowledge(intent, True, observation)
        self.assertFalse(self.core.status(self.job, self.observation())['ready'])
        self.now += 120
        self.assertFalse(self.core.status(self.job, self.observation())['ready'])
        self.assertFalse(self.warm(585)['ready'])
        self.assertTrue(self.warm(15)['ready'])

    def test_failed_heater_requires_explicit_retry_and_old_intent_stays_revoked(self):
        response = self.core.request(self.job, self.request(), self.observation())
        intent = self.core.tick(self.observation())[0]
        self.core.acknowledge(intent, False)
        self.now += 60
        self.assertEqual(self.core.tick(self.observation()), [])
        self.assertEqual(self.core.status(self.job, self.observation())['reason'], 'heater_command_failed')
        retried = self.core.retry(self.job, self.command(response, 'retry'), self.observation())
        self.assertEqual(retried['version'], response['version'] + 1)
        self.assertFalse(self.core.authorize(intent, self.observation()))
        self.assertEqual(len(self.core.tick(self.observation())), 1)

    def test_claim_rollback_preserves_readiness_and_inventory_and_history(self):
        with self.store.connection() as db:
            db.execute("INSERT INTO spools VALUES ('sentinel', '{}', 987654)")
            self.store.event(db, 'unrelated_history', dict(evidence='retain'))
            protected = list(db.execute('SELECT uuid,remaining_mg FROM spools'))
        response = self.core.request(self.job, self.request(False), self.observation())
        try:
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE')
                self.core.claim_in_transaction(db, self.job, dict(instance_id='quack-1', version=response['version']))
                raise RuntimeError('Later material binding check fails')
        except RuntimeError:
            pass
        self.assertTrue(self.core.status(self.job, self.observation())['ready'])
        with self.store.connection() as db:
            self.assertEqual([tuple(row) for row in protected], [tuple(row) for row in db.execute('SELECT uuid,remaining_mg FROM spools')])
            self.assertEqual(db.execute("SELECT count(*) FROM events WHERE kind='unrelated_history'").fetchone()[0], 1)
            self.assertEqual(db.execute("SELECT count(*) FROM events WHERE kind='job_preheat_claim'").fetchone()[0], 0)
        self.assertEqual(self.claim(response)['phase'], 'handoff')

    def test_restart_cannot_directly_claim_persisted_ready(self):
        response = self.core.request(self.job, self.request(False), self.observation())
        self.core = Preheat(self.store, clock=lambda: self.now)
        with self.assertRaises(Conflict): self.claim(response)
        response = self.core.status(self.job, self.observation())
        self.assertEqual(self.claim(response)['phase'], 'handoff')

    def test_forged_or_modified_command_intents_cannot_authorize(self):
        self.core.request(self.job, self.request(), self.observation())
        intent = self.core.tick(self.observation())[0]
        for field, value in (('value', 109), ('version', 99), ('generation', 'old'),
                             ('command_id', 'foreign'), ('kind', 'print')):
            altered = dict(intent, **{field: value})
            self.assertFalse(self.core.authorize(altered, self.observation()))
        self.assertTrue(self.core.authorize(intent, self.observation()))

    def test_obsolete_pending_target_is_replaced_after_readiness_changes(self):
        self.core.request(self.job, self.request(), self.observation())
        old = self.core.tick(self.observation())[0]
        self.device_bed_target = 110
        for step in range(40):
            self.now += 15
            self.core.status(self.job, self.observation())
        self.assertFalse(self.core.authorize(old, self.observation()))
        following = self.core.tick(self.observation())
        self.assertEqual(len(following), 1)
        self.assertEqual((following[0]['kind'], following[0]['value']), ('bed_target', 100))

    def test_disabled_zero_target_and_read_only_summaries(self):
        request = self.request(False)
        request['parameters']['chamber_target'] = 0
        response = self.core.request(self.job, request, self.observation())
        self.assertTrue(response['ready'])
        with self.store.connection() as db:
            before = list(db.iterdump())
        self.now += 100
        summary = self.core.summaries()
        self.assertEqual(len(summary), 1)
        self.assertEqual(summary[0]['job_uuid'], self.job)
        self.assertFalse(summary[0]['ready'])
        with self.store.connection() as db:
            self.assertEqual(before, list(db.iterdump()))
            self.assertEqual(self.core.summaries(db), summary)

    def test_handoff_keeps_printer_reserved_until_actual_inventory_settlement(self):
        response = self.core.request(self.job, self.request(False), self.observation())
        self.claim(response)
        other = self.reserve()
        with self.store.connection() as db:
            db.execute("UPDATE jobs SET state='printing' WHERE uuid=?", (self.job,))
        self.assertEqual(self.core.tick(self.observation(status='running')), [])
        with self.assertRaises(Conflict): self.core.request(other, self.request(False, request_key='other-start'), self.observation())
        with self.store.connection() as db:
            db.execute("UPDATE jobs SET state='failed',settlement='{}' WHERE uuid=?", (self.job,))
        self.assertEqual(self.core.tick(self.observation()), [])
        self.assertEqual(self.core.request(other, self.request(False, request_key='other-start'), self.observation())['job_uuid'], other)

    def test_recovery_schema_constants_recreate_tables_and_active_printer_index(self):
        import sqlite3
        from custom_components.quack_materials.preheat import SCHEMAS, INDEXES
        with sqlite3.connect(':memory:') as db:
            self.addCleanup(db.close)
            for statement in SCHEMAS.values(): db.execute(statement)
            for statement in INDEXES.values(): db.execute(statement)
            db.execute("INSERT INTO job_preheat VALUES ('a','printer',1,'{}')")
            with self.assertRaises(sqlite3.IntegrityError):
                db.execute("INSERT INTO job_preheat VALUES ('b','printer',1,'{}')")

    def test_brief_door_open_requires_recovery_to_pre_open_temperature(self):
        self.core.request(self.job, self.request(), self.observation())
        self.assertTrue(self.warm()['ready'])
        # No poll observed the open door; its edge counter still revokes readiness.
        self.assertFalse(self.core.status(self.job, self.observation(chamber_temp=50, door_open_count=1))['ready'])
        self.assertFalse(self.warm(615, temp=50, door_count=1)['ready'])
        self.assertFalse(self.warm(585, temp=55, door_count=1)['ready'])
        self.assertTrue(self.warm(15, temp=55, door_count=1)['ready'])

    def test_observed_open_door_preserves_pre_open_recovery_temperature(self):
        self.core.request(self.job, self.request(), self.observation())
        self.assertTrue(self.warm()['ready'])
        observation = self.observation(chamber_temp=52, door_closed=False, door_open_count=1)
        intents = self.core.tick(observation)
        self.assertEqual([(i['kind'], i['value']) for i in intents], [('bed_target', 110)])
        self.device_bed_target = 110
        self.core.acknowledge(intents[0], True, observation)
        self.assertFalse(self.warm(615, temp=52, door_count=1)['ready'])

    def test_manual_skip_conditions_owned_bed_before_ready(self):
        response = self.core.request(self.job, self.request(), self.observation())
        intent = self.core.tick(self.observation())[0]
        self.device_bed_target = 110
        self.core.acknowledge(intent, True, self.observation())
        response = self.core.skip(self.job, self.command(response, 'skip'), self.observation())
        self.assertFalse(response['ready'])
        self.assertEqual(response['phase'], 'conditioning_bed')
        self.assertEqual(self.core.tick(self.observation(idle_protection=False)), [])
        intent = self.core.tick(self.observation())[0]
        self.assertEqual((intent['kind'], intent['value']), ('bed_target', 100))
        self.assertTrue(self.core.authorize(intent, self.observation()))
        self.device_bed_target = 100
        self.core.acknowledge(intent, True, self.observation())
        self.assertFalse(self.core.status(self.job, self.observation())['ready'])
        self.device_bed_temp = 100
        response = self.core.status(self.job, self.observation())
        self.assertTrue(response['ready'])
        self.assertEqual(response['reason'], 'manual_override')

    def test_transactional_claim_reads_trusted_observation_after_cached_readiness(self):
        response = self.core.request(self.job, self.request(False), self.observation())
        self.assertTrue(response['ready'])
        for changes in ({'status': 'running'}, {'status': 'pause'},
                        {'status_at': self.now - 31}, {'door_closed': False}):
            with self.subTest(changes=changes):
                self.core.observation_reader = lambda changes=changes: self.observation(**changes)
                with self.assertRaises(Conflict):
                    with self.store.connection() as db:
                        db.execute('BEGIN IMMEDIATE')
                        self.core.claim_in_transaction(db, self.job,
                            dict(instance_id='quack-1', version=response['version']))
                        db.execute('INSERT INTO provider_dispatch VALUES (?,?,?,?,?,?)',
                                   (self.job, 'P2S-test', 'prepared', 'Plate', '[]', 'now'))
                with self.store.connection() as db:
                    self.assertEqual(db.execute('SELECT count(*) FROM provider_dispatch').fetchone()[0], 0)
        self.core.observation_reader = lambda: self.observation()
        self.assertEqual(self.claim(response)['phase'], 'handoff')

    def test_transactional_reader_revokes_warmed_readiness_for_new_door_or_protection(self):
        self.core.request(self.job, self.request(), self.observation())
        response = self.warm()
        self.assertTrue(response['ready'])
        for changes in ({'door_open_count': 1}, {'idle_protection': False},
                        {'maintenance_mode': True}, {'chamber_temp_at': self.now - 31}):
            with self.subTest(changes=changes):
                self.core.observation_reader = lambda changes=changes: self.observation(**changes)
                with self.assertRaises(Conflict): self.claim(response)

    def test_transactional_reader_failure_or_unknown_observation_never_grants(self):
        response = self.core.request(self.job, self.request(False), self.observation())
        self.core.observation_reader = lambda: None
        with self.assertRaises(Conflict): self.claim(response)
        def unavailable():
            raise RuntimeError('Trusted telemetry reader failed')
        self.core.observation_reader = unavailable
        with self.assertRaisesRegex(RuntimeError, 'Trusted telemetry reader failed'):
            self.claim(response)

    def test_commanded_bed_cooling_latches_proven_plateau_until_handoff(self):
        self.core.request(self.job, self.request(), self.observation())
        for step in range(41):
            if step: self.now += 15
            observation = self.observation()
            for intent in self.core.tick(observation):
                self.device_bed_target = intent['value']
                self.core.acknowledge(intent, True, self.observation())
        self.assertEqual(self.device_bed_target, 100)
        self.assertEqual(self.core.status(self.job, self.observation())['phase'], 'conditioning_bed')
        # Lowering the bed intentionally cools the chamber too. Its proven
        # warm-up plateau remains valid while the actual bed approaches 100 C.
        for step in range(12):
            self.now += 15
            self.device_bed_temp = 105
            observation = self.observation(chamber_temp=54)
            for intent in self.core.tick(observation):
                self.assertEqual((intent['kind'], intent['value']), ('bed_target', 100))
                self.core.acknowledge(intent, True, observation)
            self.assertEqual(self.core.status(self.job, observation)['phase'], 'conditioning_bed')
        self.now += 15
        self.device_bed_temp = 100
        observation = self.observation(chamber_temp=53)
        for intent in self.core.tick(observation):
            self.assertEqual((intent['kind'], intent['value']), ('bed_target', 100))
            self.core.acknowledge(intent, True, observation)
        self.assertTrue(self.core.status(self.job, observation)['ready'])

    def manual_request(self, ident, **changes):
        request = self.request(request_key='manual-' + ident)
        request['parameters']['print_bed_target'] = request['parameters']['preheat_bed_target']
        request.update(changes)
        return request

    def test_manual_request_has_separate_immutable_identity_and_no_inventory_mutations(self):
        ident = str(uuid.uuid4())
        with self.store.connection() as db:
            before = {name: [tuple(row) for row in db.execute('SELECT * FROM ' + name)]
                      for name in ('jobs', 'origins', 'provider_dispatch', 'spools', 'slots', 'bridge')}
        self.assertIsNone(self.core.manual_status(self.observation()))
        response = self.core.request_manual(ident, self.manual_request(ident), self.observation())
        self.assertEqual(response['job_uuid'], ident)
        self.assertFalse(response['ready'])
        self.assertEqual(self.core.manual_status(self.observation()), response)
        with self.store.connection() as db:
            for name, rows in before.items():
                self.assertEqual(rows, [tuple(row) for row in db.execute('SELECT * FROM ' + name)])
            ledger = db.execute('SELECT * FROM manual_preheat_attempts').fetchall()
            self.assertEqual(len(ledger), 1)
            self.assertEqual(ledger[0]['job_uuid'], ident)
            self.assertEqual(ledger[0]['printer_id'], 'P2S-test')
        intent = self.core.tick(self.observation())[0]
        self.assertEqual(intent['kind'], 'bed_target')
        self.assertEqual(intent['value'], 110)

    def test_manual_and_provider_share_one_physical_slot_and_routes_cannot_be_mixed(self):
        ident = str(uuid.uuid4())
        request = self.manual_request(ident)
        response = self.core.request_manual(ident, request, self.observation())
        successor = self.core.request(self.job, self.request(False), self.observation())
        self.assertTrue(successor['ready'])
        self.assertEqual(self.core.manual_status(self.observation())['phase'], 'transferred')
        other = str(uuid.uuid4())
        with self.assertRaises(Conflict): self.core.request_manual(other, self.manual_request(other), self.observation())
        with self.assertRaises(Conflict): self.core.request(ident, request, self.observation())
        with self.assertRaises(Conflict): self.core.request_manual(self.job, self.manual_request(self.job), self.observation())
        with self.assertRaises(Conflict):
            self.core.cancel(ident, self.command(response, 'cancel-manual'), self.observation())
        self.core.request(self.job, self.request(False), self.observation())
        with self.assertRaises(Conflict): self.core.request_manual(other, self.manual_request(other), self.observation())

    def start_manual_for_handoff(self, *, warm_seconds=None):
        provider = self.job
        manual = str(uuid.uuid4())
        response = self.core.request_manual(manual, self.manual_request(manual), self.observation())
        if warm_seconds is not None:
            self.job = manual
            response = self.warm(warm_seconds)
            self.job = provider
        return manual, response

    def test_print_atomically_takes_manual_ownership_and_revokes_old_controls(self):
        manual, response = self.start_manual_for_handoff()
        pending = self.core.tick(self.observation())[0]
        successor = self.core.request(self.job, self.request(), self.observation())
        transferred = self.core.manual_status(self.observation())
        self.assertEqual((transferred['phase'], transferred['reason']), ('transferred', 'print_preheat_claimed'))
        self.assertEqual(transferred['successor_job_uuid'], self.job)
        self.assertFalse(transferred['ready'])
        self.assertEqual(successor['expires_at'], response['expires_at'])
        self.assertFalse(self.core.authorize(pending, self.observation()))
        self.core.acknowledge(pending, True, self.observation())
        with self.assertRaises(Conflict):
            self.core.cancel(manual, self.command(response, 'old-cancel'), self.observation())
        replay = self.core.request_manual(manual, self.manual_request(manual), self.observation())
        self.assertEqual(replay['phase'], 'transferred')
        self.assertEqual(self.core.request(self.job, self.request(), self.observation()), successor)
        intents = self.core.tick(self.observation())
        self.assertEqual([(intent['job_uuid'], intent['kind'], intent['value']) for intent in intents],
                         [(self.job, 'bed_target', 110)])
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT job_uuid FROM job_preheat WHERE active=1').fetchone()[0], self.job)
            self.assertEqual(db.execute('SELECT count(*) FROM provider_dispatch').fetchone()[0], 0)
            self.assertEqual(db.execute('SELECT state FROM jobs WHERE uuid=?', (self.job,)).fetchone()[0], 'reserved')

    def test_handoff_reuses_compatible_plateau_but_conditions_print_bed(self):
        manual, response = self.start_manual_for_handoff(warm_seconds=600)
        self.assertTrue(response['ready'])
        result = self.core.request(self.job, self.request(), self.observation())
        self.assertEqual(result['phase'], 'conditioning_bed')
        self.assertFalse(result['ready'])
        with self.assertRaises(Conflict): self.claim(result)
        self.assertTrue(self.warm(0)['ready'])
        self.assertEqual(self.device_bed_target, 100)
        self.assertEqual(self.claim(result)['phase'], 'handoff')
        self.assertEqual(self.core.status(manual, self.observation())['phase'], 'transferred')

    def test_handoff_continues_compatible_partial_window(self):
        self.start_manual_for_handoff(warm_seconds=300)
        result = self.core.request(self.job, self.request(), self.observation())
        self.assertFalse(result['ready'])
        self.assertFalse(self.warm(285)['ready'])
        self.assertTrue(self.warm(15)['ready'])

    def test_handoff_discards_plateau_for_changed_chamber_target(self):
        self.start_manual_for_handoff(warm_seconds=600)
        request = self.request()
        request['parameters']['chamber_target'] = 65
        result = self.core.request(self.job, request, self.observation())
        self.assertFalse(result['ready'])
        self.assertFalse(self.warm(585)['ready'])
        self.assertTrue(self.warm(15)['ready'])

    def test_handoff_discards_plateau_for_changed_preheat_bed_target(self):
        self.start_manual_for_handoff(warm_seconds=600)
        request = self.request()
        request['parameters']['preheat_bed_target'] = 105
        result = self.core.request(self.job, request, self.observation())
        self.assertFalse(result['ready'])
        self.assertFalse(self.warm(585)['ready'])
        self.assertTrue(self.warm(15)['ready'])

    def test_handoff_rechecks_plateau_after_a_telemetry_gap(self):
        self.start_manual_for_handoff(warm_seconds=600)
        self.now += 31
        result = self.core.request(self.job, self.request(), self.observation())
        self.assertFalse(result['ready'])
        self.assertFalse(self.warm(585)['ready'])
        self.assertTrue(self.warm(15)['ready'])

    def test_handoff_after_restart_cannot_inherit_old_readiness(self):
        self.start_manual_for_handoff(warm_seconds=600)
        self.core = Preheat(self.store, clock=lambda: self.now)
        result = self.core.request(self.job, self.request(), self.observation())
        self.assertFalse(result['ready'])
        self.assertFalse(self.warm(585)['ready'])
        self.assertTrue(self.warm(15)['ready'])

    def test_terminal_manual_attempt_is_not_transferred_or_revived(self):
        manual, response = self.start_manual_for_handoff(warm_seconds=600)
        self.core.cancel(manual, self.command(response, 'cancel-manual'), self.observation())
        result = self.core.request(self.job, self.request(), self.observation())
        self.assertFalse(result['ready'])
        self.assertEqual(result['expires_at'], self.now + 2700)
        terminal = self.core.request_manual(manual, self.manual_request(manual), self.observation())
        self.assertEqual(terminal['phase'], 'cancelled')
        self.assertNotIn('successor_job_uuid', terminal)
        self.assertFalse(self.warm(585)['ready'])
        self.assertTrue(self.warm(15)['ready'])

    def test_handoff_preserves_total_lifetime_and_consumed_extension(self):
        manual, response = self.start_manual_for_handoff()
        response = self.core.extend(manual, self.command(response, 'manual-extend', seconds=600), self.observation())
        self.now += 200
        result = self.core.request(self.job, self.request(), self.observation())
        self.assertEqual(result['expires_at'], 103300.0)
        with self.assertRaises(Conflict):
            self.core.extend(self.job, self.command(result, 'print-extend', seconds=900), self.observation())
        self.now = 103301.0
        self.assertEqual(self.core.status(self.job, self.observation())['phase'], 'expired')
        self.assertEqual(self.core.tick(self.observation()), [])
        self.assertEqual(self.core.request(self.job, self.request(), self.observation())['phase'], 'expired')
        self.assertEqual(self.core.status(manual, self.observation())['phase'], 'transferred')

    def test_handoff_rejects_unsafe_or_unreserved_requests_without_releasing_manual(self):
        manual, _ = self.start_manual_for_handoff()
        pending = self.core.tick(self.observation())[0]
        for observation in (self.observation(status='running'), self.observation(door_closed=False),
                            self.observation(chamber_temp_at=self.now - 31), self.observation(idle_protection=False),
                            self.observation(maintenance_mode=True), self.observation(printer_id='foreign')):
            with self.assertRaises(Conflict): self.core.request(self.job, self.request(), observation)
        for ident in (str(uuid.uuid4()), self.reserve(printer='foreign'), self.reserve(source='printer_observation')):
            with self.assertRaises(Conflict): self.core.request(ident, self.request(), self.observation())
        self.assertEqual(self.core.manual_status(self.observation())['job_uuid'], manual)
        self.assertTrue(self.core.authorize(pending, self.observation()))
        with self.store.connection() as db:
            self.assertIsNone(db.execute('SELECT 1 FROM job_preheat WHERE job_uuid=?', (self.job,)).fetchone())

    def test_handoff_never_silently_retries_failed_heater(self):
        manual, _ = self.start_manual_for_handoff()
        pending = self.core.tick(self.observation())[0]
        self.core.acknowledge(pending, False)
        with self.assertRaises(Conflict): self.core.request(self.job, self.request(), self.observation())
        self.assertEqual(self.core.status(manual, self.observation())['reason'], 'heater_command_failed')
        self.assertEqual(self.core.tick(self.observation()), [])

    def test_handoff_preserves_reset_acknowledgement_deadline(self):
        manual, _ = self.start_manual_for_handoff(warm_seconds=30)
        self.device_bed_target = 0
        self.now += 1
        intent = self.core.tick(self.observation())[0]
        self.core.acknowledge(intent, True, self.observation())
        self.now += 29
        self.core.request(self.job, self.request(), self.observation())
        self.now += 2
        result = self.core.status(self.job, self.observation())
        self.assertEqual((result['phase'], result['reason']), ('interrupted', 'heater_command_failed'))
        self.assertEqual(self.core.tick(self.observation()), [])
        self.assertEqual(self.core.status(manual, self.observation())['phase'], 'transferred')

    def test_handoff_rollback_restores_manual_owner_and_receipts(self):
        import sqlite3
        manual, response = self.start_manual_for_handoff()
        with self.store.connection() as db:
            db.execute("CREATE TRIGGER fail_handoff BEFORE INSERT ON job_preheat WHEN NEW.job_uuid='" + self.job +
                       "' BEGIN SELECT RAISE(ABORT,'injected handoff failure'); END")
        with self.assertRaises(sqlite3.IntegrityError):
            self.core.request(self.job, self.request(), self.observation())
        self.assertEqual(self.core.manual_status(self.observation()), response)
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM job_preheat_receipts').fetchone()[0], 1)
            db.execute('DROP TRIGGER fail_handoff')
        self.assertFalse(self.core.request(self.job, self.request(), self.observation())['ready'])
        self.assertEqual(self.core.status(manual, self.observation())['phase'], 'transferred')

    def test_competing_print_handoffs_have_one_successor_and_no_queue(self):
        manual, _ = self.start_manual_for_handoff()
        other = self.reserve()
        def admission(ident):
            try: return self.core.request(ident, self.request(request_key='start-' + ident), self.observation())
            except Conflict: return None
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            responses = list(pool.map(admission, [self.job, other]))
        self.assertEqual(sum(response is not None for response in responses), 1)
        winner = next(response for response in responses if response)['job_uuid']
        self.assertEqual(self.core.status(manual, self.observation())['successor_job_uuid'], winner)
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM job_preheat WHERE active=1').fetchone()[0], 1)
            self.assertEqual(db.execute('SELECT count(*) FROM job_preheat').fetchone()[0], 2)

    def test_manual_lost_reply_replays_identity_and_terminal_cancel_reason(self):
        ident = str(uuid.uuid4())
        request = self.manual_request(ident)
        response = self.core.request_manual(ident, request, self.observation())
        self.assertEqual(response, self.core.request_manual(ident, request, self.observation()))
        cancelled = self.core.cancel(ident, self.command(response, 'cancel-manual'), self.observation())
        self.assertEqual(cancelled, self.core.request_manual(ident, request, self.observation()))
        self.assertEqual(self.core.manual_status(self.observation())['reason'], 'user_cancelled')
        self.assertEqual(self.core.tick(self.observation()), [])
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM manual_preheat_attempts').fetchone()[0], 1)

    def test_manual_parameters_hold_bed_and_reject_disabled_or_unequal_targets(self):
        for change in ({'enabled': False}, {'print_bed_target': 100}, {'preheat_bed_target': 111}):
            ident = str(uuid.uuid4())
            request = self.manual_request(ident)
            request['parameters'].update(change)
            with self.assertRaises(ValueError): self.core.request_manual(ident, request, self.observation())
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM manual_preheat_attempts').fetchone()[0], 0)

    def test_manual_restart_and_ready_never_allow_print_claim(self):
        self.job = str(uuid.uuid4())
        self.core.request_manual(self.job, self.manual_request(self.job), self.observation())
        response = self.warm()
        self.assertTrue(response['ready'])
        self.assertEqual(self.device_bed_target, 110)
        with self.assertRaises(Conflict): self.claim(response)
        self.core = Preheat(self.store, clock=lambda: self.now)
        self.assertFalse(self.core.manual_status(self.observation())['ready'])
        self.assertTrue(self.warm()['ready'])
        with self.assertRaises(Conflict): self.claim(response)
        self.assertEqual(self.device_bed_target, 110)

    def test_manual_guard_keeps_stale_active_attempt_and_releases_expired_without_heating(self):
        ident = str(uuid.uuid4())
        response = self.core.request_manual(ident, self.manual_request(ident), self.observation())
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            self.assertTrue(self.core.has_active_manual_in_transaction(db, 'P2S-test'))
            self.assertFalse(self.core.has_active_manual_in_transaction(db, 'foreign'))
        self.now += 31
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            self.assertTrue(self.core.has_active_manual_in_transaction(db, 'P2S-test'))
        self.now = response['expires_at'] + 1
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            self.assertFalse(self.core.has_active_manual_in_transaction(db, 'P2S-test'))
        self.assertEqual(self.core.manual_status(self.observation())['phase'], 'expired')
        self.assertEqual(self.core.tick(self.observation()), [])

    def test_manual_door_and_protection_use_existing_heating_interlocks(self):
        self.job = str(uuid.uuid4())
        self.core.request_manual(self.job, self.manual_request(self.job), self.observation())
        self.assertTrue(self.warm()['ready'])
        self.assertEqual(self.core.tick(self.observation(door_closed=False, door_open_count=1)), [])
        self.assertFalse(self.core.manual_status(self.observation(chamber_temp=50, door_open_count=1))['ready'])
        self.assertEqual(self.core.tick(self.observation(idle_protection=False, door_open_count=1)), [])
        self.assertFalse(self.core.manual_status(self.observation(idle_protection=False, door_open_count=1))['ready'])

    def test_competing_manual_and_provider_admissions_leave_only_the_print_owner(self):
        ident = str(uuid.uuid4())
        def admission(manual):
            try:
                if manual: return self.core.request_manual(ident, self.manual_request(ident), self.observation())
                return self.core.request(self.job, self.request(False), self.observation())
            except Conflict:
                return None
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            responses = list(pool.map(admission, [True, False]))
        # Manual-first can return success before the print atomically takes
        # over. Print-first rejects manual admission. Both orders leave one owner.
        self.assertIsNotNone(responses[1])
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM job_preheat WHERE active=1').fetchone()[0], 1)
            self.assertEqual(db.execute('SELECT job_uuid FROM job_preheat WHERE active=1').fetchone()[0], self.job)
            self.assertEqual(db.execute('SELECT count(*) FROM jobs').fetchone()[0], 1)

    def test_legacy_job_foreign_key_migration_preserves_all_rows_receipts_and_index(self):
        import sqlite3
        with self.store.connection() as db:
            db.execute('DROP TABLE job_preheat')
            db.execute('CREATE TABLE job_preheat(job_uuid TEXT PRIMARY KEY REFERENCES jobs(uuid), '
                       'printer_id TEXT NOT NULL, active INTEGER NOT NULL, data TEXT NOT NULL)')
            db.execute('CREATE UNIQUE INDEX job_preheat_printer ON job_preheat(printer_id) WHERE active=1')
        response = self.core.request(self.job, self.request(False), self.observation())
        self.core.cancel(self.job, self.command(response, 'provider-cancel'), self.observation())
        other = self.reserve()
        self.core.request(other, self.request(False, request_key='second-provider'), self.observation())
        with self.store.connection() as db:
            before = [tuple(row) for row in db.execute('SELECT * FROM job_preheat ORDER BY job_uuid')]
            receipts = [tuple(row) for row in db.execute('SELECT * FROM job_preheat_receipts ORDER BY request_key')]
            self.assertTrue(db.execute('PRAGMA foreign_key_list(job_preheat)').fetchall())
        self.core = Preheat(self.store, clock=lambda: self.now)
        with self.store.connection() as db:
            self.assertEqual(db.execute('PRAGMA foreign_key_list(job_preheat)').fetchall(), [])
            self.assertEqual(before, [tuple(row) for row in db.execute('SELECT * FROM job_preheat ORDER BY job_uuid')])
            self.assertEqual(receipts, [tuple(row) for row in db.execute('SELECT * FROM job_preheat_receipts ORDER BY request_key')])
            with self.assertRaises(sqlite3.IntegrityError):
                db.execute('INSERT INTO job_preheat VALUES (?,?,1,?)', (str(uuid.uuid4()), 'P2S-test', '{}'))
            self.assertEqual(db.execute('PRAGMA foreign_key_check').fetchall(), [])

    def test_manual_recovery_roundtrip_preserves_ledgers_and_printer_exclusion(self):
        from custom_components.quack_materials.recovery import export_recovery, restore_recovery
        ident = str(uuid.uuid4())
        self.core.request_manual(ident, self.manual_request(ident), self.observation())
        bundle = export_recovery(self.store)
        self.assertIn('manual_preheat_attempts', bundle['tables'])
        restored = restore_recovery(bundle, Path(self.temp.name) / 'restored.db', settings=self.store.settings)
        core = Preheat(restored, clock=lambda: self.now)
        self.assertEqual(core.manual_status(self.observation())['job_uuid'], ident)
        self.assertTrue(core.request(self.job, self.request(False), self.observation())['ready'])
        self.assertEqual(core.manual_status(self.observation())['phase'], 'transferred')
        with restored.connection() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM jobs').fetchone()[0], 1)
            self.assertEqual(db.execute('PRAGMA foreign_key_list(job_preheat)').fetchall(), [])

    def test_mode_switch_revokes_provider_heating_without_changing_reservation(self):
        response = self.core.request(self.job, self.request(), self.observation())
        intent = self.core.tick(self.observation())[0]
        self.device_bed_target = 110
        self.core.acknowledge(intent, True, self.observation())
        with self.store.connection() as db:
            before = [tuple(row) for row in db.execute('SELECT * FROM jobs')]
        self.core = Preheat(self.store, clock=lambda: self.now)
        changes = self.core.reconcile_modes(job_enabled=False, manual_enabled=True)
        self.assertEqual(len(changes), 1)
        state = self.core.status(self.job, self.observation())
        self.assertEqual((state['phase'], state['reason']), ('cancelled', 'mode_disabled'))
        self.assertEqual(state['version'], response['version'] + 1)
        self.assertEqual(self.core.tick(self.observation()), [])
        self.assertFalse(self.core.authorize(intent, self.observation()))
        with self.store.connection() as db:
            self.assertEqual(before, [tuple(row) for row in db.execute('SELECT * FROM jobs')])
        self.assertEqual(self.core.reconcile_modes(job_enabled=False, manual_enabled=True), [])

    def test_mode_switch_revokes_manual_only_when_its_kind_is_disabled(self):
        ident = str(uuid.uuid4())
        self.core.request_manual(ident, self.manual_request(ident), self.observation())
        self.assertEqual(self.core.reconcile_modes(job_enabled=False, manual_enabled=True), [])
        self.core = Preheat(self.store, clock=lambda: self.now)
        self.core.reconcile_modes(job_enabled=True, manual_enabled=False)
        state = self.core.manual_status(self.observation())
        self.assertEqual((state['phase'], state['reason']), ('cancelled', 'mode_disabled'))
        self.assertEqual(self.core.tick(self.observation()), [])

    def test_legacy_guard_conservatively_blocks_provider_owner_and_handoff(self):
        response = self.core.request(self.job, self.request(False), self.observation())
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            self.assertTrue(self.core.has_active_manual_in_transaction(db, 'P2S-test'))
        self.claim(response)
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            self.assertTrue(self.core.has_active_manual_in_transaction(db, 'P2S-test'))

    def test_manual_admission_blocks_uncertain_legacy_dispatch_even_if_printer_still_idle(self):
        for status in ('prepared', 'accepted', 'uncertain', 'observing'):
            with self.subTest(status=status):
                with self.store.connection() as db:
                    db.execute('INSERT OR REPLACE INTO provider_dispatch VALUES (?,?,?,?,?,?)',
                               (self.job, 'P2S-test', status, 'Plate', '[]', 'now'))
                ident = str(uuid.uuid4())
                with self.assertRaises(Conflict):
                    self.core.request_manual(ident, self.manual_request(ident), self.observation())
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM manual_preheat_attempts').fetchone()[0], 0)

    def test_manual_admission_blocks_unresolved_or_busy_inventory_printer_observation(self):
        for state, job in (('idle', self.job), ('running', None), ('pause', None), ('prepare', None)):
            with self.subTest(state=state, job=job):
                with self.store.connection() as db:
                    db.execute('INSERT OR REPLACE INTO observations VALUES (?,?,?)', ('duck-poop', state, job))
                ident = str(uuid.uuid4())
                with self.assertRaises(Conflict):
                    self.core.request_manual(ident, self.manual_request(ident), self.observation())
        with self.store.connection() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM manual_preheat_attempts').fetchone()[0], 0)

    def test_manual_admission_diagnostic_is_read_only_and_never_reuses_permission(self):
        with self.store.connection() as db:
            before = list(db.iterdump())
        self.assertIsNone(self.core.manual_admission_reason(self.observation()))
        self.assertEqual(self.core.manual_admission_reason(self.observation(status='running')), 'printer_busy_or_unknown')
        self.assertEqual(self.core.manual_admission_reason(self.observation(idle_protection=False)), 'idle_protection_unavailable')
        with self.store.connection() as db:
            self.assertEqual(before, list(db.iterdump()))
            db.execute('INSERT INTO provider_dispatch VALUES (?,?,?,?,?,?)',
                       (self.job, 'P2S-test', 'uncertain', 'Plate', '[]', 'now'))
        self.assertEqual(self.core.manual_admission_reason(self.observation()), 'dispatch_pending')
        ident = str(uuid.uuid4())
        with self.assertRaises(Conflict): self.core.request_manual(ident, self.manual_request(ident), self.observation())


if __name__ == '__main__':
    unittest.main()
