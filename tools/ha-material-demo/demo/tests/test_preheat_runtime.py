"""Exercise the HA adapter with fake services, real SQLite and a fake clock."""
import asyncio
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

from tests import test_job_preheat as core_fixture
from tests import test_provider_preheat as provider_fixture
from custom_components.quack_materials.preheat_runtime import PreheatRuntime
from custom_components.quack_materials.store import Conflict


class RuntimeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.fixture = core_fixture.PreheatTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.sent = []
        self.refresh_calls = []
        self.states = {
            'sensor.p2s_test_print_status': types.SimpleNamespace(state='idle', attributes={}),
            'sensor.stage': types.SimpleNamespace(state='idle', attributes={}),
            'number.p2s_test_bed_target_temperature': types.SimpleNamespace(state='0', attributes={'min': 0, 'max': 110}),
            'select.p2s_test_airduct_mode': types.SimpleNamespace(state='heating', attributes={'options': ['cooling', 'heating']}),
        }
        self.registry = types.SimpleNamespace(async_get=lambda _: types.SimpleNamespace(
            platform='bambu_lab', device_id='paired-device', config_entry_id='entry'))
        self.coordinator = types.SimpleNamespace(get_model=lambda: types.SimpleNamespace(
            info=types.SimpleNamespace(serial='P2S-test', device_type='P2S')),
            get_ha_printer_device=lambda: types.SimpleNamespace(id='paired-device'))
        async def executor(fn, *args): return fn(*args)
        async def send(domain, service, data, blocking): self.sent.append((domain, service, data))
        self.hass = types.SimpleNamespace(data={'bambu_lab': {'entry': self.coordinator}},
            states=types.SimpleNamespace(get=self.states.get),
            services=types.SimpleNamespace(has_service=lambda *a: True, async_call=send),
            async_add_executor_job=executor)
        self.settings = {'provider_printer_id': 'P2S-test', 'printer_state_entity': 'sensor.p2s_test_print_status',
                         'printer_assignment': {'stage_entity': 'sensor.stage'}}
        self.runtime = PreheatRuntime(self.hass, self.fixture.store, self.settings)
        self.runtime.core = self.fixture.core
        self.fixture.store.preheat = self.fixture.core
        async def refresh_if_needed():
            self.refresh_calls.append('status_refresh')
            return False
        self.telemetry = types.SimpleNamespace(attach=lambda: True, close=lambda: None,
                                              read=lambda: dict(self.fixture.observation(), current_stage=255, current_stage_at=self.fixture.now),
                                              refresh_if_needed=refresh_if_needed)
        self.runtime.telemetry = self.telemetry
        self.runtime.enabled = True
        self.modules = patch.dict('sys.modules', {
            'homeassistant': types.ModuleType('homeassistant'),
            'homeassistant.helpers': types.ModuleType('homeassistant.helpers'),
            'homeassistant.helpers.entity_registry': types.SimpleNamespace(async_get=lambda _: self.registry)})
        self.modules.start()
        self.addCleanup(self.modules.stop)

    async def request(self):
        return await self.runtime.command(self.fixture.job, 'request', self.fixture.request())

    async def test_tick_requests_missing_baseline_without_any_heater_attempt(self):
        await self.runtime.tick()
        self.assertEqual(self.refresh_calls, ['status_refresh'])
        self.assertEqual(self.sent, [])

    async def test_reads_and_no_preheat_never_send_commands(self):
        await self.runtime.command(self.fixture.job, 'request', self.fixture.request(enabled=False))
        for _ in range(3):
            await self.runtime.status(self.fixture.job)
            await self.runtime.tick()
        self.assertEqual(self.sent, [])

    async def test_request_sends_only_configured_paired_bed_target(self):
        await self.request()
        self.assertEqual(self.sent, [('number', 'set_value',
            {'entity_id': 'number.p2s_test_bed_target_temperature', 'value': 110})])

    async def test_wrong_device_entity_rejects_command_and_requires_retry(self):
        self.registry.async_get = lambda _: types.SimpleNamespace(platform='bambu_lab', device_id='other', config_entry_id='entry')
        result = await self.request()
        self.assertEqual(self.sent, [])
        self.assertEqual(result['reason'], 'heater_command_failed')

    async def test_busy_at_last_authorization_never_sends(self):
        self.registry.async_get = lambda _: (setattr(self.states['sensor.stage'], 'state', 'printing') or
            types.SimpleNamespace(platform='bambu_lab', device_id='paired-device', config_entry_id='entry'))
        await self.request()
        self.assertEqual(self.sent, [])

    async def test_service_failure_stops_refresh_until_explicit_retry(self):
        async def fail(*args, **kwargs): raise TimeoutError()
        self.hass.services.async_call = fail
        result = await self.request()
        self.assertEqual(result['phase'], 'interrupted')
        self.fixture.now += 120
        await self.runtime.tick()
        self.assertEqual(self.sent, [])

    async def test_cancel_waits_for_inflight_service_and_cannot_resurrect(self):
        entered, release = asyncio.Event(), asyncio.Event()
        async def held(domain, service, data, blocking):
            entered.set()
            await release.wait()
            self.sent.append((domain, service, data))
        self.hass.services.async_call = held
        request = asyncio.create_task(self.request())
        await entered.wait()
        cancel = asyncio.create_task(self.runtime.command(self.fixture.job, 'cancel',
            {'request_key': 'cancel', 'instance_id': 'quack-1', 'version': 1}))
        await asyncio.sleep(0)
        self.assertFalse(cancel.done())
        release.set()
        await request
        result = await cancel
        self.assertEqual(result['phase'], 'cancelled')
        self.fixture.now += 120
        await self.runtime.tick()
        self.assertEqual(len(self.sent), 1)

    async def test_stop_is_passive_and_prevents_future_heating(self):
        await self.request()
        await self.runtime.stop()
        self.fixture.now += 120
        await self.runtime.tick()
        self.assertEqual(len(self.sent), 1)
        self.assertIsNone(self.fixture.store.preheat)

    async def test_missing_raw_stage_cannot_be_granted_by_idle_ha_stage(self):
        self.telemetry.read = self.fixture.observation
        with self.assertRaises(Conflict): await self.request()
        self.assertEqual(self.sent, [])

    async def test_raw_busy_cannot_be_overridden_by_idle_ha_entities(self):
        self.telemetry.read = lambda: self.fixture.observation(status='running')
        with self.assertRaises(Exception): await self.request()
        self.assertEqual(self.sent, [])

    async def test_air_mode_is_selected_before_bed_heating(self):
        self.telemetry.read = lambda: self.fixture.observation(air_mode='external', current_stage=255, current_stage_at=self.fixture.now)
        await self.request()
        self.assertEqual(self.sent, [('select', 'select_option',
            {'entity_id': 'select.p2s_test_airduct_mode', 'option': 'heating'})])

    async def test_advertised_printer_limit_cannot_be_exceeded(self):
        self.states['number.p2s_test_bed_target_temperature'].attributes['max'] = 100
        result = await self.request()
        self.assertEqual(result['reason'], 'heater_command_failed')
        self.assertEqual(self.sent, [])

    async def test_prepare_rechecks_live_packet_and_handoff_never_heats(self):
        fixture = provider_fixture.ProviderPreheatTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.runtime.store, self.runtime.core = fixture.store, fixture.core
        self.telemetry.read = lambda: dict(fixture.observation(), current_stage=255, current_stage_at=fixture.now)
        request = fixture.request()
        self.telemetry.read = lambda: dict(fixture.observation(), status='running')
        with self.assertRaises(Exception): await self.runtime.dispatch('provider_job', request)
        self.assertNotEqual(fixture.preheat_phase(), 'handoff')
        self.telemetry.read = lambda: dict(fixture.observation(), current_stage=255, current_stage_at=fixture.now)
        result = await self.runtime.dispatch('provider_job', request)
        self.assertEqual(result['provider_job']['status'], 'prepared')
        await self.runtime.tick()
        self.assertEqual(self.sent, [])

    async def test_legacy_prepare_receipt_can_be_reconciled_after_upgrade(self):
        fixture = provider_fixture.ProviderPreheatTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        del fixture.store.preheat
        request = fixture.request(metadata=False)
        expected = fixture.store.provider_job(request)['provider_job']
        fixture.store.preheat = fixture.core
        self.runtime.store, self.runtime.core = fixture.store, fixture.core
        self.telemetry.read = lambda: dict(fixture.observation(), current_stage=255, current_stage_at=fixture.now)
        actual = await self.runtime.dispatch('provider_job', request)
        self.assertEqual(actual['provider_job'], expected)
        self.assertEqual(self.sent, [])

    async def test_packet_change_at_provider_entry_revokes_cached_ready_claim(self):
        fixture = provider_fixture.ProviderPreheatTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        request = fixture.request()
        self.runtime.store, self.runtime.core = fixture.store, fixture.core
        self.telemetry.read = lambda: dict(fixture.observation(), current_stage=255, current_stage_at=fixture.now)
        original = fixture.store.dispatch
        def race(action, data):
            self.telemetry.read = lambda: dict(fixture.observation(), status='running')
            return original(action, data)
        fixture.store.dispatch = race
        with self.assertRaises(Exception): await self.runtime.dispatch('provider_job', request)
        self.assertNotEqual(fixture.preheat_phase(), 'handoff')
        with fixture.store.connection() as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM provider_dispatch').fetchone()[0], 0)
        self.assertEqual(self.sent, [])


if __name__ == '__main__': unittest.main()
