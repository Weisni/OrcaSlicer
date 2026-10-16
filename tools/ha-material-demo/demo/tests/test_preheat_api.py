"""HA HTTP preheat boundaries with a recording runtime, no device services."""
import asyncio
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch
from custom_components.quack_materials import async_setup
from custom_components.quack_materials.store import Conflict

JOB = '11111111-1111-4111-8111-111111111111'


class PreheatHttpTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.views, self.runtimes, self.listeners = [], [], []
        self.frontend_urls, self.static_paths = [], []
        parent = self
        class Runtime:
            def __init__(self, hass, store, settings, publish):
                self.enabled, self.started, self.stopped = True, False, False
                self.status_calls, self.commands, self.dispatches = [], [], []
                self.manual_reads, self.manual_commands = 0, []
                self.core = types.SimpleNamespace(summaries=lambda: [{'job_uuid': JOB, 'phase': 'heating', 'ready': False}])
                parent.runtimes.append(self)
            async def start(self): self.started = True
            async def stop(self, *_): self.stopped = True
            async def status(self, job):
                self.status_calls.append(job)
                return {'job_uuid': job, 'phase': 'heating', 'ready': False}
            async def command(self, job, command, data):
                if data['request_key'] == 'conflict': raise Conflict('Refresh first')
                self.commands.append((job, command, data))
                return {'job_uuid': job, 'phase': 'cancelled' if command == 'cancel' else 'heating'}
            async def manual_status(self):
                self.manual_reads += 1
                return {'schema_version': 1, 'enabled': True, 'preheat': None}
            async def manual_command(self, command, data):
                self.manual_commands.append((command, data))
                return {'schema_version': 1, 'enabled': True, 'preheat': {'job_uuid': data['job_uuid']}}
            async def dispatch(self, action, data):
                self.dispatches.append((action, data))
                return {'accepted': True}
        class View:
            def json(self, data, status_code=200): return status_code, data
        async def executor(function, *args): return function(*args)
        async def register_paths(paths): self.static_paths.extend(paths)
        self.hass = types.SimpleNamespace(data={},
            config=types.SimpleNamespace(path=lambda *parts: str(Path(self.temp.name).joinpath(*parts))),
            states=types.SimpleNamespace(async_set=lambda *args: None),
            http=types.SimpleNamespace(register_view=self.views.append, async_register_static_paths=register_paths),
            async_add_executor_job=executor,
            bus=types.SimpleNamespace(async_listen_once=lambda name, fn: self.listeners.append((name, fn))))
        modules = {
            'homeassistant': types.ModuleType('homeassistant'),
            'homeassistant.components': types.ModuleType('homeassistant.components'),
            'homeassistant.components.frontend': types.SimpleNamespace(add_extra_js_url=lambda hass, url: self.frontend_urls.append(url)),
            'homeassistant.components.http': types.SimpleNamespace(HomeAssistantView=View, StaticPathConfig=lambda *args: args),
            'homeassistant.helpers': types.ModuleType('homeassistant.helpers'),
            'homeassistant.helpers.event': types.SimpleNamespace(async_track_time_interval=lambda *args: lambda: None,
                async_track_state_change_event=lambda *args: lambda: None),
            'custom_components.quack_materials.preheat_runtime': types.SimpleNamespace(PreheatRuntime=Runtime),
        }
        patcher = patch.dict('sys.modules', modules)
        patcher.start()
        self.addCleanup(patcher.stop)

    async def setup(self, enabled=True, manual_enabled=False):
        await async_setup(self.hass, {'quack_materials': {'mode': 'demo', 'nfc_enabled': False,
            'allowed_sync_users': ['sync'], 'job_preheat_enabled': enabled,
            'manual_preheat_enabled': manual_enabled}})
        return {view.name: view for view in self.views}

    def request(self, body=None, *, user='sync', admin=False, length=100, query=None):
        class Request(dict):
            content_length = length
            async def json(self): return body
        request = Request(hass_user=types.SimpleNamespace(id=user, is_admin=admin) if user else None)
        request.query = query or {}
        return request

    def body(self, command):
        if command == 'request':
            return dict(request_key='request-1', instance_id='quack-1', printer_id='P2S-test',
                        parameters=dict(enabled=True, chamber_target=55, print_bed_target=100, preheat_bed_target=110))
        data = dict(request_key='command-1', instance_id='quack-1', version=1)
        if command == 'extend': data['seconds'] = 300
        return data

    async def test_read_is_authenticated_and_has_no_tick_or_command(self):
        views = await self.setup()
        view = views['api:quack_materials:preheat']
        self.assertTrue(view.requires_auth)
        self.assertIn('/api/quack_material_demo/preheat/{job_uuid}', view.extra_urls)
        self.assertTrue(self.runtimes[0].started)
        self.assertEqual((await view.get(self.request(user='other'), JOB))[0], 403)
        self.assertEqual((await view.get(self.request(), JOB))[0], 200)
        self.assertEqual(self.runtimes[0].status_calls, [JOB])
        self.assertEqual(self.runtimes[0].commands, [])
        self.assertEqual((await view.get(self.request(query={'tick': 'true'}), JOB))[0], 400)
        self.assertEqual((await view.get(self.request(), 'invalid'))[0], 400)

    async def test_sync_can_skip_its_attempt_but_only_admin_may_extend(self):
        views = await self.setup()
        view = views['api:quack_materials:preheat_command']
        for command in ('request', 'cancel', 'retry', 'skip'):
            self.assertEqual((await view.post(self.request(self.body(command)), JOB, command))[0], 200)
        self.assertEqual((await view.post(self.request(self.body('skip'), user='other'), JOB, 'skip'))[0], 403)
        self.assertEqual((await view.post(self.request(self.body('extend')), JOB, 'extend'))[0], 403)
        self.assertEqual((await view.post(self.request(self.body('extend'), admin=True), JOB, 'extend'))[0], 200)
        self.assertEqual(len(self.runtimes[0].commands), 5)
        self.assertEqual((await view.post(self.request(self.body('cancel'), user=None), JOB, 'cancel'))[0], 403)

    async def test_commands_reject_extra_keys_query_bad_shapes_and_oversized_bodies(self):
        views = await self.setup()
        view = views['api:quack_materials:preheat_command']
        body = self.body('cancel')
        for data in ([], {}, dict(body, service='turn_on'), dict(body, request_key='x' * 4096)):
            self.assertEqual((await view.post(self.request(data), JOB, 'cancel'))[0], 400)
        self.assertEqual((await view.post(self.request(body, length=4097), JOB, 'cancel'))[0], 400)
        self.assertEqual((await view.post(self.request(body, query={'force': '1'}), JOB, 'cancel'))[0], 400)
        self.assertEqual((await view.post(self.request(body), JOB, 'heat'))[0], 400)
        self.assertEqual((await view.post(self.request(dict(body, request_key='conflict')), JOB, 'cancel'))[0], 409)
        self.assertEqual(self.runtimes[0].commands, [])

    async def test_inventory_summaries_are_display_only_and_actions_share_runtime(self):
        views = await self.setup()
        code, data = await views['api:quack_material_demo:inventory'].get(self.request(admin=True))
        self.assertEqual(code, 200)
        self.assertEqual(data['job_preheats'][0]['ready'], False)
        self.assertEqual(self.runtimes[0].status_calls, [])
        view = views['api:quack_material_demo:action']
        self.assertEqual((await view.post(self.request({'job_uuid': JOB}, admin=True), 'set_print_status'))[0], 200)
        self.assertEqual(self.runtimes[0].dispatches, [('set_print_status', {'job_uuid': JOB})])
        await next(fn for name, fn in self.listeners if getattr(fn, '__name__', '') == 'stop')()
        self.assertTrue(self.runtimes[0].stopped)

    async def test_disabled_feature_keeps_runtime_absent_and_endpoints_fail_closed(self):
        views = await self.setup(False)
        self.assertEqual(self.runtimes, [])
        self.assertEqual((await views['api:quack_materials:preheat'].get(self.request(), JOB))[0], 409)
        code, data = await views['api:quack_material_demo:inventory'].get(self.request())
        self.assertEqual(code, 200)
        self.assertNotIn('job_preheats', data)

    async def test_feature_setting_rejects_truthy_non_boolean(self):
        with self.assertRaisesRegex(ValueError, 'job_preheat_enabled'):
            await self.setup('true')
        self.assertEqual(self.runtimes, [])


if __name__ == '__main__':
    unittest.main()
