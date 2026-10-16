"""Fresh raw observations and bounded read-only status requests, never heater writes."""
import asyncio
import copy
import json
import types
import unittest
from unittest.mock import patch
from custom_components.quack_materials.preheat_telemetry import PreheatTelemetry

SERIAL = 'P2STEST00000001'


def full(**changes):
    data = dict(command='push_status', msg=0, gcode_state='IDLE',
                chamber_temper=44, bed_temper=90, bed_target_temper=100,
                stg_cur=255, stat='0', cfg='100000000',
                device={'airduct': {'modeCur': 1}})
    data.update(changes)
    return data


class TelemetryTests(unittest.TestCase):
    def setUp(self):
        self.now = 1000.0
        self.tap = PreheatTelemetry(None, SERIAL, clock=lambda: self.now)

    def test_full_is_fresh_once_and_read_does_not_advance_time(self):
        self.tap.ingest(full())
        self.now += 90
        observed = self.tap.read()
        self.assertEqual(observed['printer_id'], SERIAL)
        self.assertEqual(observed['status'], 'idle')
        self.assertEqual(observed['bed_target'], 100)
        self.assertTrue(observed['door_closed'])
        self.assertTrue(observed['idle_protection'])
        self.assertFalse(observed['maintenance_mode'])
        self.assertEqual(observed['current_stage'], 255)
        self.assertEqual(observed['status_at'], 1000)

    def test_delta_refreshes_unchanged_fields_only_after_complete_full(self):
        self.tap.ingest(dict(command='push_status', msg=1, bed_temper=91))
        self.assertNotIn('status_at', self.tap.read())
        self.tap.ingest(full())
        self.now += 5
        self.tap.ingest(dict(command='push_status', msg=1, bed_temper=92))
        observed = self.tap.read()
        self.assertEqual(observed['bed_temp'], 92)
        self.assertEqual(observed['status_at'], 1005)
        self.assertEqual(observed['idle_protection_at'], 1005)

    def test_actual_stage_fields_override_and_ignore_unrelated_mc_print_stage(self):
        self.tap.ingest(full(stg_cur='255', mc_print_stage='1'))
        self.assertEqual(self.tap.read()['current_stage'], 255)
        self.assertIsNone(self.tap.read()['telemetry_diagnostics']['decode_error'])
        self.tap.ingest(full(stage={'_id': '1'}, stg_cur='255', mc_print_stage='wrong'))
        self.assertEqual(self.tap.read()['current_stage'], 255)
        data = full(stage={'_id': '2'})
        del data['stg_cur']
        self.tap.ingest(data)
        self.assertEqual(self.tap.read()['current_stage'], 2)

    def test_idle_boot_stage_zero_maps_to_idle_only_with_actual_print_type(self):
        self.tap.ingest(full(stg_cur='0', print_type='idle'))
        self.assertEqual(self.tap.read()['current_stage'], 255)
        self.tap.ingest(full(stg_cur='0', print_type='local'))
        self.assertEqual(self.tap.read()['current_stage'], 0)
        for value in ('-2', '256', '1.0', ' 255', True):
            self.tap.ingest(full(stg_cur=value))
            self.assertNotIn('status_at', self.tap.read())

    def test_verified_p2s_signed_idle_live_packet_normalizes_to_canonical_idle(self):
        for stage in (-1, '-1'):
            self.tap.ingest(full(gcode_state='FINISH', stg_cur=stage, mc_print_stage='1', print_type='local',
                cfg='5C12057DA11', stat='40258000', chamber_temper=49.0, bed_temper=85.0, bed_target_temper=0.0,
                device={'bed': {'info': {'temp': 85}}, 'ctc': {'info': {'temp': 49}}, 'airduct': {'modeCur': 1}}))
            observed = self.tap.read()
            self.assertEqual(observed['current_stage'], 255)
            self.assertEqual(observed['status'], 'finish')
            self.assertTrue(observed['door_closed'])
            self.assertTrue(observed['idle_protection'])
            self.assertFalse(observed['maintenance_mode'])
            self.assertEqual((observed['bed_temp'], observed['bed_target'], observed['chamber_temp']), (85, 0, 49))
            self.assertTrue(observed['telemetry_diagnostics']['base_valid'])
            self.assertIsNone(observed['telemetry_diagnostics']['decode_error'])
        for stage in (-2, '-2', -1.0):
            self.tap.ingest(full(stg_cur=stage))
            self.assertNotIn('status_at', self.tap.read())

    def test_missing_real_stage_cannot_establish_delta_baseline(self):
        data = full(mc_print_stage='255')
        del data['stg_cur']
        self.tap.ingest(data)
        self.assertNotIn('current_stage', self.tap.read())
        self.assertFalse(self.tap.read()['telemetry_diagnostics']['base_valid'])
        self.now += 5
        self.tap.ingest(dict(command='push_status', msg=1, bed_temper=91))
        self.assertNotEqual(self.tap.read().get('status_at'), 1005)

    def test_delta_after_receive_gap_or_clock_rollback_requires_a_new_full_frame(self):
        for next_time in (1031, 999):
            with self.subTest(next_time=next_time):
                self.now = 1000
                self.tap.ingest(full())
                self.now = next_time
                self.tap.ingest(dict(command='push_status', msg=1, bed_temper=92))
                self.assertNotIn('status_at', self.tap.read())
                self.now += 1
                self.tap.ingest(dict(command='push_status', msg=1, bed_temper=93))
                self.assertNotIn('door_closed_at', self.tap.read())
                self.tap.ingest(full())
                self.assertEqual(self.tap.read()['status_at'], self.now)

    def test_incomplete_full_never_invents_safety_defaults(self):
        self.tap.ingest(full(cfg=None))
        self.assertNotIn('idle_protection', self.tap.read())
        self.now += 5
        self.tap.ingest(dict(command='push_status', msg=1, bed_temper=92))
        self.assertNotEqual(self.tap.read().get('status_at'), 1005)

    def test_ack_does_not_refresh_and_unknown_status_discards_base(self):
        self.tap.ingest(full())
        baseline = self.tap.read()
        self.now += 5
        self.tap.ingest(dict(command='set_bed_temper', msg=0, bed_temper=110))
        observed = self.tap.read()
        observed.pop('telemetry_diagnostics', None)
        baseline.pop('telemetry_diagnostics', None)
        self.assertEqual(observed, baseline)
        self.tap.ingest(dict(command='push_status', msg=9, bed_temper=110))
        self.assertNotIn('status_at', self.tap.read())
        self.tap.ingest(dict(command='push_status', msg=1, bed_temper=110))
        self.assertNotIn('status_at', self.tap.read())

    def test_malformed_delta_cannot_make_previous_safe_state_fresh(self):
        for changes in ({'stat': 'not-hex'}, {'cfg': 'bad data'}, {'bed_temper': True},
                        {'device': []}, {'gcode_state': 8}, {'msg': True},
                        {'device': {'airduct': {'modeCur': 3}}}):
            with self.subTest(changes=changes):
                self.tap.ingest(full())
                self.now += 5
                self.tap.ingest(dict(dict(command='push_status', msg=1), **changes))
                self.assertNotIn('status_at', self.tap.read())

    def test_modern_packed_temperatures_and_protection_modes(self):
        data = dict(command='push_status', gcode_state='FINISH', stg_cur=255, stat='0', cfg='100000000',
                    device={'bed': {'info': {'temp': 7209050}},
                            'ctc': {'info': {'temp': 655415}}, 'airduct': {'modeCur': 0}})
        self.tap.ingest(data)
        result = self.tap.read()
        self.assertEqual(result['bed_temp'], 90)
        self.assertEqual(result['bed_target'], 110)
        self.assertEqual(result['chamber_temp'], 55)
        self.assertEqual(result['air_mode'], 'external')
        for cfg, enabled, maintenance in [('0', False, False), ('200000000', False, True)]:
            self.tap.ingest(full(cfg=cfg))
            self.assertEqual(self.tap.read()['idle_protection'], enabled)
            self.assertEqual(self.tap.read()['maintenance_mode'], maintenance)
        self.tap.ingest(full(cfg='300000000'))
        self.assertNotIn('idle_protection', self.tap.read())

    def test_modern_bed_temp_fallback_and_input_is_unchanged(self):
        data = full(device={'bed_temp': 7209050, 'airduct': {'modeCur': 1}})
        del data['bed_temper']
        del data['bed_target_temper']
        before = copy.deepcopy(data)
        self.tap.ingest(data)
        self.assertEqual(self.tap.read()['bed_target'], 110)
        self.assertEqual(data, before)

    def test_diagnostics_report_decode_failure_without_granting_freshness(self):
        self.tap.ingest(full())
        self.now += 5
        self.tap.ingest(full(device={'ctc': {'info': {'temp': 65535}}, 'airduct': {'modeCur': 1}}))
        observed = self.tap.read()
        diagnostic = observed['telemetry_diagnostics']
        self.assertNotIn('status_at', observed)
        self.assertEqual(diagnostic['last_received_at'], 1005)
        self.assertEqual(diagnostic['last_command'], 'push_status')
        self.assertEqual(diagnostic['decode_error'], 'Invalid temperature')
        self.assertEqual(diagnostic['protocol_fields']['device.ctc.info.temp'], 65535)
        self.now += 60
        self.assertEqual(self.tap.read()['telemetry_diagnostics']['last_received_at'], 1005)

    def test_diagnostics_bound_protocol_values_and_exclude_credentials_and_other_payload(self):
        data = full(password='do-not-return-this-secret', access_code='another-secret',
                    gcode_file='private-file-name.3mf', blob='x' * 100000,
                    device={'bed': {'info': {'temp': 7209050, 'password': 'nested-secret'}},
                            'ctc': {'info': {'temp': 655415}}, 'airduct': {'modeCur': 1},
                            'credentials': {'token': 'nested-token'}})
        self.tap.ingest(data)
        diagnostic = self.tap.read()['telemetry_diagnostics']
        encoded = json.dumps(diagnostic)
        for secret in ('do-not-return-this-secret', 'another-secret', 'nested-secret', 'nested-token', 'private-file-name'):
            self.assertNotIn(secret, encoded)
        self.assertLess(len(encoded), 4096)
        self.assertEqual(diagnostic['protocol_fields']['device.bed.info.temp'], 7209050)
        self.assertIn('gcode_state', diagnostic['packet_keys'])
        self.assertIsNone(diagnostic['decode_error'])

    def test_door_open_edges_survive_open_close_between_reader_polls(self):
        self.tap.ingest(full())
        self.tap.ingest(dict(command='push_status', msg=1, stat='800000'))
        self.tap.ingest(dict(command='push_status', msg=1, stat='800000'))
        self.tap.ingest(dict(command='push_status', msg=1, stat='0'))
        self.assertTrue(self.tap.read()['door_closed'])
        self.assertEqual(self.tap.read()['door_open_count'], 1)
        self.tap.ingest(dict(command='push_status', msg=1, stat='800000'))
        self.assertEqual(self.tap.read()['door_open_count'], 2)


