"""Target attainment bypasses chamber waiting, never dispatch safety checks."""
import unittest
from tests import test_job_preheat as support


class TargetReadyTests(unittest.TestCase):
    setUp = support.PreheatTests.setUp
    reserve = support.PreheatTests.reserve
    observation = support.PreheatTests.observation
    request = support.PreheatTests.request
    warm = support.PreheatTests.warm
    claim = support.PreheatTests.claim
    manual_request = support.PreheatTests.manual_request

    def test_at_or_above_target_skips_chamber_and_never_requests_warmup_bed(self):
        for temperature in (55, 57):
            with self.subTest(temperature=temperature):
                self.job = self.reserve()
                request = self.request()
                request['parameters']['chamber_target'] = 55
                observation = self.observation(chamber_temp=temperature, bed_temp=100, bed_target=100)
                response = self.core.request(self.job, request, observation)
                self.assertTrue(response['ready'])
                self.assertEqual(response['reason'], 'target_reached')
                intents = self.core.tick(observation)
                self.assertTrue(all(i['value'] == 100 for i in intents if i['kind'] == 'bed_target'))
                self.claim(response)
                with self.store.connection() as db:
                    db.execute("UPDATE jobs SET state='failed',settlement='{}' WHERE uuid=?", (self.job,))

    def test_hot_chamber_still_waits_for_the_print_bed_target(self):
        request = self.request()
        request['parameters']['chamber_target'] = 55
        response = self.core.request(self.job, request, self.observation(chamber_temp=57, bed_target=110))
        self.assertEqual(response['phase'], 'conditioning_bed')
        self.assertFalse(response['ready'])
        intent = self.core.tick(self.observation(chamber_temp=57, bed_target=110))[0]
        self.assertEqual(intent['value'], 100)
        self.core.acknowledge(intent, True, self.observation(chamber_temp=57, bed_target=100, bed_temp=110))
        self.assertFalse(self.core.status(self.job, self.observation(chamber_temp=57, bed_target=100, bed_temp=110))['ready'])
        response = self.core.status(self.job, self.observation(chamber_temp=57, bed_target=100, bed_temp=100))
        self.assertTrue(response['ready'])

    def test_target_only_readiness_is_revoked_when_temperature_falls(self):
        request = self.request()
        request['parameters']['chamber_target'] = 55
        response = self.core.request(self.job, request, self.observation(chamber_temp=57, bed_target=100, bed_temp=100))
        self.assertTrue(response['ready'])
        response = self.core.status(self.job, self.observation(chamber_temp=54, bed_target=100, bed_temp=100))
        self.assertFalse(response['ready'])
        with self.assertRaises(support.Conflict):
            self.claim(response)

    def test_closed_door_above_target_does_not_require_recovery_to_a_higher_previous_temperature(self):
        request = self.request()
        request['parameters']['chamber_target'] = 55
        self.core.request(self.job, request, self.observation(chamber_temp=60, bed_target=100, bed_temp=100))
        self.assertFalse(self.core.status(self.job, self.observation(chamber_temp=57, bed_target=100, bed_temp=100,
                         door_closed=False, door_open_count=1))['ready'])
        response = self.core.status(self.job, self.observation(chamber_temp=57, bed_target=100, bed_temp=100,
                                   door_closed=True, door_open_count=1))
        self.assertTrue(response['ready'])

    def test_hot_chamber_does_not_bypass_stale_or_busy_printer_checks(self):
        request = self.request()
        request['parameters']['chamber_target'] = 55
        for changes in ({'door_closed': False}, {'status': 'running'}, {'idle_protection': False},
                        {'maintenance_mode': True}, {'chamber_temp_at': self.now - 31}):
            with self.subTest(changes=changes), self.assertRaises(support.Conflict):
                self.core.request(self.job, request, self.observation(chamber_temp=57, **changes))

    def test_manual_transfer_preserves_target_only_basis_and_revokes_on_drop(self):
        observation = self.observation(chamber_temp=57, bed_target=110)
        manual = 'manual-target-transfer'
        request = self.manual_request(manual)
        request['parameters']['chamber_target'] = 55
        self.assertTrue(self.core.request_manual(manual, request, observation)['ready'])
        request = self.request()
        request['parameters']['chamber_target'] = 55
        self.core.request(self.job, request, observation)
        response = self.core.status(self.job, self.observation(chamber_temp=54, bed_target=110))
        self.assertFalse(response['ready'])
        intents = self.core.tick(self.observation(chamber_temp=54, bed_target=110))
        self.assertTrue(all(i['value'] == 110 for i in intents if i['kind'] == 'bed_target'))

    def test_dispatch_rechecks_temperature_even_after_a_ready_reply(self):
        request = self.request()
        request['parameters']['chamber_target'] = 55
        response = self.core.request(self.job, request, self.observation(chamber_temp=57, bed_target=100, bed_temp=100))
        self.assertTrue(response['ready'])
        self.core.observation_reader = lambda: self.observation(chamber_temp=54, bed_target=100, bed_temp=100)
        with self.assertRaises(support.Conflict):
            self.claim(response)

    def test_reaching_target_during_warmup_ends_the_partial_plateau_wait(self):
        request = self.request()
        request['parameters']['chamber_target'] = 55
        self.core.request(self.job, request, self.observation(chamber_temp=54, bed_target=110))
        self.now += 5
        response = self.core.status(self.job, self.observation(chamber_temp=55, bed_target=110))
        self.assertEqual(response['phase'], 'conditioning_bed')
        intent = self.core.tick(self.observation(chamber_temp=55, bed_target=110))[0]
        self.assertEqual(intent['value'], 100)
        observation = self.observation(chamber_temp=55, bed_target=100, bed_temp=100)
        self.core.acknowledge(intent, True, observation)
        self.assertTrue(self.core.status(self.job, observation)['ready'])
