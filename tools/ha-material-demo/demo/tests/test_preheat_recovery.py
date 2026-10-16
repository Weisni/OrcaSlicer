"""Regressions for observed P2S idle shutdown and heating with an open door."""
import unittest
from tests import test_job_preheat as fixture
from tests import test_preheat_runtime as runtime_fixture
from custom_components.quack_materials.store import Conflict


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.f = fixture.PreheatTests()
        self.f.setUp()
        self.addCleanup(self.f.doCleanups)

    def establish(self):
        f = self.f
        response = f.core.request(f.job, f.request(), f.observation())
        intent = f.core.tick(f.observation())[0]
        f.device_bed_target = intent['value']
        f.core.acknowledge(intent, True, f.observation())
        return response

    def test_zero_target_reheats_after_five_minutes_without_claim_or_deadline_extension(self):
        f = self.f
        response = self.establish()
        f.warm(285)
        f.device_bed_target = 0
        f.now += 15
        state = f.core.status(f.job, f.observation())
        self.assertEqual(state['phase'], 'heating')
        self.assertEqual(state['reason'], 'bed_target_reset')
        self.assertFalse(state['ready'])
        self.assertEqual(state['expires_at'], response['expires_at'])
        with self.assertRaises(Conflict): f.claim(response)
        intents = f.core.tick(f.observation())
        self.assertEqual([(i['kind'], i['value']) for i in intents], [('bed_target', 110)])
        self.assertTrue(f.core.authorize(intents[0], f.observation()))
        f.core.acknowledge(intents[0], True, f.observation())
        f.device_bed_target = 110
        self.assertFalse(f.core.status(f.job, f.observation())['ready'])
        self.assertFalse(f.warm(285)['ready'])
        self.assertTrue(f.warm(15)['ready'])

    def test_repeating_five_minute_resets_do_not_prevent_a_real_thermal_plateau(self):
        f = self.f
        self.establish()
        for step in range(1, 41):
            f.now += 15
            if step % 20 == 0:
                f.device_bed_target = 0
            observation = f.observation()
            for intent in f.core.tick(observation):
                self.assertTrue(f.core.authorize(intent, observation))
                f.device_bed_target = intent['value']
                f.device_bed_temp = intent['value']
                f.core.acknowledge(intent, True, f.observation())
        self.assertTrue(f.core.status(f.job, f.observation())['ready'])

    def test_unconfirmed_reheat_stops_after_thirty_seconds_and_reads_cannot_restart_it(self):
        f = self.f
        self.establish()
        f.now += 300
        f.device_bed_target = 0
        intent = f.core.tick(f.observation())[0]
        f.core.acknowledge(intent, True, f.observation())
        f.now += 15
        self.assertEqual(f.core.tick(f.observation()), [], 'Wait for printer acknowledgement instead of spamming commands')
        f.now += 16
        self.assertEqual(f.core.tick(f.observation()), [])
        self.assertEqual(f.core.status(f.job, f.observation())['reason'], 'heater_command_failed')
        f.device_bed_target = 110
        self.assertEqual(f.core.tick(f.observation()), [])
        f.device_bed_target = 0
        state = f.core.status(f.job, f.observation())
        f.core.retry(f.job, f.command(state, 'explicit-retry'), f.observation())
        self.assertEqual(len(f.core.tick(f.observation())), 1)

    def test_manual_attempt_reheats_but_does_not_extend_deadline(self):
        import uuid
        f = self.f
        f.job = str(uuid.uuid4())
        response = f.core.request_manual(f.job, f.manual_request(f.job), f.observation())
        intent = f.core.tick(f.observation())[0]
        f.device_bed_target = 110
        f.core.acknowledge(intent, True, f.observation())
        f.now += 300
        f.device_bed_target = 0
        self.assertEqual(len(f.core.tick(f.observation())), 1)
        self.assertFalse(f.core.manual_status(f.observation())['ready'])
        f.now = response['expires_at']
        self.assertEqual(f.core.tick(f.observation()), [])
        self.assertEqual(f.core.manual_status(f.observation())['phase'], 'expired')

    def test_reset_after_bed_conditioning_restores_print_target_without_granting_early(self):
        f = self.f
        self.establish()
        self.assertTrue(f.warm()['ready'])
        f.device_bed_target = 0
        self.assertFalse(f.core.status(f.job, f.observation())['ready'])
        intents = f.core.tick(f.observation())
        self.assertEqual([(i['kind'], i['value']) for i in intents], [('bed_target', 100)])
        f.device_bed_target = 100
        f.core.acknowledge(intents[0], True, f.observation())
        self.assertTrue(f.core.status(f.job, f.observation())['ready'])

    def test_zero_target_with_real_cooling_does_not_reuse_earlier_plateau_samples(self):
        f = self.f
        self.establish()
        f.warm(585)
        f.device_bed_target = 0
        f.device_bed_temp = 100
        f.now += 15
        intent = f.core.tick(f.observation(chamber_temp=50))[0]
        f.device_bed_target = 110
        f.core.acknowledge(intent, True, f.observation(chamber_temp=50))
        f.device_bed_temp = 110
        self.assertFalse(f.warm(585, temp=50)['ready'])
        self.assertTrue(f.warm(15, temp=50)['ready'])

    def test_zero_target_does_not_override_cancel_busy_or_unknown_protection(self):
        for changes in ({'status': 'running'}, {'idle_protection': False},
                        {'door_closed': None}, {'bed_target_at': self.f.now - 31}):
            f = self.f
            self.establish()
            f.device_bed_target = 0
            self.assertEqual(f.core.tick(f.observation(**changes)), [])
            state = f.core.status(f.job, f.observation())
            f.core.cancel(f.job, f.command(state, key='cancel-' + f.job), f.observation())
            self.assertEqual(f.core.tick(f.observation()), [])
            f.job = f.reserve()

    def test_open_door_keeps_refreshing_but_cannot_accumulate_plateau_or_dispatch(self):
        f = self.f
        response = self.establish()
        f.warm(285)
        sent = []
        for _ in range(45):
            f.now += 15
            observation = f.observation(door_closed=False, door_open_count=1, chamber_temp=53)
            for intent in f.core.tick(observation):
                self.assertTrue(f.core.authorize(intent, observation))
                sent.append(intent)
                f.core.acknowledge(intent, True, observation)
            state = f.core.status(f.job, observation)
            self.assertEqual((state['phase'], state['reason']), ('heating', 'door_open_heating'))
            self.assertFalse(state['ready'])
            with self.assertRaises(Conflict): f.claim(response)
        self.assertGreater(len(sent), 5)
        self.assertFalse(f.warm(585, temp=53, door_count=1)['ready'])
        self.assertFalse(f.core.status(f.job, f.observation(chamber_temp=54, door_open_count=1))['ready'])
        self.assertFalse(f.warm(585, temp=55, door_count=1)['ready'])
        self.assertTrue(f.warm(15, temp=55, door_count=1)['ready'])

    def test_starting_with_open_door_and_dispatch_without_preheat_still_require_closed_door(self):
        f = self.f
        with self.assertRaises(Conflict):
            f.core.request(f.job, f.request(), f.observation(door_closed=False))
        f.core.request(f.job, f.request(False), f.observation())
        self.assertEqual(f.core.tick(f.observation(door_closed=False)), [])
        self.assertFalse(f.core.status(f.job, f.observation(door_closed=False))['ready'])