class HookTests(unittest.TestCase):
    def model(self, serial=SERIAL, device_type='P2S'):
        class Model:
            def __init__(self):
                self.info = types.SimpleNamespace(serial=serial, device_type=device_type)
                self.calls = []
            def print_update(self, data):
                self.calls.append(data)
                if data.get('raise'):
                    raise ValueError('model decode failed')
                return data.get('result', True)
        return Model()

    def tap(self, *models):
        hass = types.SimpleNamespace(data={'bambu_lab': {
            str(i): types.SimpleNamespace(get_model=lambda model=model: model)
            for i, model in enumerate(models)}})
        return PreheatTelemetry(hass, SERIAL, clock=lambda: 1000)

    def test_attach_is_scoped_reversible_and_forwards_original_once(self):
        other, model = self.model(serial='other'), self.model()
        tap = self.tap(other, model)
        original = model.print_update
        self.assertTrue(tap.attach())
        self.assertTrue(tap.attach())
        data = full(result=False)
        self.assertIs(model.print_update(data), False)
        self.assertEqual(model.calls, [data])
        self.assertEqual(tap.read()['status'], 'idle')
        self.assertEqual(other.calls, [])
        tap.close()
        self.assertEqual(model.print_update, original)
        self.assertNotIn('status_at', tap.read())
        self.assertFalse(self.tap(self.model(device_type='X1C')).attach())

    def test_diagnostics_expose_attachment_and_capture_keyword_calls_once(self):
        model = self.model()
        tap = self.tap(model)
        self.assertTrue(tap.attach())
        data = full()
        self.assertTrue(model.print_update(data=data))
        diagnostic = tap.read()['telemetry_diagnostics']
        self.assertTrue(diagnostic['attached'])
        self.assertEqual(diagnostic['attach_reason'], 'attached')
        self.assertEqual(diagnostic['last_command'], 'push_status')
        self.assertEqual(model.calls, [data])
        tap.close()
        self.assertFalse(tap.read()['telemetry_diagnostics']['attached'])
        missing = self.tap(self.model(serial='foreign'))
        self.assertFalse(missing.attach())
        self.assertEqual(missing.read()['telemetry_diagnostics']['attach_reason'], 'matching_p2s_not_found')

    def test_original_errors_propagate_and_failed_decode_invalidates_freshness(self):
        model = self.model()
        tap = self.tap(model)
        tap.attach()
        model.print_update(full())
        with self.assertRaisesRegex(ValueError, 'model decode failed'):
            model.print_update(full(**{'raise': True}))
        self.assertEqual(len(model.calls), 2)
        self.assertNotIn('status_at', tap.read())
        tap.close()

    def test_diagnostic_failure_cannot_replace_an_original_decode_error(self):
        model = self.model()
        tap = self.tap(model)
        tap.attach()
        def broken(data):
            raise RuntimeError('diagnostic failed')
        tap._record_packet = broken
        with self.assertRaisesRegex(ValueError, 'model decode failed'):
            model.print_update(full(**{'raise': True}))
        self.assertEqual(len(model.calls), 1)
        tap.close()

    def test_tap_failure_never_breaks_original_observer(self):
        model = self.model()
        tap = self.tap(model)
        tap.attach()
        def broken(data):
            raise RuntimeError('capture failed')
        tap.ingest = broken
        self.assertTrue(model.print_update(full()))
        self.assertEqual(len(model.calls), 1)
        tap.close()

    def test_attach_retries_after_integration_load_and_model_reload(self):
        first, second = self.model(), self.model()
        hass = types.SimpleNamespace(data={})
        tap = PreheatTelemetry(hass, SERIAL, clock=lambda: 1000)
        self.assertFalse(tap.attach())
        hass.data['bambu_lab'] = {'entry': types.SimpleNamespace(get_model=lambda: first)}
        original = first.print_update
        self.assertTrue(tap.attach())
        first.print_update(full())
        hass.data['bambu_lab']['entry'] = types.SimpleNamespace(get_model=lambda: second)
        self.assertTrue(tap.attach())
        self.assertEqual(first.print_update, original)
        self.assertNotIn('status_at', tap.read())
        second.print_update(full(gcode_state='RUNNING'))
        self.assertEqual(tap.read()['status'], 'running')
        tap.close()

    def test_close_preserves_a_later_wrapper(self):
        model = self.model()
        tap = self.tap(model)
        tap.attach()
        replacement = lambda data: None
        model.print_update = replacement
        tap.close()
        self.assertIs(model.print_update, replacement)


class RefreshTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.now = 1000.0
        self.calls = []
        self.model = HookTests().model()
        async def refresh():
            self.calls.append('status_refresh')
        self.coordinator = types.SimpleNamespace(get_model=lambda: self.model,
            client=types.SimpleNamespace(refresh=refresh))
        self.hass = types.SimpleNamespace(data={'bambu_lab': {'entry': self.coordinator}})
        self.tap = PreheatTelemetry(self.hass, SERIAL, clock=lambda: self.now)
        self.assertTrue(self.tap.attach())
        self.addCleanup(self.tap.close)

    async def test_cold_delta_baseline_requests_full_status_once_per_minute_and_never_grants(self):
        self.model.print_update(dict(command='push_status', msg=1, bed_temper=91))
        self.assertNotIn('status_at', self.tap.read())
        self.assertTrue(await self.tap.refresh_if_needed())
        self.assertFalse(await self.tap.refresh_if_needed())
        self.now = 1059
        self.assertFalse(await self.tap.refresh_if_needed())
        self.now = 1060
        self.assertTrue(await self.tap.refresh_if_needed())
        self.assertEqual(self.calls, ['status_refresh', 'status_refresh'])
        self.assertNotIn('status_at', self.tap.read())
        diagnostic = self.tap.read()['telemetry_diagnostics']
        self.assertEqual(diagnostic['refresh_requested_at'], 1060)
        self.assertEqual(diagnostic['refresh_succeeded_at'], 1060)
        self.assertIsNone(diagnostic['refresh_error'])

    async def test_complete_full_baseline_prevents_refresh_even_after_throttle_window(self):
        self.model.print_update(full())
        for _ in range(8):
            self.now += 15
            self.model.print_update(dict(command='push_status', msg=1, bed_temper=91))
            self.assertFalse(await self.tap.refresh_if_needed())
        self.assertEqual(self.calls, [])
        self.now += 31
        self.model.print_update(dict(command='push_status', msg=1, bed_temper=91))
        self.assertNotIn('status_at', self.tap.read())
        self.assertTrue(await self.tap.refresh_if_needed())
        self.assertNotIn('status_at', self.tap.read())

    async def test_refresh_rechecks_exact_coordinator_and_closed_hook(self):
        self.coordinator.get_model = lambda: HookTests().model(serial='foreign')
        self.assertFalse(await self.tap.refresh_if_needed())
        self.assertEqual(self.calls, [])
        self.tap.close()
        self.assertFalse(await self.tap.refresh_if_needed())

    async def test_refresh_timeout_cancels_wait_and_keeps_throttle_without_freshness(self):
        cancelled = []
        async def never_finishes():
            self.calls.append('status_refresh')
            try:
                await asyncio.Event().wait()
            finally:
                cancelled.append(True)
        self.coordinator.client.refresh = never_finishes
        wait_for = asyncio.wait_for
        async def short_deadline(awaitable, timeout):
            self.assertEqual(timeout, 5)
            return await wait_for(awaitable, timeout=0.01)
        with patch('asyncio.wait_for', short_deadline):
            self.assertFalse(await wait_for(self.tap.refresh_if_needed(), timeout=0.2))
        self.assertEqual(cancelled, [True])
        self.assertFalse(await self.tap.refresh_if_needed())
        self.assertEqual(self.calls, ['status_refresh'])
        diagnostic = self.tap.read()['telemetry_diagnostics']
        self.assertEqual(diagnostic['refresh_error'], 'TimeoutError')
        self.assertFalse(diagnostic['refresh_pending'])
        self.assertIsNone(diagnostic['refresh_succeeded_at'])
        self.assertNotIn('status_at', self.tap.read())

    async def test_refresh_failure_is_throttled_and_exposes_only_error_type(self):
        async def failed():
            self.calls.append('status_refresh')
            raise RuntimeError('sensitive transport details must stay private')
        self.coordinator.client.refresh = failed
        self.assertFalse(await self.tap.refresh_if_needed())
        self.assertFalse(await self.tap.refresh_if_needed())
        self.assertEqual(self.calls, ['status_refresh'])
        self.assertEqual(self.tap.read()['telemetry_diagnostics']['refresh_error'], 'RuntimeError')
        self.assertNotIn('sensitive transport', json.dumps(self.tap.read()))
        self.assertNotIn('status_at', self.tap.read())


if __name__ == '__main__':
    unittest.main()
