"""Management reports committed synchronization and safe review eligibility."""
import unittest
import test_provider as fixtures


class ManagementDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.ProviderTests()
        self.fixture.setUp()
        self.store = self.fixture.store

    def tearDown(self):
        self.fixture.tearDown()

    def test_delta_upload_is_reported_as_latest_quack_sync(self):
        with self.store.connection() as db:
            self.store.event(db, 'provider_delta', {'request_key':'diagnostics-test'})
            db.execute("UPDATE events SET created_at='2026-10-03T12:00:00+00:00' WHERE kind='provider_delta'")
        self.assertEqual('2026-10-03T12:00:00+00:00', self.store.management_snapshot()['last_quack_sync'])

    def test_only_known_failed_provider_attempt_offers_reconciliation(self):
        self.fixture.reserve()
        self.fixture.prepare()
        self.store.observe_printer('running', 'Test plate')
        active = next(j for j in self.store.management_snapshot()['jobs'] if j['uuid']==self.fixture.job)
        self.assertFalse(active.get('can_reconcile_provider', False))
        self.store.observe_printer('failed', 'Test plate')
        failed = next(j for j in self.store.management_snapshot()['jobs'] if j['uuid']==self.fixture.job)
        self.assertTrue(failed.get('can_reconcile_provider', False))
        self.assertEqual('failed', failed.get('observed_outcome'))

    def test_idle_without_confirmed_failure_stays_pending(self):
        self.fixture.reserve()
        self.fixture.prepare()
        self.store.observe_printer('running', 'Test plate')
        self.store.observe_printer('idle', 'Test plate')
        job = next(j for j in self.store.management_snapshot()['jobs'] if j['uuid']==self.fixture.job)
        self.assertFalse(job.get('can_reconcile_provider', False))


if __name__ == '__main__': unittest.main()