class RecoveryRuntimeTests(runtime_fixture.RuntimeTests):
    async def test_door_opening_during_authorization_does_not_fail_heater_service(self):
        await self.request()
        f = self.fixture
        f.device_bed_target = 110
        await self.runtime.status(f.job)
        original = self.hass.async_add_executor_job
        async def executor(fn, *args):
            result = await original(fn, *args)
            if fn == self.runtime.core.authorize:
                self.telemetry.read = lambda: dict(f.observation(door_closed=False, door_open_count=1),
                                                  current_stage=255, current_stage_at=f.now)
            return result
        self.hass.async_add_executor_job = executor
        f.now += 65
        await self.runtime.tick()
        self.assertEqual(len(self.sent), 2)
        self.assertEqual((await self.runtime.status(f.job))['reason'], 'door_open_heating')

    async def test_open_door_passes_final_service_guard_for_existing_owner(self):
        await self.request()
        f = self.fixture
        f.device_bed_target = 110
        await self.runtime.status(f.job)
        self.telemetry.read = lambda: dict(f.observation(door_closed=False, door_open_count=1),
                                          current_stage=255, current_stage_at=f.now)
        f.now += 65
        await self.runtime.tick()
        self.assertEqual(len(self.sent), 2)
        self.assertEqual(self.sent[-1][2]['value'], 110)


if __name__ == '__main__':
    unittest.main()
