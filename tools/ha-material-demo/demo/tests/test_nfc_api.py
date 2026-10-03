"""Real NFC HTTP/event adapter with only HA runtime boundaries substituted."""
import asyncio
from datetime import datetime, timezone
import types
import unittest
from unittest.mock import patch

try:
    from custom_components.quack_material_demo.nfc_api import async_setup_nfc
except ImportError:
    async_setup_nfc = None

ROLL = '11111111-1111-4111-8111-111111111111'
A, B = 'a' * 64, 'b' * 64


class NfcApiTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.assertIsNotNone(async_setup_nfc, 'NFC HTTP/event integration is missing')
        self.views, self.listeners, self.modules, self.saves = [], {}, [], []
        self.entries = [
            types.SimpleNamespace(data={'user_id': 'user', 'device_id': 'app-a', 'device_name': 'Phone A'}),
            types.SimpleNamespace(data={'user_id': 'user', 'device_id': 'app-b', 'device_name': 'Phone B'}),
            types.SimpleNamespace(data={'user_id': 'other', 'device_id': 'app-c', 'device_name': 'Other phone'}),
        ]
        self.registry = {'app-a': types.SimpleNamespace(id='phone-a', name='Phone A', name_by_user=None),
                         'app-b': types.SimpleNamespace(id='phone-b', name='Phone B', name_by_user=None),
                         'app-c': types.SimpleNamespace(id='phone-c', name='Other phone', name_by_user=None)}
        parent = self
        class Storage:
            def __init__(self, *args): pass
            async def async_load(self): return None
            async def async_save(self, data):
                if getattr(parent, 'fail_save', False): raise OSError('disk failure')
                parent.saves.append(data)
        class View:
            def json(self, data, status_code=200): return status_code, data
        async def executor(fn, *args): return fn(*args)
        self.hass = types.SimpleNamespace(
            data={}, http=types.SimpleNamespace(register_view=self.views.append),
            config_entries=types.SimpleNamespace(async_entries=lambda domain: self.entries),
            async_add_executor_job=executor,
            bus=types.SimpleNamespace(
                async_listen=lambda name, fn: (self.listeners.update({name: fn}) or (lambda: None)),
                async_listen_once=lambda *args: None))
        self.store = types.SimpleNamespace(management_snapshot=lambda: {'spools': [{'uuid': ROLL, 'status': 'active'}]})
        modules = {
            'homeassistant': types.ModuleType('homeassistant'),
            'homeassistant.components': types.ModuleType('homeassistant.components'),
            'homeassistant.components.http': types.SimpleNamespace(HomeAssistantView=View),
            'homeassistant.components.frontend': types.SimpleNamespace(add_extra_js_url=lambda hass, url: self.modules.append(url)),
            'homeassistant.helpers': types.ModuleType('homeassistant.helpers'),
            'homeassistant.helpers.storage': types.SimpleNamespace(Store=Storage),
            'homeassistant.helpers.device_registry': types.SimpleNamespace(async_get=lambda hass:
                types.SimpleNamespace(async_get_device=lambda identifiers: self.registry.get(next(iter(identifiers))[1]))),
        }
        self.patcher = patch.dict('sys.modules', modules)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)
        await async_setup_nfc(self.hass, self.store)
        self.view = self.views[0]

    async def call(self, action, key=A, user='user', admin=True, **data):
        class Request(dict):
            content_length = 300
            async def json(self): return {'client_key': key, **data}
        return await self.view.post(Request(hass_user=types.SimpleNamespace(id=user, is_admin=admin)), action)

    async def scan(self, roll=ROLL, device='phone-a', user='user'):
        await self.listeners['tag_scanned'](types.SimpleNamespace(
            data={'tag_id': roll, 'device_id': device}, time_fired=datetime.now(timezone.utc),
            context=types.SimpleNamespace(id='event-'+roll, user_id=user)))

    async def test_devices_are_owner_filtered_and_admin_only(self):
        self.assertTrue(self.view.requires_auth)
        code, result = await self.call('devices')
        self.assertEqual(code, 200)
        self.assertEqual({r['id'] for r in result['devices']}, {'phone-a', 'phone-b'})
        self.assertEqual((await self.call('devices', admin=False))[0], 403)
        self.assertEqual((await self.call('bind', device_id='phone-c', confirmed=True))[0], 403)
        self.assertEqual((await self.call('bind', device_id='phone-a', confirmed=False))[0], 400)

    async def test_scan_is_delivered_to_linked_phone_and_acknowledged_without_inventory_writes(self):
        await self.call('bind', device_id='phone-a', confirmed=True)
        await self.call('bind', key=B, device_id='phone-b', confirmed=True)
        await self.scan()
        code, result = await self.call('poll')
        self.assertEqual(code, 200)
        self.assertEqual(result['scan']['spool_uuid'], ROLL)
        self.assertGreater(result['scan']['expires_in'], 0)
        self.assertIsNone((await self.call('poll', key=B))[1]['scan'])
        await self.call('ack', request_id=result['scan']['id'])
        self.assertIsNone((await self.call('poll'))[1]['scan'])
        self.assertNotIn(A, str(self.saves))
        self.assertTrue(any('/nfc.js?' in url for url in self.modules))

    async def test_unrelated_tags_and_wrong_event_owner_do_not_navigate(self):
        await self.call('bind', device_id='phone-a', confirmed=True)
        await self.scan(roll='00000000-0000-4000-8000-000000000000')
        await self.scan(user='other')
        self.assertIsNone((await self.call('poll'))[1]['scan'])

    async def test_deleted_or_reassigned_device_loses_routing_access(self):
        await self.call('bind', device_id='phone-a', confirmed=True)
        await self.scan()
        self.entries[0].data['user_id'] = 'other'
        self.assertFalse((await self.call('poll'))[1]['paired'])
        self.assertFalse(any(row['device_id'] == 'phone-a' for row in self.saves[-1]))

    async def test_failed_persistence_does_not_claim_successful_pairing(self):
        self.fail_save = True
        self.assertEqual((await self.call('bind', device_id='phone-a', confirmed=True))[0], 503)
        self.assertFalse((await self.call('poll'))[1]['paired'])
