"""Bounded automatic stabilization grace uses real SQLite and fake telemetry."""
import unittest
from tests import test_job_preheat as support


class DeadlineGraceTests(unittest.TestCase):
    setUp = support.PreheatTests.setUp
    reserve = support.PreheatTests.reserve
    observation = support.PreheatTests.observation
    request = support.PreheatTests.request
    command = support.PreheatTests.command
    warm = support.PreheatTests.warm
    claim = support.PreheatTests.claim

    def late_door(self):
        request = self.request()
        request['parameters']['print_bed_target'] = 110
        started = self.core.request(self.job, request, self.observation())
        self.assertTrue(self.warm(temp=54)['ready'])
        self.now += 1500  # A later door edge invalidates the old stable plateau.
        self.assertFalse(self.core.status(self.job, self.observation(chamber_temp=54, door_closed=False, door_open_count=1))['ready'])
        self.warm(525, temp=54, door_count=1)
        return started

    def test_late_door_gets_one_bounded_window_then_ready_instead_of_expiry(self):
        start = self.late_door()
        self.assertEqual(self.core.status(self.job, self.observation(door_open_count=1))['expires_at'], start['expires_at'])
        response = self.warm(15, temp=54, door_count=1)
        self.assertEqual(response['expires_at'], start['expires_at'] + 900)
        self.assertFalse(response['ready'], 'A time allowance itself never grants readiness')
        response = self.warm(90, temp=54, door_count=1)
        self.assertGreater(self.now, start['expires_at'])
        self.assertTrue(response['ready'])
        self.assertEqual(self.claim(response)['phase'], 'handoff')
        with self.store.connection() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM events WHERE kind='job_preheat_auto_extension'").fetchone()[0], 1)

    def test_grace_never_renews_itself_or_revives_an_expired_attempt(self):
        start = self.late_door()
        response = self.warm(15, temp=54, door_count=1)
        self.assertEqual(response['expires_at'], start['expires_at'] + 900)
        for _ in range(64):
            self.now += 15
            response = self.core.status(self.job, self.observation(door_open_count=1, door_closed=False))
        self.assertEqual(response['phase'], 'expired')
        self.assertEqual(self.core.tick(self.observation(door_open_count=1)), [])
        self.assertEqual(self.core.status(self.job, self.observation(door_open_count=1))['phase'], 'expired')

    def test_unsafe_off_stale_or_unconfirmed_heating_cannot_extend(self):
        start = self.late_door()
        for changes in ({'bed_target': 0}, {'door_closed': False}, {'status': 'running'},
                        {'bed_temp': 90}, {'idle_protection': False}, {'maintenance_mode': True},
                        {'air_mode': 'external'}, {'chamber_temp_at': self.now - 31}):
            with self.subTest(changes=changes):
                with self.store.connection() as db:
                    saved = self.core._load(db, self.job)
                self.now = start['expires_at'] - 60
                response = self.core.status(self.job, self.observation(door_open_count=1, **changes))
                self.assertEqual(response['expires_at'], start['expires_at'])
                with self.store.connection() as db:
                    self.core._save(db, saved)
        self.now = start['expires_at']
        self.assertEqual(self.core.status(self.job, self.observation(door_open_count=1))['phase'], 'expired')

    def test_consumed_extension_cannot_grant_another_grace(self):
        start = self.late_door()
        with self.store.connection() as db:
            job = self.core._load(db, self.job)
            job['extended'] = True
            self.core._save(db, job)
        self.now = start['expires_at'] - 30
        self.assertEqual(self.core.status(self.job, self.observation(door_open_count=1))['expires_at'], start['expires_at'])
        self.now = start['expires_at'] + 1
        self.assertEqual(self.core.status(self.job, self.observation(door_open_count=1))['phase'], 'expired')

    def test_manual_hold_does_not_automatically_extend(self):
        # Manual IDs have no inventory origin; admission creates their ledger.
        request = self.request(request_key='manual-grace-test')
        request['parameters']['print_bed_target'] = 110
        self.job = 'manual-only-grace-test'
        start = self.core.request_manual(self.job, request, self.observation())
        self.warm(600, temp=54)
        self.now = start['expires_at'] - 75
        self.core.status(self.job, self.observation(chamber_temp=54, door_open_count=1))
        response = self.warm(30, temp=54, door_count=1)
        self.assertEqual(response['expires_at'], start['expires_at'])

    def test_restart_or_late_first_contact_cannot_extend_an_expired_owner(self):
        start = self.late_door()
        self.now = start['expires_at'] + 1
        self.core = support.Preheat(self.store, clock=lambda: self.now)
        self.assertEqual(self.core.status(self.job, self.observation(door_open_count=1))['phase'], 'expired')
        self.assertEqual(self.core.tick(self.observation(door_open_count=1)), [])
