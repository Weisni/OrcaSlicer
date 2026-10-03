"""Completed HA-observed dispatches must remain complete in the native cache."""
import unittest
from unittest.mock import patch

import test_provider as fixtures


class ProviderLifecycleProjectionTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.ProviderTests()
        self.fixture.setUp()
        self.store = self.fixture.store

    def tearDown(self):
        self.fixture.tearDown()

    def completed(self):
        self.fixture.reserve()
        self.fixture.prepare()
        with patch('custom_components.quack_material_demo.store.now', return_value='2026-10-03T10:00:00+00:00'):
            self.store.observe_printer('running', 'Test plate')
        with patch('custom_components.quack_material_demo.store.now', return_value='2026-10-03T10:20:00+00:00'), \
             patch('custom_components.quack_material_demo.provider.stamp', return_value='2026-10-03T10:20:00+00:00'):
            self.store.observe_printer('finish', 'Test plate')
        return self.store.snapshot()

    def test_observed_completion_exports_terminal_time_and_elapsed_runtime(self):
        snapshot = self.completed()
        job = snapshot['native_bundle']['tables']['print_jobs'][0]
        self.assertEqual(job['state'], 'completed')
        self.assertEqual(job.get('completed_at'), '2026-10-03T10:20:00+00:00')
        self.assertEqual(job.get('started_at'), '2026-10-03T10:00:00+00:00')
        self.assertEqual(job.get('actual_runtime_seconds'), 1200)
        self.assertEqual(snapshot['jobs'][0]['settlement']['quality'], 'estimated')

    def test_booked_material_cost_uses_historical_price_and_native_cent_rounding(self):
        request = self.fixture.request('price')
        request['bundle']['tables']['spools'][0]['material_price_per_kg_micros'] = 19990000
        self.store.provider_apply(request)
        self.fixture.reserve()
        # This allocation has its own historical price, independent of later roll edits.
        request = self.fixture.request('historical-price')
        request['bundle']['tables']['allocations'][0].update(material_price_per_kg_micros=19990000,
            estimated_material_cost_micros=200000)
        self.store.provider_apply(request)
        self.fixture.prepare()
        self.store.observe_printer('running', 'Test plate')
        self.store.observe_printer('finish', 'Test plate')
        allocation = self.store.snapshot()['native_bundle']['tables']['allocations'][0]
        self.assertEqual(allocation['actual_weight_mg'], 10000)
        self.assertEqual(allocation.get('actual_material_cost_micros'), 200000)

    def test_terminal_refresh_does_not_duplicate_settlement_or_change_completion(self):
        initial = self.completed()
        self.store.observe_printer('finish', 'Test plate')
        self.store.observe_printer('idle', 'Test plate')
        refreshed = self.store.snapshot()
        self.assertEqual(refreshed['spools'][0]['remaining_mg'], 490000)
        self.assertEqual(refreshed['spools'][0]['reserved_mg'], 0)
        self.assertEqual(refreshed['native_bundle'], initial['native_bundle'])
        bookings = [e for e in refreshed['native_bundle']['tables']['stock_events'] if e['event_type'] == 'consumption']
        self.assertEqual(len(bookings), 1)

    def test_active_observation_repeats_keep_revision_and_graph_stable(self):
        self.fixture.reserve()
        self.fixture.prepare()
        self.store.observe_printer('running', 'Test plate')
        before = self.store.snapshot()
        self.store.observe_printer('running', 'Test plate')
        after = self.store.snapshot()
        self.assertEqual(after['revision'], before['revision'])
        self.assertEqual(after['jobs'], before['jobs'])
        self.assertEqual(after['native_bundle'], before['native_bundle'])

    def test_pause_and_resume_advance_revision_before_next_page(self):
        from custom_components.quack_material_demo import read_api
        from custom_components.quack_material_demo.store import Conflict
        self.fixture.reserve()
        self.fixture.prepare()
        self.store.observe_printer('running', 'Test plate')
        before = self.store.snapshot()
        for state, native_state in (('pause', 'paused'), ('running', 'printing')):
            self.store.observe_printer(state, 'Test plate')
            after = self.store.snapshot()
            self.assertGreater(after['revision'], before['revision'])
            self.assertEqual(after['jobs'][0]['state'], native_state)
            with self.assertRaises(Conflict):
                read_api.native_page(self.store, {'table':'print_jobs','revision':before['revision']})
            before = after

    def test_delayed_failed_reconciliation_uses_printer_terminal_time(self):
        self.fixture.reserve()
        self.fixture.prepare()
        with patch('custom_components.quack_material_demo.store.now', return_value='2026-10-03T10:00:00+00:00'):
            self.store.observe_printer('running', 'Test plate')
        with patch('custom_components.quack_material_demo.store.now', return_value='2026-10-03T10:20:00+00:00'):
            self.store.observe_printer('failed', 'Test plate')
        with patch('custom_components.quack_material_demo.store.now', return_value='2026-10-04T12:00:00+00:00'), \
             patch('custom_components.quack_material_demo.provider.stamp', return_value='2026-10-04T12:00:00+00:00'):
            self.store.reconcile_provider(self.fixture.review_request(3000))
        job = self.store.snapshot()['native_bundle']['tables']['print_jobs'][0]
        self.assertEqual(job['completed_at'], '2026-10-03T10:20:00+00:00')
        self.assertEqual(job['actual_runtime_seconds'], 1200)


if __name__ == '__main__':
    unittest.main()
