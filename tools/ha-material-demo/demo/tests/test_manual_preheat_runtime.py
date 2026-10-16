"""Separate manual/job feature modes exercise real runtime startup and SQLite."""
import types
import unittest
import uuid
from unittest.mock import patch
from tests import test_preheat_runtime as runtime_fixture
from custom_components.quack_materials.preheat_runtime import PreheatRuntime
from custom_components.quack_materials.store import Conflict


class ManualRuntimeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.fixture = runtime_fixture.RuntimeTests()
        await self.fixture.asyncSetUp()
        self.addCleanup(self.fixture.doCleanups)
        self.timers = []
        modules = {'homeassistant.helpers.event': types.SimpleNamespace(
            async_track_time_interval=lambda hass, callback, interval: self.timers.append((callback, interval)) or (lambda: None))}
        patcher = patch.dict('sys.modules', modules)
        patcher.start()
        self.addCleanup(patcher.stop)

    async def runtime(self, *, manual=True, job=False):
        settings = dict(self.fixture.settings, manual_preheat_enabled=manual, job_preheat_enabled=job)
        runtime = PreheatRuntime(self.fixture.hass, self.fixture.fixture.store, settings)
        runtime.telemetry = self.fixture.telemetry
        await runtime.start()
        runtime.core.clock = lambda: self.fixture.fixture.now
        self.addAsyncCleanup(runtime.stop)
        return runtime

    def start_body(self, job=None):
        return dict(job_uuid=job or str(uuid.uuid4()), request_key='manual-start', instance_id='ha-admin',
                    printer_id='P2S-test', parameters=dict(enabled=True, chamber_target=55,
                        print_bed_target=110, preheat_bed_target=110))

    async def test_manual_only_start_installs_shared_guard_without_job_capability_or_heat(self):
        runtime = await self.runtime()
        self.assertTrue(runtime.enabled)
        self.assertIs(runtime.store.preheat_guard, runtime.core)
        self.assertIsNone(getattr(runtime.store, 'preheat', None))
        from custom_components.quack_materials.read_api import materials
        self.assertNotIn('job_preheat_v1', materials(runtime.store, {})['capabilities'])
        self.assertEqual(self.fixture.sent, [])
        self.assertEqual(self.fixture.refresh_calls, ['status_refresh'])
        self.assertEqual(len(self.timers), 1)
        self.assertEqual(self.timers[0][1].total_seconds(), 5)
        status = await runtime.manual_status()
        self.assertEqual((status['schema_version'], status['enabled'], status['preheat']), (1, True, None))
        self.assertTrue(status['ready_to_start'])
        self.assertIsNone(status['blocked_reason'])
        self.assertEqual(status['telemetry']['printer_id'], 'P2S-test')
        await runtime.stop()
        self.assertIsNone(runtime.store.preheat_guard)
        self.assertIsNone(runtime.store.preheat)

    async def test_job_start_maintains_job_capability_and_disabled_manual_cannot_run(self):
        runtime = await self.runtime(manual=False, job=True)
        self.assertIs(runtime.store.preheat, runtime.core)
        self.assertIs(runtime.store.preheat_guard, runtime.core)
        with self.assertRaises(Conflict): await runtime.manual_status()
        with self.assertRaises(Conflict): await runtime.manual_command('start', self.start_body())
        self.assertEqual(self.fixture.sent, [])
        await runtime.stop()
        self.assertIsNone(runtime.store.preheat)
        self.assertIsNone(runtime.store.preheat_guard)

    async def test_manual_start_holds_target_and_cancel_uses_same_mutex_and_envelope(self):
        runtime = await self.runtime()
        body = self.start_body()
        before = runtime.store.snapshot()
        result = await runtime.manual_command('start', body)
        self.assertEqual(result['schema_version'], 1)
        self.assertTrue(result['enabled'])
        self.assertEqual(result['preheat']['job_uuid'], body['job_uuid'])
        self.assertEqual(self.fixture.sent, [('number', 'set_value',
            {'entity_id': 'number.p2s_test_bed_target_temperature', 'value': 110})])
        await runtime.manual_status()
        self.assertEqual(len(self.fixture.sent), 1, 'A status read cannot heat')
        cancelled = await runtime.manual_command('cancel', dict(job_uuid=body['job_uuid'], request_key='manual-cancel',
            instance_id=body['instance_id'], version=result['preheat']['version']))
        self.assertEqual(cancelled['preheat']['phase'], 'cancelled')
        self.fixture.fixture.now += 120
        await runtime.tick()
        self.assertEqual(len(self.fixture.sent), 1)
        after = runtime.store.snapshot()
        for field in ('jobs', 'spools', 'slots'):
            self.assertEqual(after[field], before[field], 'Manual preheat must not invent inventory attempts')

    async def test_manual_only_start_revokes_persisted_job_heating_without_inventory_changes(self):
        fixture = self.fixture.fixture
        fixture.core.request(fixture.job, fixture.request(), fixture.observation())
        before = fixture.store.snapshot()
        runtime = await self.runtime(manual=True, job=False)
        state = runtime.core.status(fixture.job, fixture.observation())
        self.assertEqual((state['phase'], state['reason']), ('cancelled', 'mode_disabled'))
        self.assertEqual(self.fixture.sent, [])
        after = fixture.store.snapshot()
        for field in ('jobs', 'spools', 'slots'): self.assertEqual(before[field], after[field])

    async def test_job_only_start_revokes_persisted_manual_heating_without_commands(self):
        fixture = self.fixture.fixture
        body = self.start_body()
        fixture.core.request_manual(body['job_uuid'], {key: value for key, value in body.items() if key != 'job_uuid'}, fixture.observation())
        runtime = await self.runtime(manual=False, job=True)
        state = runtime.core.manual_status(fixture.observation())
        self.assertEqual((state['phase'], state['reason']), ('cancelled', 'mode_disabled'))
        self.assertEqual(self.fixture.sent, [])

    async def test_manual_status_explains_busy_printer_without_issuing_commands(self):
        runtime = await self.runtime()
        self.fixture.telemetry.read = lambda: self.fixture.fixture.observation(status='running')
        result = await runtime.manual_status()
        self.assertFalse(result['ready_to_start'])
        self.assertIsNotNone(result['blocked_reason'])
        self.assertEqual(self.fixture.sent, [])

    async def test_manual_endpoint_cannot_cancel_an_existing_real_job(self):
        runtime = await self.runtime()
        body = dict(job_uuid=self.fixture.fixture.job, request_key='cancel-real', instance_id='quack-1', version=1)
        with self.assertRaises(Conflict): await runtime.manual_command('cancel', body)
        self.assertEqual(self.fixture.sent, [])

    async def test_manual_start_rejects_extra_fields_or_non_hold_parameters(self):
        runtime = await self.runtime()
        for changes in ({'entity_id': 'number.foreign'}, {'parameters': dict(enabled=False, chamber_target=0,
                print_bed_target=0, preheat_bed_target=0)}, {'parameters': dict(enabled=True, chamber_target=55,
                print_bed_target=90, preheat_bed_target=110)}):
            body = self.start_body()
            body.update(changes)
            with self.assertRaises(ValueError): await runtime.manual_command('start', body)
        with self.assertRaises(ValueError): await runtime.manual_command('extend', self.start_body())
        self.assertEqual(self.fixture.sent, [])


if __name__ == '__main__': unittest.main()
