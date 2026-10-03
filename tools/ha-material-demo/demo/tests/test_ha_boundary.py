"""Exercise HA registration/auth/observer boundaries without a live HA write."""
import asyncio
import json
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch
from custom_components.quack_material_demo import async_setup


class BoundaryTests(unittest.IsolatedAsyncioTestCase):
    async def test_authenticated_views_deny_nonadmin_edits_and_observe_without_commands(self):
        with tempfile.TemporaryDirectory() as temp:
            views, states, callbacks, tracked = [], {}, [], []
            class View:
                def json(self, data, status_code=200):
                    return status_code, data
            class Http:
                def register_view(self, view):
                    views.append(view)
                async def async_register_static_paths(self, paths):
                    self.paths = paths
            async def executor(function, *args):
                return await asyncio.to_thread(function, *args)
            def track(hass, entities, callback):
                callbacks.append(callback)
                tracked.extend(entities)
                return lambda: None
            hass = types.SimpleNamespace(data={}, http=Http(), async_add_executor_job=executor,
                config=types.SimpleNamespace(path=lambda *parts: str(Path(temp).joinpath(*parts))),
                states=types.SimpleNamespace(async_set=lambda name, value, attributes: states.update({name: attributes}),
                    get=lambda name: types.SimpleNamespace(state='finish' if name.endswith('status') else 'test-job')),
                bus=types.SimpleNamespace(async_listen_once=lambda *args: None))
            modules = {
                'homeassistant': types.ModuleType('homeassistant'),
                'homeassistant.components': types.ModuleType('homeassistant.components'),
                'homeassistant.components.http': types.SimpleNamespace(HomeAssistantView=View,
                    StaticPathConfig=lambda *args: args),
                'homeassistant.helpers': types.ModuleType('homeassistant.helpers'),
                'homeassistant.helpers.event': types.SimpleNamespace(
                    async_track_time_interval=lambda *args: lambda: None,
                    async_track_state_change_event=track),
            }
            with patch.dict('sys.modules', modules):
                self.assertTrue(await async_setup(hass, {'quack_material_demo': {
                    'printer_state_entity': 'sensor.test_status', 'printer_name_entity': 'sensor.test_name'}}))
                self.assertTrue(all(view.requires_auth for view in views))
                self.assertIn('sensor.quack_material_demo', states)
                self.assertEqual(states['sensor.quack_material_demo']['jobs'], [])
                class Request(dict):
                    async def json(self):
                        return {'slot': 'A1', 'revision': 0, 'spool_uuid': None}
                denied = await views[1].post(Request(hass_user=types.SimpleNamespace(is_admin=False)), 'assign')
                self.assertEqual(denied[0], 403)
                accepted = await views[1].post(Request(hass_user=types.SimpleNamespace(is_admin=True)), 'assign')
                self.assertEqual(accepted[0], 200)
                self.assertFalse(accepted[1]['device_command_sent'])
                self.assertEqual(len(callbacks), 1)
                self.assertEqual(tracked,['sensor.test_status','sensor.test_name'])
                denied_read=await views[0].get(Request(hass_user=types.SimpleNamespace(is_admin=False,id='other')))
                self.assertEqual(denied_read[0],403)
                settings={'mode':'pilot','database':'quack_material_inventory.sqlite3','allowed_sync_users':['sync'],'nfc_enabled':False}
                pilot_start=len(views)
                self.assertTrue(await async_setup(hass,{'quack_material_demo':settings}))
                self.assertNotIn('customers',states['sensor.quack_material_inventory'])
                self.assertNotIn('jobs',states['sensor.quack_material_inventory'])
                pilot_views=views[pilot_start:]
                sync=types.SimpleNamespace(is_admin=False,id='sync')
                inventory=await pilot_views[2].get(Request(hass_user=sync))
                self.assertEqual(inventory[0],200)
                self.assertFalse(inventory[1]['can_edit'])
                self.assertEqual(inventory[1]['spools'],[])
                class Query(Request):
                    def __init__(self, user, query):
                        super().__init__(hass_user=user)
                        self.query=query
                denied_page=await pilot_views[3].get(Query(types.SimpleNamespace(is_admin=False,id='other'),{'table':'spools'}))
                self.assertEqual(denied_page[0],403)
                page=await pilot_views[3].get(Query(sync,{'table':'spools'}))
                self.assertEqual(page[0],200)
                self.assertEqual(page[1]['rows'],[])
                self.assertEqual((await pilot_views[3].get(Query(sync,{'table':'wrong'})))[0],400)
                self.assertEqual((await pilot_views[3].get(Query(sync,{'table':'spools','revision':'999'})))[0],409)
                lite=await pilot_views[0].get(Query(sync,{'view':'provider'}))
                self.assertNotIn('native_bundle',lite[1])
                recovery=next(view for view in pilot_views if view.name=='api:quack_material_demo:recovery')
                self.assertEqual((await recovery.get(Request(hass_user=sync)))[0],403)
                exported=await recovery.get(Request(hass_user=types.SimpleNamespace(is_admin=True)))
                self.assertEqual(exported[0],200)
                self.assertEqual(exported[1]['format'],'quack-ha-recovery')
                profile_view=next(view for view in pilot_views if view.name=='api:quack_material_demo:profile')
                self.assertEqual((await profile_view.get(Query(types.SimpleNamespace(is_admin=False,id='other'),{})))[0],403)
                self.assertEqual((await profile_view.get(Query(sync,{})))[0],400)
                self.assertEqual((await pilot_views[1].post(Request(hass_user=sync),'customer'))[0],403)
                self.assertEqual((await pilot_views[1].post(Request(hass_user=types.SimpleNamespace(is_admin=True)),'start'))[0],400)
                with patch('custom_components.quack_material_demo.nfc_api.async_setup_nfc', new_callable=AsyncMock) as setup_nfc:
                    settings.pop('nfc_enabled')
                    self.assertTrue(await async_setup(hass, {'quack_material_demo': settings}))
                    setup_nfc.assert_awaited_once()
                    self.assertTrue(any(path[0] == '/quack-material-demo/nfc.js' for path in hass.http.paths))
                assignment=types.SimpleNamespace(lock=asyncio.Lock(),pending=lambda: True,reconcile_metadata=AsyncMock())
                with patch('custom_components.quack_material_demo.slot_api.async_setup_assignment',new=AsyncMock(return_value=assignment)):
                    settings['nfc_enabled']=False
                    await async_setup(hass,{'quack_material_demo':settings})
                    await asyncio.sleep(0)
                    assignment.reconcile_metadata.assert_awaited()
                action=next(view for view in reversed(views) if view.name=='api:quack_material_demo:action');store=hass.data['quack_material_demo']
                class Payload(Request):
                    async def json(self):return self['payload']
                with patch.object(store,'dispatch',return_value={'recorded':True}) as dispatch:
                    for command in ('dispatch_result','bind_external_id'):
                        response=await action.post(Payload(hass_user=sync,payload={'command':command}),'provider_job')
                        self.assertEqual(response,(200,{'recorded':True}))
                    dispatch.reset_mock()
                    request={'request_key':'accepted-before-assignment','command':'prepare_print','job_uuid':'example','data':{}}
                    self.assertEqual((await action.post(Payload(hass_user=sync,payload=request),'provider_job'))[0],409)
                    dispatch.assert_not_called()
                    with store.connection() as db:
                        db.execute('INSERT INTO receipts VALUES (?,?,?)',(request['request_key'],json.dumps(dict(action='provider_job',data=request),sort_keys=True),'{}'))
                    self.assertEqual((await action.post(Payload(hass_user=sync,payload=request),'provider_job'))[0],200)
